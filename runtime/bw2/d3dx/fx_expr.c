/*
 * fx_expr.c - D3DX's CPU expressions (plan 5 Task 6): constant tables, and
 * the FXLC bytecode of effect state expressions, array selectors and shader
 * preshaders, run in float32.
 *
 * An expression is a token stream like a shader's: a version (0x46580200 for
 * a state's value or a selector's index, 0x46580201 for a preshader), then
 * comments, then the end token:
 *   CTAB: its inputs, effect parameters laid out in c registers;
 *   CLIT: its literals, a count then that many doubles;
 *   FXLC: a count, then instructions: (flags << 31 | opcode << 20 |
 *         components, input count) and the inputs and the output, each as
 *         (relative 0, table, component offset). Tables: 1 the literals, 2 the
 *         inputs, 4 the outputs, 7 temporaries. A scalar instruction (bit 31)
 *         reads one component of its first input for every component;
 *   PRSI: a preshader's outputs: (first register, 0, 0, registers, 0, 0,
 *         range count, then (register, count) pairs, 0, 0). The output table
 *         is the shader's float constants, register for register.
 * These layouts are what the game's 117 expressions show; Wine's d3dx9 names
 * the same opcodes. An opcode not listed here is reported by fx_expr_parse.
 */
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "fx.h"

#define FOURCC(a, b, c, d) ((uint32_t)(a) | (uint32_t)(b) << 8 | (uint32_t)(c) << 16 | (uint32_t)(d) << 24)
#define CC_CTAB FOURCC('C', 'T', 'A', 'B')
#define CC_CLIT FOURCC('C', 'L', 'I', 'T')
#define CC_FXLC FOURCC('F', 'X', 'L', 'C')
#define CC_PRSI FOURCC('P', 'R', 'S', 'I')
#define MAX_TABLE 4096u                     /* floats in one table */

enum { T_LIT = 1, T_IN = 2, T_OUT = 4, T_TEMP = 7 };
enum { O_MOV = 0x100, O_NEG, O_RCP = 0x103, O_FRC, O_EXP, O_LOG, O_RSQ, O_SIN, O_COS, O_ASIN, O_ACOS, O_ATAN,
       O_MIN = 0x200, O_MAX, O_LT, O_GE, O_ADD, O_MUL, O_ATAN2, O_DIV = 0x208, O_CMP = 0x300, O_DOT = 0x500 };
static const struct { uint16_t op; uint8_t nin; const char *name; } g_ops[] = {
    { O_MOV, 1, "mov" }, { O_NEG, 1, "neg" }, { O_RCP, 1, "rcp" }, { O_FRC, 1, "frc" }, { O_EXP, 1, "exp" },
    { O_LOG, 1, "log" }, { O_RSQ, 1, "rsq" }, { O_SIN, 1, "sin" }, { O_COS, 1, "cos" }, { O_ASIN, 1, "asin" },
    { O_ACOS, 1, "acos" }, { O_ATAN, 1, "atan" }, { O_MIN, 2, "min" }, { O_MAX, 2, "max" }, { O_LT, 2, "lt" },
    { O_GE, 2, "ge" }, { O_ADD, 2, "add" }, { O_MUL, 2, "mul" }, { O_ATAN2, 2, "atan2" }, { O_DIV, 2, "div" },
    { O_CMP, 3, "cmp" }, { O_DOT, 2, "dot" },
};
const char *fx_expr_opname(uint32_t opcode, uint32_t nin) {
    for (size_t i = 0; i < sizeof g_ops / sizeof g_ops[0]; i++)
        if (g_ops[i].op == opcode && g_ops[i].nin == nin) return g_ops[i].name;
    return NULL;
}

static int ferr(char *err, size_t n, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
static int ferr(char *err, size_t n, const char *fmt, ...) {
    if (err && n) {
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(err, n, fmt, ap);
        va_end(ap);
    }
    return -1;
}
static uint16_t rd16(const uint8_t *p) { uint16_t v; memcpy(&v, p, 2); return v; }
static uint32_t rd32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }
static float u2f(uint32_t u) { float f; memcpy(&f, &u, 4); return f; }
static uint32_t f2u(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }

