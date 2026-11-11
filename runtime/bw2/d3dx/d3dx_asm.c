/*
 * d3dx_asm.c - the D3DX shader assembler (plan 5 Task 2): D3D9 shader
 * assembly text to the bytecode sm_decode reads, and D3DXAssembleShader,
 * D3DXGetShaderVersion and the shader profiles.
 *
 * The text: comments (; // and block comments), a small C preprocessor
 * (#define of object-like macros, #undef, #if/#ifdef/#ifndef/#elif/#else/
 * #endif, #error; #include stops the run), a vs_x_y / ps_x_y version line,
 * then one instruction per line (a leading + co-issues it). The output is
 * then checked by sm_decode, so the version, register and modifier rules
 * live in one place; its errors name the line they came from.
 */
#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "d3d_com.h"
#include "d3dx.h"
#include "sm.h"

#define E_FAIL_ 0x80004005u
#define MAX_ERRORS 64

/* a growable string */
struct sb { char *s; size_t n, cap; };
static void sb_addn(struct sb *b, const char *s, size_t n) {
    if (b->n + n + 1 > b->cap) {
        b->cap = (b->n + n + 1) * 2 + 64;
        b->s = realloc(b->s, b->cap);
    }
    memcpy(b->s + b->n, s, n);
    b->n += n;
    b->s[b->n] = 0;
}
static void sb_add(struct sb *b, const char *s) { sb_addn(b, s, strlen(s)); }

struct macro { char *name, *value; };
struct cond { int parent, active, taken, seen_else, line; };

struct as {
    struct sb errs;
    int nerr;
    struct macro *macros;
    int nmacros, capmacros;
    /* preprocessed lines */
    char **lines;
    int *lineno;
    int nlines;
    /* output */
    uint32_t *tok;
    uint32_t ntok, captok;
    uint32_t *ins_at;                       /* per instruction: first token, line */
    int *ins_line;
    uint32_t nins, capins;
    uint32_t ver;
    int ps;
    int ver_line;
};