/* the comment of fourcc among those after tokens[0]: its data (after the fourcc) and dwords, or NULL */
const uint32_t *fx_comment(const uint32_t *t, uint32_t n, uint32_t cc, uint32_t *len) {
    for (uint32_t i = 1; i < n && (t[i] & 0xFFFFu) == 0xFFFEu;) {
        uint32_t l = (t[i] >> 16) & 0x7FFFu;
        if (l > n - i - 1) return NULL;
        if (l >= 1 && t[i + 1] == cc) { *len = l - 1; return t + i + 2; }
        i += 1 + l;
    }
    return NULL;
}

/* ---- constant tables ---- */

/* D3DXSHADER_CONSTANTTABLE: Size, Creator, Version, Constants, ConstantInfo, Flags, Target;
 * D3DXSHADER_CONSTANTINFO (20 bytes): Name, RegisterSet, RegisterIndex, RegisterCount, Reserved, TypeInfo,
 * DefaultValue; D3DXSHADER_TYPEINFO (16): Class, Type, Rows, Columns, Elements, StructMembers,
 * StructMemberInfo; D3DXSHADER_STRUCTMEMBERINFO (8): Name, TypeInfo. Offsets count from the table. */
static const char *tstr(const uint8_t *b, uint32_t size, uint32_t off) {
    if (off >= size || !memchr(b + off, 0, size - off)) return NULL;
    return (const char *)b + off;
}
int fx_ctab_read(const uint32_t *t, uint32_t n, struct fx_effect *e, struct fx_ctab *ct, char *err, size_t errlen) {
    memset(ct, 0, sizeof *ct);
    uint32_t len;
    const uint32_t *d = fx_comment(t, n, CC_CTAB, &len);
    if (!d) return 0;
    const uint8_t *b = (const uint8_t *)d;
    uint32_t size = 4 * len;
    if (size < 28 || rd32(b) != 28) return ferr(err, errlen, "constant table: header of %u bytes", size);
    uint32_t count = rd32(b + 12), info = rd32(b + 16);
    if (info > size || count > (size - info) / 20) return ferr(err, errlen, "constant table: %u constants", count);
    ct->base = b, ct->size = size;
    ct->e = calloc(count ? count : 1, sizeof *ct->e);
    for (uint32_t i = 0; i < count; i++) {
        const uint8_t *c = b + info + 20 * i;
        struct fx_ctab_entry *x = &ct->e[i];
        x->name = tstr(b, size, rd32(c));
        x->set = rd16(c + 4), x->index = rd16(c + 6), x->count = rd16(c + 8), x->type = rd32(c + 12);
        if (!x->name || x->set > FX_RS_SAMPLER || x->type > size || size - x->type < 16) {
            fx_ctab_free(ct);
            return ferr(err, errlen, "constant table: constant %u is malformed", i);
        }
        x->param = e ? fx_find_param(e, NULL, x->name) : NULL;
        ct->n = i + 1;
    }
    return 0;
}
void fx_ctab_free(struct fx_ctab *t) { free(t->e); memset(t, 0, sizeof *t); }

/* a numeric leaf's element (r, c) as stored (its own class's layout), converted to set's kind */
static uint32_t elem(const struct fx_param *p, uint32_t r, uint32_t c, uint32_t set) {
    uint32_t v = 0;
    if (p && p->data && (p->type == FXT_BOOL || p->type == FXT_INT || p->type == FXT_FLOAT) && r < p->rows
        && c < p->cols && p->cls <= FXC_MATRIX_COLUMNS) {
        uint32_t i = p->cls == FXC_MATRIX_COLUMNS ? c * p->rows + r : r * p->cols + c;
        v = rd32(p->data + 4 * i);
    } else return 0;
    if (set == FX_RS_FLOAT4)
        return p->type == FXT_FLOAT ? v : p->type == FXT_INT ? f2u((float)(int32_t)v) : f2u(v ? 1.0f : 0.0f);
    if (set == FX_RS_INT4) {
        if (p->type != FXT_FLOAT) return p->type == FXT_BOOL ? (v != 0) : v;
        float f = u2f(v);
        return f > -2147483649.0f && f < 2147483648.0f ? (uint32_t)(int32_t)f : 0x80000000u;
    }
    return p->type == FXT_FLOAT ? (u2f(v) != 0.0f) : (v != 0);
}
struct regs { const uint8_t *b; uint32_t size, set, n, max; uint32_t (*dst)[4]; };
static void regs_of(struct regs *r, const struct fx_param *p, uint32_t ti, int depth) {
    if (depth > 16 || ti > r->size || r->size - ti < 16) return;
    const uint8_t *t = r->b + ti;
    uint32_t cls = rd16(t), rows = rd16(t + 4), cols = rd16(t + 6), elements = rd16(t + 8), members = rd16(t + 10);
    uint32_t minfo = rd32(t + 12);
    for (uint32_t k = 0; k < (elements ? elements : 1) && r->n < r->max; k++) {
        const struct fx_param *pe = p && elements && p->elements ? (k < p->elements ? &p->members[k] : NULL) : p;
        if (cls == FXC_STRUCT) {
            for (uint32_t m = 0; m < members && r->n < r->max; m++) {
                if (minfo > r->size || (r->size - minfo) / 8 <= m) return;
                const struct fx_param *pm = pe && pe->cls == FXC_STRUCT && m < pe->nmembers ? &pe->members[m] : NULL;
                regs_of(r, pm, rd32(r->b + minfo + 8 * m + 4), depth + 1);
            }
            continue;
        }
        if (cls == FXC_OBJECT) continue;
        /* a matrix by rows or columns, a vector or scalar in one register; a BOOL register holds one
         * component, so there each component takes a register of its own */
        uint32_t nreg = cls == FXC_MATRIX_ROWS ? rows : cls == FXC_MATRIX_COLUMNS ? cols : 1;
        uint32_t ncomp = cls == FXC_MATRIX_COLUMNS ? rows : cols;
        for (uint32_t g = 0; g < nreg && r->n < r->max; g++) {
            uint32_t *d = NULL;
            for (uint32_t c = 0; c < ncomp && c < 4; c++) {
                if (r->set == FX_RS_BOOL || !c) {
                    if (r->n >= r->max) break;
                    d = r->dst[r->n++];
                    memset(d, 0, 16);
                }
                d[r->set == FX_RS_BOOL ? 0 : c] = cls == FXC_MATRIX_COLUMNS ? elem(pe, c, g, r->set) : elem(pe, g, c, r->set);
            }
        }
    }
}
uint32_t fx_ctab_regs(const struct fx_ctab *t, uint32_t i, uint32_t (*dst)[4], uint32_t max) {
    if (i >= t->n) return 0;
    const struct fx_ctab_entry *x = &t->e[i];
    struct regs r = { t->base, t->size, x->set, 0, max < x->count ? max : x->count, dst };
    regs_of(&r, x->param, x->type, 0);
    return r.n;
}

/* ---- expressions ---- */

void fx_expr_free(struct fx_expr *x) {
    if (!x) return;
    fx_ctab_free(&x->in);
    free(x->lit), free(x->prsi);
    memset(x, 0, sizeof *x);
}

static int grow(uint32_t *size, uint32_t need) { if (need > *size) *size = need; return need <= MAX_TABLE; }