static void error(struct as *a, int line, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
static void error(struct as *a, int line, const char *fmt, ...) {
    if (a->nerr++ >= MAX_ERRORS) return;
    char msg[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    char head[32];
    snprintf(head, sizeof head, "(%d): error: ", line);
    sb_add(&a->errs, head);
    sb_add(&a->errs, msg);
    sb_add(&a->errs, "\n");
}

/* ---- comments ---- */

/* Split text into lines with comments blanked; a block comment keeps its newlines. */
static char **strip_comments(struct as *a, const char *text, size_t len, int *nlines) {
    int cap = 64, n = 0;
    char **lines = malloc(sizeof(char *) * (size_t)cap);
    struct sb cur = { 0 };
    sb_addn(&cur, "", 0);
    int line = 1, in_block = 0, block_line = 0;
    for (size_t i = 0; i <= len; i++) {
        char c = i < len ? text[i] : '\n';
        if (i == len || c == '\n') {
            if (n == cap) lines = realloc(lines, sizeof(char *) * (size_t)(cap *= 2));
            lines[n++] = cur.s;
            cur = (struct sb){ 0 };
            sb_addn(&cur, "", 0);
            line++;
            if (i == len) break;
            continue;
        }
        if (in_block) {
            if (c == '*' && i + 1 < len && text[i + 1] == '/') { in_block = 0, i++; sb_add(&cur, " "); }
            continue;
        }
        if (c == '/' && i + 1 < len && text[i + 1] == '*') { in_block = 1, block_line = line, i++; continue; }
        if ((c == '/' && i + 1 < len && text[i + 1] == '/') || c == ';') {
            while (i + 1 < len && text[i + 1] != '\n') i++;
            continue;
        }
        if (c == '\r' || c == '\t' || c == '\f' || c == '\v') c = ' ';
        sb_addn(&cur, &c, 1);
    }
    free(cur.s);
    if (in_block) error(a, block_line, "a block comment is not closed");
    *nlines = n;
    return lines;
}

/* ---- the preprocessor ---- */

static struct macro *find_macro(struct as *a, const char *name, size_t n) {
    for (int i = a->nmacros - 1; i >= 0; i--)
        if (strlen(a->macros[i].name) == n && !memcmp(a->macros[i].name, name, n)) return &a->macros[i];
    return NULL;
}
static void undef_macro(struct as *a, const char *name) {
    struct macro *m;
    while ((m = find_macro(a, name, strlen(name)))) {
        free(m->name), free(m->value);
        *m = a->macros[--a->nmacros];
    }
}
static void define_macro(struct as *a, const char *name, const char *value) {
    undef_macro(a, name);
    if (a->nmacros == a->capmacros) a->macros = realloc(a->macros, sizeof *a->macros * (size_t)(a->capmacros = a->capmacros * 2 + 8));
    a->macros[a->nmacros].name = strdup(name);
    a->macros[a->nmacros++].value = strdup(value);
}

static int ident_start(char c) { return isalpha((unsigned char)c) || c == '_'; }
static int ident_char(char c) { return isalnum((unsigned char)c) || c == '_'; }

/* Expand macros in s into out. active: the macros being expanded (no recursion into them). */
static void expand(struct as *a, const char *s, struct sb *out, const struct macro **active, int nactive, int defined_ops) {
    for (size_t i = 0; s[i];) {
        if (ident_start(s[i])) {
            size_t j = i;
            while (ident_char(s[j])) j++;
            if (defined_ops && j - i == 7 && !memcmp(s + i, "defined", 7)) {
                size_t k = j;
                while (s[k] == ' ') k++;
                int paren = s[k] == '(';
                if (paren) { k++; while (s[k] == ' ') k++; }
                size_t n0 = k;
                while (ident_char(s[k])) k++;
                size_t n1 = k;
                while (paren && s[k] == ' ') k++;
                if (n1 == n0 || (paren && s[k] != ')')) { sb_add(out, "@"); i = j; continue; }   /* an invalid expression */
                if (paren) k++;
                sb_add(out, find_macro(a, s + n0, n1 - n0) ? " 1 " : " 0 ");
                i = k;
                continue;
            }
            struct macro *m = find_macro(a, s + i, j - i);
            int busy = 0;
            for (int k = 0; m && k < nactive; k++) busy |= active[k] == m;
            if (m && !busy && nactive < 64) {
                const struct macro *act[65];
                memcpy(act, active, sizeof *act * (size_t)nactive);
                act[nactive] = m;
                expand(a, m->value, out, act, nactive + 1, defined_ops);
            } else sb_addn(out, s + i, j - i);
            i = j;
        } else if (isdigit((unsigned char)s[i]) || (s[i] == '.' && isdigit((unsigned char)s[i + 1]))) {
            size_t j = i;                                        /* a pp-number: never expanded */
            while (ident_char(s[j]) || s[j] == '.' ||
                   ((s[j] == '+' || s[j] == '-') && (s[j - 1] == 'e' || s[j - 1] == 'E'))) j++;
            sb_addn(out, s + i, j - i);
            i = j;
        } else sb_addn(out, s + i++, 1);
    }
}

/* #if expressions: C's integer operators over long long */
struct ex { const char *p; int bad; };
static void ex_ws(struct ex *e) { while (*e->p == ' ') e->p++; }
static int ex_is(struct ex *e, const char *op) {
    ex_ws(e);
    size_t n = strlen(op);
    if (strncmp(e->p, op, n)) return 0;
    /* "&" is not "&&", "<" is not "<<" or "<=" ... */
    if (n == 1 && (op[0] == '&' || op[0] == '|' || op[0] == '<' || op[0] == '>' || op[0] == '=' || op[0] == '!') &&
        (e->p[1] == op[0] || e->p[1] == '=')) return 0;
    if (n == 2 && (op[0] == '<' || op[0] == '>') && op[1] == op[0] && e->p[2] == '=') return 0;
    e->p += n;
    return 1;
}
static long long ex_cond(struct ex *e);
static long long ex_unary(struct ex *e) {
    ex_ws(e);
    if (ex_is(e, "!")) return !ex_unary(e);
    if (ex_is(e, "-")) return -ex_unary(e);
    if (ex_is(e, "+")) return ex_unary(e);
    if (ex_is(e, "~")) return ~ex_unary(e);
    if (ex_is(e, "(")) {
        long long v = ex_cond(e);
        if (!ex_is(e, ")")) e->bad = 1;
        return v;
    }
    if (isdigit((unsigned char)*e->p)) {
        char *end;
        long long v = strtoll(e->p, &end, 0);
        while (*end == 'u' || *end == 'U' || *end == 'l' || *end == 'L') end++;
        if (ident_char(*end) || *end == '.') e->bad = 1;
        e->p = end;
        return v;
    }
    if (ident_start(*e->p)) {                                    /* a name left after expansion is 0 */
        while (ident_char(*e->p)) e->p++;
        return 0;
    }
    e->bad = 1;
    return 0;
}
#define EX_LEVEL(name, sub, ...) \
    static long long name(struct ex *e) { \
        long long v = sub(e); \
        for (;;) { \
            const char *const ops[] = { __VA_ARGS__, NULL }; \
            int k = 0; \
            while (ops[k] && !ex_is(e, ops[k])) k++; \
            if (!ops[k]) return v; \
            long long r = sub(e); \
            v = name##_apply(e, k, v, r); \
        } \
    }
static long long ex_mul_apply(struct ex *e, int k, long long v, long long r) {
    if (k && !r) { e->bad = 1; return 0; }
    return k == 0 ? v * r : k == 1 ? v / r : v % r;
}
EX_LEVEL(ex_mul, ex_unary, "*", "/", "%")
static long long ex_add_apply(struct ex *e, int k, long long v, long long r) { (void)e; return k ? v - r : v + r; }
EX_LEVEL(ex_add, ex_mul, "+", "-")
static long long ex_shift_apply(struct ex *e, int k, long long v, long long r) {
    if (r < 0 || r > 63) { e->bad = 1; return 0; }
    return k ? v >> r : (long long)((unsigned long long)v << r);
}
EX_LEVEL(ex_shift, ex_add, "<<", ">>")
static long long ex_rel_apply(struct ex *e, int k, long long v, long long r) {
    (void)e;
    return k == 0 ? v <= r : k == 1 ? v >= r : k == 2 ? v < r : v > r;
}
EX_LEVEL(ex_rel, ex_shift, "<=", ">=", "<", ">")
static long long ex_eq_apply(struct ex *e, int k, long long v, long long r) { (void)e; return k ? v != r : v == r; }
EX_LEVEL(ex_eq, ex_rel, "==", "!=")
static long long ex_band_apply(struct ex *e, int k, long long v, long long r) { (void)e, (void)k; return v & r; }
EX_LEVEL(ex_band, ex_eq, "&")
static long long ex_bxor_apply(struct ex *e, int k, long long v, long long r) { (void)e, (void)k; return v ^ r; }
EX_LEVEL(ex_bxor, ex_band, "^")
static long long ex_bor_apply(struct ex *e, int k, long long v, long long r) { (void)e, (void)k; return v | r; }
EX_LEVEL(ex_bor, ex_bxor, "|")
static long long ex_land_apply(struct ex *e, int k, long long v, long long r) { (void)e, (void)k; return v && r; }
EX_LEVEL(ex_land, ex_bor, "&&")
static long long ex_lor_apply(struct ex *e, int k, long long v, long long r) { (void)e, (void)k; return v || r; }
EX_LEVEL(ex_lor, ex_land, "||")
static long long ex_cond(struct ex *e) {
    long long c = ex_lor(e);
    if (!ex_is(e, "?")) return c;
    long long t = ex_cond(e);
    if (!ex_is(e, ":")) e->bad = 1;
    long long f = ex_cond(e);
    return c ? t : f;
}

/* 1/0, or -1 for an invalid expression */
static int eval_if(struct as *a, const char *expr) {
    struct sb x = { 0 };
    sb_addn(&x, "", 0);
    expand(a, expr, &x, NULL, 0, 1);
    struct ex e = { x.s, 0 };
    ex_ws(&e);
    long long v = *e.p ? ex_cond(&e) : (e.bad = 1, 0);
    ex_ws(&e);
    int r = e.bad || *e.p ? -1 : v != 0;
    free(x.s);
    return r;
}

static const char *skip_ws(const char *s) { while (*s == ' ') s++; return s; }
static char *trim_copy(const char *s) {
    s = skip_ws(s);
    size_t n = strlen(s);
    while (n && s[n - 1] == ' ') n--;
    char *r = malloc(n + 1);
    memcpy(r, s, n);
    r[n] = 0;
    return r;
}

/* the directive on a line (after '#'); returns its name length */
static void preprocess(struct as *a, char **raw, int nraw) {
    struct cond stack[64];
    int depth = 0;
    a->lines = malloc(sizeof(char *) * (size_t)(nraw + 1));
    a->lineno = malloc(sizeof(int) * (size_t)(nraw + 1));
    for (int li = 0; li < nraw; li++) {
        int line = li + 1, active = depth ? stack[depth - 1].active : 1;
        const char *s = skip_ws(raw[li]);
        if (*s != '#') {
            if (!active || !*s) continue;
            struct sb x = { 0 };
            sb_addn(&x, "", 0);
            expand(a, s, &x, NULL, 0, 0);
            a->lines[a->nlines] = x.s, a->lineno[a->nlines++] = line;
            continue;
        }
        s = skip_ws(s + 1);
        size_t n = 0;
        while (ident_char(s[n])) n++;
        char dir[16] = "";
        snprintf(dir, sizeof dir, "%.*s", (int)(n < 15 ? n : 15), s);
        const char *rest = skip_ws(s + n);
        if (!strcmp(dir, "if") || !strcmp(dir, "ifdef") || !strcmp(dir, "ifndef")) {
            if (depth == 64) { error(a, line, "#if nesting is too deep"); continue; }
            int v = 0;
            if (active) {
                if (dir[2] == 0) {
                    v = eval_if(a, rest);
                    if (v < 0) { error(a, line, "invalid #if expression '%s'", rest); v = 0; }
                } else {
                    size_t k = 0;
                    while (ident_char(rest[k])) k++;
                    if (!k || *skip_ws(rest + k)) error(a, line, "#%s takes one name", dir);
                    v = (find_macro(a, rest, k) != NULL) == (dir[2] == 'd');
                }
            }
            stack[depth++] = (struct cond){ active, active && v, active && v, 0, line };
        } else if (!strcmp(dir, "elif")) {
            if (!depth) { error(a, line, "#elif without #if"); continue; }
            struct cond *c = &stack[depth - 1];
            if (c->seen_else) error(a, line, "#elif after #else");
            int v = 0;
            if (c->parent && !c->taken) {
                v = eval_if(a, rest);
                if (v < 0) { error(a, line, "invalid #elif expression '%s'", rest); v = 0; }
            }
            c->active = c->parent && !c->taken && v;
            c->taken |= c->active;
        } else if (!strcmp(dir, "else")) {
            if (!depth) { error(a, line, "#else without #if"); continue; }
            struct cond *c = &stack[depth - 1];
            if (c->seen_else) error(a, line, "a second #else");
            c->seen_else = 1;
            c->active = c->parent && !c->taken;
            c->taken = 1;
        } else if (!strcmp(dir, "endif")) {
            if (!depth) { error(a, line, "#endif without #if"); continue; }
            depth--;
        } else if (!active) {
            continue;                                         /* other directives in a skipped block */
        } else if (!strcmp(dir, "define")) {
            size_t k = 0;
            while (ident_char(rest[k])) k++;
            if (!k || !ident_start(rest[0])) { error(a, line, "#define needs a name"); continue; }
            char *name = strndup(rest, k);
            if (rest[k] == '(') error(a, line, "function-like macro %s is not supported", name);
            else {
                char *value = trim_copy(rest + k);
                define_macro(a, name, value);
                free(value);
            }
            free(name);
        } else if (!strcmp(dir, "undef")) {
            size_t k = 0;
            while (ident_char(rest[k])) k++;
            if (!k) { error(a, line, "#undef needs a name"); continue; }
            char *name = strndup(rest, k);
            undef_macro(a, name);
            free(name);
        } else if (!strcmp(dir, "include")) {
            char *file = trim_copy(rest);
            size_t fl = strlen(file);
            if (fl >= 2 && (file[0] == '"' || file[0] == '<')) { memmove(file, file + 1, fl - 2); file[fl - 2] = 0; }
            d3d_fail("shader assembler: #include \"%s\" (line %d) is not supported yet", file, line);
        } else if (!strcmp(dir, "error")) {
            error(a, line, "#error %s", rest);
        } else if (!*dir && !*rest) {
            continue;                                         /* a lone '#' */
        } else {
            error(a, line, "unsupported directive #%s", dir);
        }
    }
    if (depth) error(a, stack[depth - 1].line, "#if without #endif");
}

/* ---- instructions ---- */

static void emit(struct as *a, uint32_t t) {
    if (a->ntok == a->captok) a->tok = realloc(a->tok, sizeof *a->tok * (a->captok = a->captok * 2 + 64));
    a->tok[a->ntok++] = t;
}
static uint32_t reg_bits(uint32_t type) { return (type & 7u) << 28 | (type & 0x18u) << 8; }

struct operand {
    struct sm_reg r;
    int ok;
};

static int lower_eq(const char *s, size_t n, const char *word) { return strlen(word) == n && !strncasecmp(s, word, n); }

/* A register name at *p: type and index (index -1: none written, as in c[a0.x]). */
static int parse_regname(struct as *a, int line, const char **p, struct sm_reg *r, int *has_index) {
    const char *s = *p;
    size_t n = 0;
    while (ident_char(s[n])) n++;
    /* the name proper stops at a modifier's '_' */
    size_t base = 0;
    while (base < n && s[base] != '_') base++;
    static const struct { const char *name; uint8_t type; uint16_t index; } named[] = {
        { "opos", SM_R_RASTOUT, 0 }, { "ofog", SM_R_RASTOUT, 1 }, { "opts", SM_R_RASTOUT, 2 },
        { "odepth", SM_R_DEPTHOUT, 0 }, { "al", SM_R_LOOP, 0 }, { "vpos", SM_R_MISCTYPE, 0 },
        { "vface", SM_R_MISCTYPE, 1 },
    };
    for (size_t k = 0; k < sizeof named / sizeof *named; k++)
        if (lower_eq(s, base, named[k].name)) {
            r->type = named[k].type, r->index = named[k].index, *has_index = 1;
            *p = s + base;
            return 0;
        }
    size_t letters = 0;
    while (letters < base && isalpha((unsigned char)s[letters])) letters++;
    char pre[4] = "";
    if (letters >= 1 && letters <= 2) for (size_t k = 0; k < letters; k++) pre[k] = (char)tolower((unsigned char)s[k]);
    int digits = letters < base;
    for (size_t k = letters; k < base; k++) digits &= isdigit((unsigned char)s[k]) != 0;
    int type = -1;
    if (letters == 1) {
        switch (pre[0]) {
        case 'r': type = SM_R_TEMP; break;
        case 'v': type = SM_R_INPUT; break;
        case 'c': type = SM_R_CONST; break;
        case 'a': type = a->ps ? -2 : SM_R_ADDR; break;
        case 't': type = a->ps ? SM_R_TEXTURE : -2; break;
        case 's': type = SM_R_SAMPLER; break;
        case 'i': type = SM_R_CONSTINT; break;
        case 'b': type = SM_R_CONSTBOOL; break;
        case 'p': type = SM_R_PREDICATE; break;
        case 'l': type = SM_R_LABEL; break;
        case 'o': type = SM_R_TEXCRDOUT; break;
        }
    } else if (letters == 2 && pre[0] == 'o') {
        switch (pre[1]) {
        case 'd': type = SM_R_ATTROUT; break;
        case 't': type = SM_R_TEXCRDOUT; break;
        case 'c': type = SM_R_COLOROUT; break;
        }
    }
    /* "c[...]": a constant with the index inside the brackets */
    if (type == SM_R_CONST && base == 1 && s[1] == '[') { r->type = SM_R_CONST, r->index = 0, *has_index = 0; *p = s + 1; return 0; }
    if (type == -2) { error(a, line, "register '%.*s' is not valid in a %s shader", (int)base, s, a->ps ? "pixel" : "vertex"); return -1; }
    if (type < 0 || !digits) { error(a, line, "unknown register '%.*s'", (int)(n ? n : 1), s); return -1; }
    long idx = strtol(s + letters, NULL, 10);
    if (idx > 0x7FF) { error(a, line, "register '%.*s' has too large an index", (int)base, s); return -1; }
    r->type = (uint8_t)type, r->index = (uint16_t)idx, *has_index = 1;
    *p = s + base;
    return 0;
}

static int comp_of(char c);

/* [a0.x + n] after a register: one of a0, a0.<c> or aL, and integers, joined by + (or - before a number) */
static int parse_rel(struct as *a, int line, const char **p, struct sm_reg *r, int has_index) {
    const char *s = *p + 1, *close = strchr(s, ']'), *q = s;
    if (!close) { error(a, line, "'[' without ']'"); return -1; }
    long off = has_index ? r->index : 0;
    int have_reg = 0, sign = 1, want_term = 1, bad = 0;
    while (!bad) {
        q = skip_ws(q);
        if (q >= close) break;
        if (*q == '+' || *q == '-') {
            bad = want_term, sign = *q == '-' ? -1 : 1, want_term = 1, q++;
            continue;
        }
        if (!want_term) { bad = 1; break; }
        if (isdigit((unsigned char)*q)) {
            char *e;
            off += sign * strtol(q, &e, 10);
            q = e;
        } else if (ident_start(*q)) {
            const char *b0 = q;
            while (q < close && (ident_char(*q) || *q == '.')) q++;
            size_t n = (size_t)(q - b0);
            if (have_reg || sign < 0) bad = 1;
            else if (lower_eq(b0, n, "al")) r->rel_type = SM_R_LOOP, r->rel_comp = 0;
            else if ((n == 2 || (n == 4 && b0[2] == '.' && comp_of(b0[3]) >= 0)) && tolower((unsigned char)b0[0]) == 'a' &&
                     b0[1] == '0')
                r->rel_type = SM_R_ADDR, r->rel_comp = (uint8_t)(n == 2 ? 0 : comp_of(b0[3]));
            else { error(a, line, "relative addressing uses a0 or aL, not '%.*s'", (int)n, b0); return -1; }
            have_reg = 1;
        } else bad = 1;
        want_term = 0, sign = 1;
    }
    if (bad || want_term) { error(a, line, "invalid relative address '[%.*s]'", (int)(close - s), s); return -1; }
    if (!have_reg) { error(a, line, "relative address without a0 or aL"); return -1; }
    if (off < 0 || off > 0x7FF) { error(a, line, "relative address offset %ld is out of range", off); return -1; }
    if ((a->ver & 0xFFFF) < 0x200 && (r->rel_type != SM_R_ADDR || r->rel_comp != 0)) {
        error(a, line, "relative addressing in vs_1_1 uses a0.x");
        return -1;
    }
    r->rel = 1, r->rel_index = 0, r->index = (uint16_t)off;
    *p = close + 1;
    return 0;
}

static int comp_of(char c) {
    const char *x = strchr("xyzw", tolower((unsigned char)c)), *g = strchr("rgba", tolower((unsigned char)c));
    return c && x ? (int)(x - "xyzw") : c && g ? (int)(g - "rgba") : -1;
}

static int parse_dst(struct as *a, int line, const char *text, struct sm_reg *r) {
    memset(r, 0, sizeof *r);
    const char *p = text;
    int has_index;
    if (parse_regname(a, line, &p, r, &has_index)) return -1;
    if (*p == '[') { if (parse_rel(a, line, &p, r, has_index)) return -1; }
    else if (!has_index) { error(a, line, "register 'c' without an index"); return -1; }
    r->mask_or_swizzle = 0xF;
    if (*p == '.') {
        const char *m = ++p;
        int mask = 0, last = -1, ok = 1;
        while (ident_char(*p)) p++;
        for (const char *q = m; q < p; q++) {
            int c = comp_of(*q);
            if (c <= last) ok = 0;
            else mask |= 1 << c, last = c;
        }
        if (!ok || p == m || p - m > 4) { error(a, line, "invalid write mask '.%.*s'", (int)(p - m), m); return -1; }
        r->mask_or_swizzle = (uint8_t)mask;
    }
    p = skip_ws(p);
    if (*p) { error(a, line, "unexpected '%s' after destination '%s'", p, text); return -1; }
    return 0;
}

static int parse_src(struct as *a, int line, const char *text, struct sm_reg *r) {
    memset(r, 0, sizeof *r);
    const char *p = skip_ws(text);
    int neg = 0, comp = 0, mod = -1;
    if (*p == '-') neg = 1, p = skip_ws(p + 1);
    if (p[0] == '1' && *skip_ws(p + 1) == '-') { comp = 1; p = skip_ws(skip_ws(p + 1) + 1); }
    int has_index;
    if (parse_regname(a, line, &p, r, &has_index)) return -1;
    if (*p == '[') { if (parse_rel(a, line, &p, r, has_index)) return -1; }
    else if (!has_index) { error(a, line, "register 'c' without an index"); return -1; }
    r->mask_or_swizzle = SM_SWIZZLE_IDENTITY;
    int have_swz = 0;
    for (;;) {
        if (*p == '_' && mod < 0) {
            const char *m = ++p;
            while (ident_char(*p) && *p != '.') p++;
            size_t n = (size_t)(p - m);
            if (lower_eq(m, n, "bias")) mod = SM_SRC_BIAS;
            else if (lower_eq(m, n, "bx2")) mod = SM_SRC_SIGN;
            else if (lower_eq(m, n, "x2")) mod = SM_SRC_X2;
            else if (lower_eq(m, n, "dz") || lower_eq(m, n, "db")) mod = SM_SRC_DZ;
            else if (lower_eq(m, n, "dw") || lower_eq(m, n, "da")) mod = SM_SRC_DW;
            else if (lower_eq(m, n, "abs")) mod = SM_SRC_ABS;
            else { error(a, line, "unknown source modifier '_%.*s'", (int)n, m); return -1; }
        } else if (*p == '.' && !have_swz) {
            const char *m = ++p;
            while (ident_char(*p) && *p != '_') p++;
            size_t n = (size_t)(p - m);
            int c[4], ok = n >= 1 && n <= 4;
            for (size_t k = 0; ok && k < n; k++) ok = (c[k] = comp_of(m[k])) >= 0;
            if (!ok) { error(a, line, "invalid swizzle '.%.*s'", (int)n, m); return -1; }
            for (size_t k = n; k < 4; k++) c[k] = c[n - 1];
            r->mask_or_swizzle = (uint8_t)(c[0] | c[1] << 2 | c[2] << 4 | c[3] << 6);
            have_swz = 1;
        } else break;
    }
    p = skip_ws(p);
    if (*p) { error(a, line, "unexpected '%s' in source '%s'", p, text); return -1; }
    if (comp && (neg || mod >= 0)) { error(a, line, "'1-' does not combine with other modifiers in '%s'", text); return -1; }
    if (comp) r->mod = SM_SRC_COMP;
    else if (mod < 0) r->mod = neg ? SM_SRC_NEG : SM_SRC_NONE;
    else if (!neg) r->mod = (uint8_t)mod;
    else if (mod == SM_SRC_BIAS) r->mod = SM_SRC_BIASNEG;
    else if (mod == SM_SRC_SIGN) r->mod = SM_SRC_SIGNNEG;
    else if (mod == SM_SRC_X2) r->mod = SM_SRC_X2NEG;
    else if (mod == SM_SRC_ABS) r->mod = SM_SRC_ABSNEG;
    else { error(a, line, "'-' does not combine with _dz/_dw in '%s'", text); return -1; }
    return 0;
}

static uint32_t dst_token(const struct sm_reg *r) {
    return 0x80000000u | reg_bits(r->type) | r->index | (uint32_t)r->mask_or_swizzle << 16 |
           (uint32_t)(r->mod & 0xF) << 20 | ((uint32_t)r->shift & 0xFu) << 24 | (r->rel ? 0x2000u : 0);
}
static void emit_src(struct as *a, const struct sm_reg *r) {
    emit(a, 0x80000000u | reg_bits(r->type) | r->index | (uint32_t)r->mask_or_swizzle << 16 |
            (uint32_t)(r->mod & 0xF) << 24 | (r->rel ? 0x2000u : 0));
    if (r->rel && (a->ver & 0xFFFF) >= 0x200) {
        uint32_t swz = r->rel_type == SM_R_LOOP ? SM_SWIZZLE_IDENTITY : r->rel_comp * 0x55u;
        emit(a, 0x80000000u | reg_bits(r->rel_type) | r->rel_index | swz << 16);
    }
}

/* split the operands at commas, trimmed; returns the count */
static int split_operands(const char *s, char **ops, int max) {
    int n = 0;
    s = skip_ws(s);
    if (!*s) return 0;
    for (;;) {
        const char *e = strchr(s, ',');
        size_t len = e ? (size_t)(e - s) : strlen(s);
        char *t = strndup(s, len);
        if (n < max) ops[n] = trim_copy(t);
        n++;
        free(t);
        if (!e) break;
        s = e + 1;
    }
    return n;
}

static const char *const g_usage[] = { "position", "blendweight", "blendindices", "normal", "psize", "texcoord",
    "tangent", "binormal", "tessfactor", "positiont", "color", "fog", "depth", "sample" };

/* the version line: vs_1_1 or vs.1.1 (x for 2_x, sw) */
static int parse_version(const char *w, uint32_t *ver) {
    size_t n = strlen(w);
    if (n < 6 || (strncasecmp(w, "vs", 2) && strncasecmp(w, "ps", 2)) || (w[2] != '_' && w[2] != '.')) return 0;
    if (!isdigit((unsigned char)w[3]) || w[4] != w[2]) return 0;
    uint32_t major = (uint32_t)(w[3] - '0'), minor;
    const char *m = w + 5;
    if (isdigit((unsigned char)m[0]) && !m[1]) minor = (uint32_t)(m[0] - '0');
    else if (!strcasecmp(m, "x")) minor = 1;
    else if (!strcasecmp(m, "sw")) minor = 0xFF;
    else return 0;
    *ver = (tolower((unsigned char)w[0]) == 'v' ? 0xFFFE0000u : 0xFFFF0000u) | major << 8 | minor;
    return 1;
}

static void assemble_line(struct as *a, int line, const char *text) {
    const char *p = skip_ws(text);
    int coissue = 0;
    if (*p == '+') coissue = 1, p = skip_ws(p + 1);
    size_t n = 0;
    while (ident_char(p[n]) || p[n] == '.') n++;
    if (!n) { error(a, line, "expected an instruction, got '%s'", p); return; }
    char word[64];
    snprintf(word, sizeof word, "%.*s", (int)(n < 63 ? n : 63), p);
    for (char *q = word; *q; q++) *q = (char)tolower((unsigned char)*q);
    const char *rest = p + n;
    uint32_t v;
    if (parse_version(word, &v)) { error(a, line, "a second shader version '%s'", word); return; }
    /* mnemonic and modifiers */
    char *mods[8];
    int nmods = 0;
    char *us = strchr(word, '_');
    if (us) {
        *us = 0;
        for (char *m = us + 1; m && nmods < 8;) {
            char *e = strchr(m, '_');
            if (e) *e = 0;
            mods[nmods++] = m;
            m = e ? e + 1 : NULL;
        }
    }
    const char *base = word;
    uint32_t control = 0;
    if (!strcmp(base, "texldp")) base = "texld", control = SM_TEXLD_PROJECT;
    else if (!strcmp(base, "texldb")) base = "texld", control = SM_TEXLD_BIAS;
    char vb[16];
    const struct sm_opinfo *oi = sm_opinfo_named(base, a->ver);
    if (!oi) {
        if (sm_op_known(base)) error(a, line, "instruction '%s' is not valid in %s", word, sm_version_name(a->ver, vb, sizeof vb));
        else error(a, line, "unknown instruction '%s'", word);
        return;
    }
    if (control && (a->ver & 0xFFFF) < 0x200) { error(a, line, "instruction '%sp/b' is not valid here", base); return; }
    /* modifiers */
    int sat = 0, pp = 0, centroid = 0, shift = 0, k0 = 0;
    uint32_t usage = 0, usage_index = 0, stt = 0;
    if (oi->op == SM_DCL && nmods) {
        char *m = mods[0];
        size_t ml = 0;
        while (isalpha((unsigned char)m[ml])) ml++;
        int found = 0;
        for (uint32_t u = 0; u < sizeof g_usage / sizeof *g_usage; u++)
            if (lower_eq(m, ml, g_usage[u])) {
                char *end;
                usage = u, found = 1;
                if (m[ml]) {
                    usage_index = (uint32_t)strtoul(m + ml, &end, 10);
                    if (*end || usage_index > 15) found = 0;
                }
            }
        if (!strcmp(m, "2d")) stt = SM_STT_2D, found = 1;
        else if (!strcmp(m, "cube")) stt = SM_STT_CUBE, found = 1;
        else if (!strcmp(m, "volume")) stt = SM_STT_VOLUME, found = 1;
        if (found) k0 = 1;
        else if (strcmp(m, "pp") && strcmp(m, "centroid")) { error(a, line, "unknown dcl usage '_%s'", m); return; }
    }
    if (oi->op == SM_DCL && !a->ps && !k0) { error(a, line, "dcl in a vertex shader needs a usage (dcl_position ...)"); return; }
    for (int k = k0; k < nmods; k++) {
        const char *m = mods[k];
        if (!strcmp(m, "sat")) sat = 1;
        else if (!strcmp(m, "pp")) pp = 1;
        else if (!strcmp(m, "centroid")) centroid = 1;
        else if (!strcmp(m, "x2")) shift = 1;
        else if (!strcmp(m, "x4")) shift = 2;
        else if (!strcmp(m, "x8")) shift = 3;
        else if (!strcmp(m, "d2")) shift = -1;
        else if (!strcmp(m, "d4")) shift = -2;
        else if (!strcmp(m, "d8")) shift = -3;
        else { error(a, line, "unknown instruction modifier '_%s'", m); return; }
    }
    /* operands */
    char *ops[8] = { 0 };
    int nops = split_operands(rest, ops, 8), want;
    if (oi->op == SM_DEF || oi->op == SM_DEFI) want = 5;
    else if (oi->op == SM_DEFB) want = 2;
    else want = oi->ndst + oi->nsrc;
    uint32_t start = a->ntok;
    if (nops != want) {
        error(a, line, "'%s' takes %d operand%s, not %d", word, want, want == 1 ? "" : "s", nops);
        goto out;
    }
    struct sm_reg dst, src[4];
    if (oi->ndst && parse_dst(a, line, ops[0], &dst)) goto out;
    if (oi->ndst) dst.mod = (uint8_t)(sat | pp << 1 | centroid << 2), dst.shift = (int8_t)shift;
    if (oi->op == SM_DEF || oi->op == SM_DEFI || oi->op == SM_DEFB) {
        uint32_t vals[4];
        for (int k = 1; k < want; k++) {
            char *e = NULL;
            const char *t = ops[k];
            if (oi->op == SM_DEF) {
                float f = strtof(t, &e);
                memcpy(&vals[k - 1], &f, 4);
            } else if (oi->op == SM_DEFI) {
                long l = strtol(t, &e, 0);
                vals[k - 1] = (uint32_t)(int32_t)l;
            } else {
                if (!strcasecmp(t, "true") || !strcmp(t, "1")) vals[0] = 1, e = (char *)t + strlen(t);
                else if (!strcasecmp(t, "false") || !strcmp(t, "0")) vals[0] = 0, e = (char *)t + strlen(t);
            }
            if (!*t || !e || *e) { error(a, line, "'%s' value '%s' is not a number", base, t); goto out; }
        }
        emit(a, oi->op);
        emit(a, dst_token(&dst));
        for (int k = 1; k < want; k++) emit(a, vals[k - 1]);
    } else if (oi->op == SM_DCL) {
        emit(a, SM_DCL);
        emit(a, 0x80000000u | usage | usage_index << 16 | stt << 27);
        emit(a, dst_token(&dst));
    } else {
        for (int k = 0; k < oi->nsrc; k++) if (parse_src(a, line, ops[oi->ndst + k], &src[k])) goto out;
        emit(a, oi->op | control << 16);
        if (oi->ndst) emit(a, dst_token(&dst));
        for (int k = 0; k < oi->nsrc; k++) emit_src(a, &src[k]);
    }
    if (coissue) a->tok[start] |= 0x40000000u;
    if ((a->ver & 0xFFFF) >= 0x200) a->tok[start] |= (a->ntok - start - 1) << 24;
    if (a->nins == a->capins) {
        a->capins = a->capins * 2 + 32;
        a->ins_at = realloc(a->ins_at, sizeof *a->ins_at * a->capins);
        a->ins_line = realloc(a->ins_line, sizeof *a->ins_line * a->capins);
    }
    a->ins_at[a->nins] = start, a->ins_line[a->nins++] = line;
out:
    for (int k = 0; k < 8; k++) free(ops[k]);
}

int sm_assemble(const char *text, size_t len, const struct sm_define *defines, uint32_t **tokens,
                uint32_t *ntokens, char **errors) {
    struct as a = { 0 };
    sb_addn(&a.errs, "", 0);
    *tokens = NULL, *ntokens = 0, *errors = NULL;
    size_t n = 0;
    while (n < len && text[n]) n++;                           /* the text ends at len or a NUL */
    int nraw;
    char **raw = strip_comments(&a, text, n, &nraw);
    for (const struct sm_define *d = defines; d && d->name; d++) define_macro(&a, d->name, d->value ? d->value : "1");
    preprocess(&a, raw, nraw);
    for (int i = 0; i < nraw; i++) free(raw[i]);
    free(raw);
    int i = 0;
    /* the version first */
    if (!a.nlines) error(&a, 1, "expected a shader version (vs_x_y or ps_x_y), got nothing");
    else {
        const char *p = skip_ws(a.lines[0]);
        size_t w = 0;
        while (ident_char(p[w]) || p[w] == '.') w++;
        char word[64];
        snprintf(word, sizeof word, "%.*s", (int)(w ? (w < 63 ? w : 63) : strcspn(p, " ")), p);
        uint32_t v;
        if (!parse_version(word, &v)) error(&a, a.lineno[0], "expected a shader version, got '%s'", word);
        else if (*skip_ws(p + w)) error(&a, a.lineno[0], "unexpected '%s' after the shader version", skip_ws(p + w));
        else {
            char vb[16], err[256];
            struct sm_shader s;
            uint32_t vt[2] = { v, 0x0000FFFFu };
            a.ver = v, a.ps = SM_IS_PS(v), a.ver_line = a.lineno[0];
            if (sm_decode(vt, 2, &s, err, sizeof err)) error(&a, a.lineno[0], "%s", err);
            else sm_free(&s);
            (void)vb;
            emit(&a, v);
            i = 1;
        }
    }
    if (!a.nerr)
        for (; i < a.nlines; i++) assemble_line(&a, a.lineno[i], a.lines[i]);
    emit(&a, 0x0000FFFFu);
    if (!a.nerr) {
        /* the rules: the decoder's, reported at the instruction's line */
        struct sm_shader s;
        char err[256];
        uint32_t bad = 0;
        if (sm_decode_at(a.tok, a.ntok, &s, err, sizeof err, &bad)) {
            int line = a.ver_line;
            for (uint32_t k = 0; k < a.nins; k++) if (a.ins_at[k] <= bad) line = a.ins_line[k];
            error(&a, line, "%s", err);
        } else sm_free(&s);
    }
    for (int k = 0; k < a.nlines; k++) free(a.lines[k]);
    free(a.lines), free(a.lineno), free(a.ins_at), free(a.ins_line);
    for (int k = 0; k < a.nmacros; k++) free(a.macros[k].name), free(a.macros[k].value);
    free(a.macros);
    if (a.nerr) {
        free(a.tok);
        *errors = a.errs.s;
        return -1;
    }
    free(a.errs.s);
    *tokens = a.tok, *ntokens = a.ntok;
    return 0;
}

/* ---- D3DX ---- */

/* D3DXAssembleShader(pSrcData, SrcDataLen, pDefines, pInclude, Flags, ppShader, ppErrorMsgs). Flags (debug
 * information, validation, optimisation) change nothing here: the output never carries comments. */
static void x_D3DXAssembleShader(void) {
    uint32_t src = A32(0), len = A32(1), defs = A32(2), pshader = A32(5), perr = A32(6);
    if (perr) MEM32(perr) = 0;
    if (!src || !pshader) RET(D3DERR_INVALIDCALL, 7);
    MEM32(pshader) = 0;
    struct sm_define *hd = NULL;
    if (defs) {
        uint32_t nd = 0;
        while (MEM32(defs + 8 * nd)) nd++;
        hd = calloc(nd + 1, sizeof *hd);
        for (uint32_t k = 0; k < nd; k++) {
            hd[k].name = hle_ptr(MEM32(defs + 8 * k));
            hd[k].value = MEM32(defs + 8 * k + 4) ? hle_ptr(MEM32(defs + 8 * k + 4)) : NULL;
        }
    }
    uint32_t *t, nt;
    char *errs;
    int r = sm_assemble(hle_ptr(src), len, hd, &t, &nt, &errs);
    free(hd);
    if (r) {
        if (perr) {
            uint32_t n = (uint32_t)strlen(errs) + 1, b = d3dx_buffer_new(n);
            memcpy(hle_ptr(d3dx_buffer_ptr(b)), errs, n);
            MEM32(perr) = b;
        }
        free(errs);
        RET(E_FAIL_, 7);
    }
    uint32_t b = d3dx_buffer_new(4 * nt);
    memcpy(hle_ptr(d3dx_buffer_ptr(b)), t, 4 * nt);
    free(t);
    MEM32(pshader) = b;
    RET(D3D_OK, 7);
}

static void x_D3DXGetShaderVersion(void) { uint32_t p = A32(0); RET(p ? MEM32(p) : 0, 1); }

/* The device advertises shader model 2.0 (plan 5's Global Constraints). */
static uint32_t profile(uint32_t dev, const char *name, uint32_t *cache) {
    if (!dev) return 0;
    d3d_host(dev, D3DK_DEVICE);                               /* stops loudly on anything else */
    if (!*cache) {
        *cache = hle_va(hle_alloc((uint32_t)strlen(name) + 1));
        strcpy(hle_ptr(*cache), name);
    }
    return *cache;
}
static void x_D3DXGetVertexShaderProfile(void) { static uint32_t s; RET(profile(A32(0), "vs_2_0", &s), 1); }
static void x_D3DXGetPixelShaderProfile(void) { static uint32_t s; RET(profile(A32(0), "ps_2_0", &s), 1); }

static const win32hle_shim g_exports[] = {
    { "D3DXAssembleShader", x_D3DXAssembleShader }, { "D3DXGetShaderVersion", x_D3DXGetShaderVersion },
    { "D3DXGetVertexShaderProfile", x_D3DXGetVertexShaderProfile },
    { "D3DXGetPixelShaderProfile", x_D3DXGetPixelShaderProfile }, { 0, 0 } };
void d3dx_asm_register(void) { win32hle_register(g_exports); }