int fx_expr_parse(const uint32_t *t, uint32_t n, struct fx_effect *e, struct fx_expr *x, char *err, size_t errlen) {
    memset(x, 0, sizeof *x);
    if (n < 2 || (t[0] & 0xFFFF0000u) != 0x46580000u) return ferr(err, errlen, "not an expression (0x%08X)", n ? t[0] : 0);
    x->version = t[0];
    uint32_t len, i;
    for (i = 1; i < n && (t[i] & 0xFFFFu) == 0xFFFEu;) {
        uint32_t l = (t[i] >> 16) & 0x7FFFu;
        if (l > n - i - 1) return ferr(err, errlen, "expression: comment at token %u runs past the end", i);
        i += 1 + l;
    }
    if (i >= n || t[i] != 0x0000FFFFu) return ferr(err, errlen, "expression: no end token after the comments");
    if (fx_ctab_read(t, n, e, &x->in, err, errlen)) return -1;
    const uint32_t *d = fx_comment(t, n, CC_CLIT, &len);
    if (d) {
        if (!len || d[0] > (len - 1) / 2) { fx_expr_free(x); return ferr(err, errlen, "expression: literals do not fit"); }
        x->nlit = d[0];
        x->lit = calloc(x->nlit ? x->nlit : 1, sizeof *x->lit);
        for (uint32_t k = 0; k < x->nlit; k++) {
            double v;
            memcpy(&v, d + 1 + 2 * k, 8);
            x->lit[k] = (float)v;
        }
    }
    for (uint32_t k = 0; k < x->in.n; k++)
        if (x->in.e[k].set != FX_RS_FLOAT4) {
            fx_expr_free(x);
            return ferr(err, errlen, "expression: input '%s' in register set %u", x->in.e[k].name, x->in.e[k].set);
        } else if (!grow(&x->nin, 4 * (x->in.e[k].index + x->in.e[k].count))) {
            fx_expr_free(x);
            return ferr(err, errlen, "expression: input register %u", x->in.e[k].index + x->in.e[k].count);
        }
    d = fx_comment(t, n, CC_PRSI, &len);
    if (d) {
        if (len < 7 || d[6] > (len - 7) / 2) { fx_expr_free(x); return ferr(err, errlen, "expression: PRSI malformed"); }
        x->nranges = d[6];
        x->prsi = calloc(2 * x->nranges + 1, sizeof *x->prsi);
        memcpy(x->prsi, d + 7, 8 * x->nranges);
        for (uint32_t k = 0; k < x->nranges; k++)
            if (!grow(&x->nout, 4 * (x->prsi[2 * k] + x->prsi[2 * k + 1]))) {
                fx_expr_free(x);
                return ferr(err, errlen, "expression: preshader output register %u", x->prsi[2 * k]);
            }
    }
    d = fx_comment(t, n, CC_FXLC, &len);
    if (!d || !len) { fx_expr_free(x); return ferr(err, errlen, "expression: no FXLC"); }
    x->nins = d[0], x->code = d + 1;
    uint32_t at = 1;
    for (uint32_t k = 0; k < x->nins; k++) {
        if (len - at < 2) { fx_expr_free(x); return ferr(err, errlen, "expression: instruction %u runs past the end", k); }
        uint32_t code = d[at], nin = d[at + 1], op = (code >> 20) & 0x7FFu, ncomp = code & 0xFFFFu;
        if (!fx_expr_opname(op, nin)) {
            fx_expr_free(x);
            return ferr(err, errlen, "FXLC opcode 0x%03X with %u inputs (instruction %u) is not implemented", op, nin, k);
        }
        if (!ncomp || ncomp > 4) { fx_expr_free(x); return ferr(err, errlen, "expression: %u components", ncomp); }
        at += 2;
        if ((len - at) / 3 < nin + 1) { fx_expr_free(x); return ferr(err, errlen, "expression: operands run past the end"); }
        for (uint32_t a = 0; a <= nin; a++, at += 3) {
            uint32_t rel = d[at], table = d[at + 1], off = d[at + 2];
            uint32_t width = a == nin ? (op == O_DOT ? 1 : ncomp) : ((code >> 31) && a == 0 ? 1 : ncomp);
            if (rel) { fx_expr_free(x); return ferr(err, errlen, "expression: relative addressing (instruction %u)", k); }
            int ok = off < MAX_TABLE;
            switch (table) {
            case T_LIT: ok = ok && a < nin && off + width <= x->nlit; break;
            case T_IN: ok = ok && a < nin && grow(&x->nin, off + width); break;
            case T_TEMP: ok = ok && grow(&x->ntemp, off + width); break;
            case T_OUT: ok = ok && a == nin && grow(&x->nout, off + width); break;
            default: ok = 0;
            }
            if (!ok) {
                fx_expr_free(x);
                return ferr(err, errlen, "expression: operand %u of instruction %u (table %u offset %u)", a, k, table, off);
            }
        }
    }
    return 0;
}

uint64_t fx_expr_serial(const struct fx_expr *x) {
    uint64_t s = 0;
    for (uint32_t i = 0; i < x->in.n; i++)
        if (x->in.e[i].param && *x->in.e[i].param->serial > s) s = *x->in.e[i].param->serial;
    return s;
}

static float run1(uint32_t op, const float *a) {
    switch (op) {
    case O_MOV: return a[0];
    case O_NEG: return -a[0];
    case O_RCP: return 1.0f / a[0];
    case O_FRC: return a[0] - floorf(a[0]);
    case O_EXP: return exp2f(a[0]);
    case O_LOG: return log2f(fabsf(a[0]));
    case O_RSQ: return 1.0f / sqrtf(fabsf(a[0]));
    case O_SIN: return sinf(a[0]);
    case O_COS: return cosf(a[0]);
    case O_ASIN: return asinf(a[0]);
    case O_ACOS: return acosf(a[0]);
    case O_ATAN: return atanf(a[0]);
    case O_MIN: return a[0] < a[1] ? a[0] : a[1];
    case O_MAX: return a[0] > a[1] ? a[0] : a[1];
    case O_LT: return a[0] < a[1] ? 1.0f : 0.0f;
    case O_GE: return a[0] >= a[1] ? 1.0f : 0.0f;
    case O_ADD: return a[0] + a[1];
    case O_MUL: return a[0] * a[1];
    case O_ATAN2: return atan2f(a[0], a[1]);
    case O_DIV: return a[0] / a[1];
    case O_CMP: return a[0] >= 0.0f ? a[1] : a[2];
    }
    return 0.0f;
}

int fx_expr_run(const struct fx_expr *x, const struct fx_effect *e, float *out, uint32_t nout) {
    (void)e;
    float in[x->nin + 1], tmp[x->ntemp + 1], res[x->nout + 1];
    memset(in, 0, sizeof in), memset(tmp, 0, sizeof tmp), memset(res, 0, sizeof res);
    for (uint32_t i = 0; i < x->in.n; i++) {
        const struct fx_ctab_entry *c = &x->in.e[i];
        if (c->set != FX_RS_FLOAT4 || !c->count) continue;
        uint32_t regs[c->count][4];
        uint32_t got = fx_ctab_regs(&x->in, i, regs, c->count);
        memcpy(in + 4 * c->index, regs, 16 * got);
    }
    const uint32_t *d = x->code;
    for (uint32_t k = 0, at = 0; k < x->nins; k++) {
        uint32_t code = d[at], nin = d[at + 1], op = (code >> 20) & 0x7FFu, ncomp = code & 0xFFFFu;
        int scalar = (int)(code >> 31);
        const uint32_t *opd = d + at + 2;
        at += 2 + 3 * (nin + 1);
        float v[4], a[3];
        const float *src[3];
        for (uint32_t i = 0; i < nin; i++) {
            uint32_t table = opd[3 * i + 1], off = opd[3 * i + 2];
            src[i] = table == T_LIT ? x->lit + off : table == T_IN ? in + off : tmp + off;
        }
        uint32_t nres = ncomp;
        if (op == O_DOT) {
            float s = 0.0f;
            for (uint32_t c = 0; c < ncomp; c++) s += src[0][c] * src[1][c];
            v[0] = s, nres = 1;
        } else
            for (uint32_t c = 0; c < ncomp; c++) {
                for (uint32_t i = 0; i < nin; i++) a[i] = src[i][scalar && i == 0 ? 0 : c];
                v[c] = run1(op, a);
            }
        uint32_t table = opd[3 * nin + 1], off = opd[3 * nin + 2];
        memcpy((table == T_OUT ? res : tmp) + off, v, 4 * nres);
    }
    for (uint32_t i = 0; i < nout; i++) out[i] = i < x->nout ? res[i] : 0.0f;
    return 0;
}

uint32_t *fx_expr_ops(const uint32_t *t, uint32_t n, uint32_t *count) {
    *count = 0;
    uint32_t len;
    const uint32_t *d = fx_comment(t, n, CC_FXLC, &len);
    if (!d || !len) return NULL;
    uint32_t *ops = calloc(d[0] + 1, sizeof *ops), at = 1;
    for (uint32_t k = 0; k < d[0] && len - at >= 2; k++) {
        uint32_t code = d[at], nin = d[at + 1];
        ops[(*count)++] = ((code >> 20) & 0x7FFu) << 16 | nin;
        at += 2 + 3 * (nin + 1);
        if (at > len) break;
    }
    return ops;
}
