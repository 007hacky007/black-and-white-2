/*
 * sm_selftest.c - the shader assembler and decoder (plan 5 Task 2), called
 * directly and through D3DX's imports as lifted code calls them. Built with
 * the engine (minus main.c) by tests/test_shaders.py;
 * `sm_selftest <group> [arg]` runs one group and prints "<group>: <n> failed".
 *
 * The roundtrip: assemble (or take bytecode), decode, print the decoded form
 * as assembly, assemble that, and compare the tokens.
 */
#include <dirent.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "win32hle.h"
#include "bw2.h"
#include "d3d_com.h"
#include "sm.h"

const recomp_dispatch_entry_t recomp_dispatch_table[1] = { { 0, 0 } };
const uint32_t recomp_dispatch_count = 0;

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)
#define CHECKEQ(a, b) do { uint64_t _a = (uint64_t)(a), _b = (uint64_t)(b); if (_a != _b) { \
    printf("FAIL %s:%d %s == %s: 0x%llX != 0x%llX\n", __FILE__, __LINE__, #a, #b, \
           (unsigned long long)_a, (unsigned long long)_b); fails++; } } while (0)

#define E_FAIL_ 0x80004005u

static uint32_t run(recomp_func_t f, const char *name, int n, const uint32_t *a) {
    uint32_t esp0 = g_esp;
    for (int i = n - 1; i >= 0; i--) { g_esp -= 4; MEM32(g_esp) = a[i]; }
    g_esp -= 4;
    MEM32(g_esp) = 0x12345678u;
    f();
    if (g_esp != esp0) {
        printf("FAIL %s: popped %d bytes, want %d\n", name, (int)(g_esp - (esp0 - 4u - 4u * (uint32_t)n)), 4 + 4 * n);
        fails++;
        g_esp = esp0;
    }
    return g_eax;
}
static uint32_t call(const char *name, int n, ...) {
    uint32_t va = hle_resolve(name);
    recomp_func_t f = va ? recomp_lookup_import(va) : NULL;
    if (!f) { printf("FAIL %s: not answered\n", name); fails++; return 0xDEADBEEFu; }
    uint32_t a[16];
    va_list ap;
    va_start(ap, n);
    for (int i = 0; i < n; i++) a[i] = va_arg(ap, uint32_t);
    va_end(ap);
    return run(f, name, n, a);
}
static uint32_t vcall(uint32_t obj, int slot, int n, ...) {
    uint32_t va = MEM32(MEM32(obj) + 4u * (uint32_t)slot);
    recomp_func_t f = recomp_lookup_import(va);
    if (!f) { printf("FAIL slot %d of 0x%08X: no method\n", slot, obj); fails++; return 0xDEADBEEFu; }
    uint32_t a[16] = { obj };
    va_list ap;
    va_start(ap, n);
    for (int i = 0; i < n; i++) a[1 + i] = va_arg(ap, uint32_t);
    va_end(ap);
    return run(f, g_cur_import, n + 1, a);
}
static uint32_t gmem(uint32_t n) { uint32_t p = hle_va(hle_alloc(n)); memset(hle_ptr(p), 0, n); return p; }
static uint32_t gstr(const char *s) { uint32_t p = gmem((uint32_t)strlen(s) + 1); strcpy(hle_ptr(p), s); return p; }
static uint32_t gbytes(const void *b, uint32_t n) { uint32_t p = gmem(n + 1); memcpy(hle_ptr(p), b, n); return p; }

static char *read_file(const char *path, size_t *n) {
    FILE *f = fopen(path, "rb");
    if (!f) { printf("FAIL cannot open %s\n", path); fails++; *n = 0; return NULL; }
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *d = malloc((size_t)len + 1);
    *n = fread(d, 1, (size_t)len, f);
    d[*n] = 0;
    fclose(f);
    return d;
}
/* the sorted names of the regular files in dir */
static int list_dir(const char *dir, char ***names) {
    DIR *d = opendir(dir);
    int n = 0, cap = 64;
    *names = malloc(sizeof(char *) * (size_t)cap);
    if (!d) { printf("FAIL cannot open %s\n", dir); fails++; return 0; }
    for (struct dirent *e; (e = readdir(d));) {
        if (e->d_name[0] == '.') continue;
        if (n == cap) *names = realloc(*names, sizeof(char *) * (size_t)(cap *= 2));
        (*names)[n++] = strdup(e->d_name);
    }
    closedir(d);
    for (int i = 1; i < n; i++)
        for (int j = i; j > 0 && strcmp((*names)[j - 1], (*names)[j]) > 0; j--) {
            char *t = (*names)[j]; (*names)[j] = (*names)[j - 1]; (*names)[j - 1] = t;
        }
    return n;
}

/* ---- the decoded form printed as assembly ---- */

static void cat(char **p, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void cat(char **p, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    *p += vsprintf(*p, fmt, ap);
    va_end(ap);
}
static void print_reg(char **p, uint32_t ver, const struct sm_reg *r) {
    char b[32], rb[32];
    if (!r->rel) { cat(p, "%s", sm_reg_name(ver, r, b, sizeof b)); return; }
    struct sm_reg base = *r, rr = { .type = r->rel_type, .index = r->rel_index };
    base.rel = 0, base.index = 0;
    sm_reg_name(ver, &base, b, sizeof b);
    b[strcspn(b, "0123456789")] = 0;
    sm_reg_name(ver, &rr, rb, sizeof rb);
    if (r->rel_type == SM_R_LOOP) cat(p, "%s[%s + %u]", b, rb, r->index);
    else cat(p, "%s[%s.%c + %u]", b, rb, "xyzw"[r->rel_comp & 3], r->index);
}
static void print_dst(char **p, uint32_t ver, const struct sm_reg *r) {
    print_reg(p, ver, r);
    if (r->mask_or_swizzle != 0xF) {
        cat(p, ".");
        for (int c = 0; c < 4; c++) if (r->mask_or_swizzle >> c & 1) cat(p, "%c", "xyzw"[c]);
    }
}
static void print_src(char **p, uint32_t ver, const struct sm_reg *r) {
    switch (r->mod) {
    case SM_SRC_NEG: case SM_SRC_BIASNEG: case SM_SRC_SIGNNEG: case SM_SRC_X2NEG: case SM_SRC_ABSNEG: cat(p, "-"); break;
    case SM_SRC_COMP: cat(p, "1-"); break;
    case SM_SRC_NOT: cat(p, "!"); break;
    }
    print_reg(p, ver, r);
    switch (r->mod) {
    case SM_SRC_BIAS: case SM_SRC_BIASNEG: cat(p, "_bias"); break;
    case SM_SRC_SIGN: case SM_SRC_SIGNNEG: cat(p, "_bx2"); break;
    case SM_SRC_X2: case SM_SRC_X2NEG: cat(p, "_x2"); break;
    case SM_SRC_DZ: cat(p, "_dz"); break;
    case SM_SRC_DW: cat(p, "_dw"); break;
    case SM_SRC_ABS: case SM_SRC_ABSNEG: cat(p, "_abs"); break;
    }
    if (r->mask_or_swizzle != SM_SWIZZLE_IDENTITY) {
        cat(p, ".");
        for (int c = 0; c < 4; c++) cat(p, "%c", "xyzw"[r->mask_or_swizzle >> (2 * c) & 3]);
    }
}
static const char *const g_usage[] = { "position", "blendweight", "blendindices", "normal", "psize", "texcoord",
    "tangent", "binormal", "tessfactor", "positiont", "color", "fog", "depth", "sample" };

/* malloc'd assembly for s */
static char *print_asm(const struct sm_shader *s) {
    char *out = malloc(256 + (size_t)s->nins * 256), *p = out, vb[16];
    cat(&p, "%s\n", sm_version_name(s->version, vb, sizeof vb));
    for (uint32_t i = 0; i < s->nins; i++) {
        const struct sm_ins *in = &s->ins[i];
        const struct sm_opinfo *oi = sm_opinfo(in->op, s->version);
        if (in->coissue) cat(&p, "+");
        if (in->op == SM_TEX && in->control) cat(&p, "%s", in->control == SM_TEXLD_PROJECT ? "texldp" : "texldb");
        else cat(&p, "%s", oi ? oi->name : "?");
        if (in->op == SM_DCL) {
            if (in->dst.type == SM_R_SAMPLER) {
                if (in->sampler_type == SM_STT_2D) cat(&p, "_2d");
                else if (in->sampler_type == SM_STT_CUBE) cat(&p, "_cube");
                else if (in->sampler_type == SM_STT_VOLUME) cat(&p, "_volume");
            } else if (!s->is_ps || in->usage || in->usage_index) {
                cat(&p, "_%s", in->usage < 14 ? g_usage[in->usage] : "?");
                if (in->usage_index) cat(&p, "%u", in->usage_index);
            }
        }
        if (in->sat) cat(&p, "_sat");
        if (in->pp) cat(&p, "_pp");
        if (in->centroid) cat(&p, "_centroid");
        static const char *const shifts[] = { "_d8", "_d4", "_d2", "", "_x2", "_x4", "_x8" };
        if (in->ndst && in->dst.shift >= -3 && in->dst.shift <= 3) cat(&p, "%s", shifts[in->dst.shift + 3]);
        const char *sep = " ";
        if (in->ndst) { cat(&p, "%s", sep); print_dst(&p, s->version, &in->dst); sep = ", "; }
        if (in->op == SM_DEF) for (int k = 0; k < 4; k++) cat(&p, ", %.9g", (double)in->def[k]);
        else if (in->op == SM_DEFI) for (int k = 0; k < 4; k++) cat(&p, ", %d", in->defi[k]);
        else if (in->op == SM_DEFB) cat(&p, ", %s", in->defi[0] ? "true" : "false");
        for (int k = 0; k < in->nsrc; k++) { cat(&p, "%s", sep); print_src(&p, s->version, &in->src[k]); sep = ", "; }
        cat(&p, "\n");
    }
    return out;
}

static void print_tokens(const char *label, const uint32_t *t, uint32_t n) {
    printf("%s", label);
    for (uint32_t i = 0; i < n; i++) printf(" %08X", t[i]);
    printf("\n");
}

/* decode tokens, print, assemble the print, compare with want (want/nwant: the expected tokens) */
static int roundtrip(const char *name, const uint32_t *t, uint32_t n, const uint32_t *want, uint32_t nwant) {
    struct sm_shader s;
    char err[256];
    if (sm_decode(t, n, &s, err, sizeof err)) { printf("FAIL %s: decode: %s\n", name, err); fails++; return 0; }
    int ok = 1;
    if (s.version != t[0] || s.hash != sm_hash(t, s.ntokens) || s.ntokens > n || t[s.ntokens - 1] != 0xFFFFu) {
        printf("FAIL %s: version/hash/ntokens\n", name); fails++; ok = 0;
    }
    char *text = print_asm(&s), *errs = NULL;
    uint32_t *t2 = NULL, n2 = 0;
    if (sm_assemble(text, strlen(text), NULL, &t2, &n2, &errs)) {
        printf("FAIL %s: the print does not assemble:\n%s%s", name, errs, text); fails++; ok = 0;
    } else if (n2 != nwant || memcmp(t2, want, 4 * n2)) {
        uint32_t k = 0;
        while (k < n2 && k < nwant && t2[k] == want[k]) k++;
        printf("FAIL %s: roundtrip differs at token %u\n%s", name, k, text);
        print_tokens("  want", want, nwant);
        print_tokens("  got ", t2, n2);
        fails++, ok = 0;
    }
    free(t2), free(errs), free(text);
    sm_free(&s);
    return ok;
}

/* ---- groups ---- */

static void group_asm_tokens(const char *path) {
    size_t n;
    char *src = read_file(path, &n), *errs = NULL;
    uint32_t *t = NULL, nt = 0;
    if (!src) return;
    if (sm_assemble(src, n, NULL, &t, &nt, &errs)) { printf("FAIL errors:\n%s", errs); fails++; }
    else { print_tokens("tokens:", t, nt); CHECK(errs == NULL); }
    free(t), free(errs), free(src);
}

/* D3DXAssembleShader through the guest: hr, *shader, *errors (guest objects or 0) */
static uint32_t asm_guest(const char *src, uint32_t defines, uint32_t *shader, uint32_t *errors) {
    uint32_t ps = gmem(4), pe = gmem(4);
    MEM32(ps) = MEM32(pe) = 0xCDCDCDCDu;
    uint32_t hr = call("D3DXAssembleShader", 7, gstr(src), (uint32_t)strlen(src), defines, 0u, 1u, ps, pe);
    *shader = MEM32(ps), *errors = MEM32(pe);
    return hr;
}
enum { B_GetBufferPointer = 3, B_GetBufferSize = 4 };
static const char *buf_text(uint32_t b) { return b ? (const char *)hle_ptr(vcall(b, B_GetBufferPointer, 0)) : ""; }

static int has_tokens(uint32_t b, const uint32_t *want, uint32_t n) {
    if (!b) return 0;
    uint32_t size = vcall(b, B_GetBufferSize, 0), p = vcall(b, B_GetBufferPointer, 0);
    return size == 4 * n && !memcmp(hle_ptr(p), want, 4 * n);
}
static const uint32_t g_m4x4[] = { 0xFFFE0101, 0x0000001F, 0x80000000, 0x900F0000, 0x00000014, 0xC00F0000,
                                   0x90E40000, 0xA0E40000, 0x0000FFFF };

static void expect_error(const char *src, const char *line, const char *what) {
    uint32_t sh, er, hr = asm_guest(src, 0, &sh, &er);
    const char *t = buf_text(er);
    if (hr != E_FAIL_ || sh || !er || !strstr(t, line) || !strstr(t, what)) {
        printf("FAIL source %s: hr 0x%08X shader 0x%X, want \"%s\" and \"%s\" in:\n%s\n", src, hr, sh, line, what, t);
        fails++;
    }
}

static void group_asm_errors(void) {
    uint32_t sh, er, hr;
    /* an unknown instruction: E_FAIL, no shader, the error names line 2 */
    hr = asm_guest("vs_1_1\nfoo r0, v0\n", 0, &sh, &er);
    CHECKEQ(hr, E_FAIL_);
    CHECKEQ(sh, 0);
    CHECK(er && strstr(buf_text(er), "(2): error:") && strstr(buf_text(er), "foo"));
    if (er) { CHECKEQ(vcall(er, B_GetBufferSize, 0), strlen(buf_text(er)) + 1); CHECKEQ(vcall(er, 2, 0), 0); }
    /* success: the tokens, no error buffer */
    hr = asm_guest("vs_1_1\ndcl_position v0\nm4x4 oPos, v0, c0\n", 0, &sh, &er);
    CHECKEQ(hr, D3D_OK);
    CHECKEQ(er, 0);
    CHECK(has_tokens(sh, g_m4x4, 9));
    if (sh) CHECKEQ(vcall(sh, 2, 0), 0);
    /* a #define substitutes */
    hr = asm_guest("vs_1_1\n#define POS v0\ndcl_position POS\nm4x4 oPos, POS, c0\n", 0, &sh, &er);
    CHECKEQ(hr, D3D_OK);
    CHECK(has_tokens(sh, g_m4x4, 9));
    /* a defines array substitutes (D3DXMACRO { Name, Definition }, ending with a NULL name) */
    uint32_t defs = gmem(24);
    MEM32(defs) = gstr("MAT"), MEM32(defs + 4) = gstr("c0");
    MEM32(defs + 8) = gstr("IN"), MEM32(defs + 12) = gstr("v0");
    hr = asm_guest("vs_1_1\ndcl_position IN\nm4x4 oPos, IN, MAT\n", defs, &sh, &er);
    CHECKEQ(hr, D3D_OK);
    CHECK(has_tokens(sh, g_m4x4, 9));
    /* a NULL definition defines the name as 1 */
    MEM32(defs + 12) = 0;
    hr = asm_guest("vs_1_1\n#if IN\ndcl_position v0\n#endif\nm4x4 oPos, v0, MAT\n", defs, &sh, &er);
    CHECKEQ(hr, D3D_OK);
    CHECK(has_tokens(sh, g_m4x4, 9));
    /* the error messages name the line and the problem */
    expect_error("mov r0, v0\n", "(1): error:", "expected a shader version");
    expect_error("float4x4 m : register(c0);\n", "(1): error:", "expected a shader version, got 'float4x4'");
    expect_error("vs_1_1\nmov r0\n", "(2): error:", "mov");
    expect_error("vs_1_1\n\nmov r0, q0\n", "(3): error:", "q0");
    expect_error("vs_1_1\nmov r0.yx, v0\n", "(2): error:", "mask");
    expect_error("vs_1_1\nmov r0, v0.xyzwx\n", "(2): error:", "swizzle");
    expect_error("vs_1_1\nmov_x9 r0, v0\n", "(2): error:", "_x9");
    expect_error("vs_1_1\ndcl_bogus v0\n", "(2): error:", "bogus");
    expect_error("vs_1_1\ndef c0, 1, 2, x, 4\n", "(2): error:", "def");
    expect_error("ps_2_0\nmov r0, c0\ncnd r0, r0, r0, r0\n", "(3): error:", "cnd");
    expect_error("ps_1_1\ntexld r0, t0\n", "(2): error:", "texld");
    expect_error("ps_2_0\nmov r0, c40\n", "(2): error:", "c40");
    expect_error("vs_1_1\nmov_sat r0, v0\n", "(2): error:", "_sat");
    expect_error("vs_1_1\nmov r0, v0_bx2\n", "(2): error:", "bx2");
    expect_error("vs_1_1\nmov r0, c[a0.y + 1]\n", "(2): error:", "a0.x");
    expect_error("vs_3_0\nmov o0, v0\n", "(1): error:", "vs_3_0");
    expect_error("ps_2_x\nmov oC0, c0\n", "(1): error:", "ps_2_x");
    expect_error("vs_1_1\nvs_1_1\n", "(2): error:", "version");
    expect_error("vs_1_1\n#if 1\nmov r0, v0\n", "error:", "#endif");
    expect_error("vs_1_1\n#endif\n", "(2): error:", "#endif");
    expect_error("vs_1_1\n#error stop here\n", "(2): error:", "stop here");
    expect_error("vs_1_1\n#if 1 +\n#endif\n", "(2): error:", "#if");
    expect_error("vs_1_1\n#define F(x) x\n", "(2): error:", "F");
    expect_error("vs_1_1\n/* open\n", "(2): error:", "comment");
    /* two bad lines: both reported */
    hr = asm_guest("vs_1_1\nfoo\nbar r0\n", 0, &sh, &er);
    CHECKEQ(hr, E_FAIL_);
    CHECK(strstr(buf_text(er), "(2): error:") && strstr(buf_text(er), "(3): error:"));
    /* NULL text or out pointer: D3DERR_INVALIDCALL; a NULL ppErrorMsgs is fine */
    uint32_t out = gmem(4), src = gstr("vs_1_1\nfoo\n");
    CHECKEQ(call("D3DXAssembleShader", 7, 0u, 4u, 0u, 0u, 0u, out, 0u), D3DERR_INVALIDCALL);
    CHECKEQ(call("D3DXAssembleShader", 7, src, 11u, 0u, 0u, 0u, 0u, 0u), D3DERR_INVALIDCALL);
    CHECKEQ(call("D3DXAssembleShader", 7, src, 11u, 0u, 0u, 0u, out, 0u), E_FAIL_);
    CHECKEQ(MEM32(out), 0);
    /* the length counts: text after len is not read */
    src = gstr("vs_1_1\ndcl_position v0\nm4x4 oPos, v0, c0\nfoo\n");
    CHECKEQ(call("D3DXAssembleShader", 7, src, 41u, 0u, 0u, 0u, out, 0u), D3D_OK);
    CHECK(has_tokens(MEM32(out), g_m4x4, 9));
}

/* decode bytecode that must fail, with what in the message */
static void expect_bad(const uint32_t *t, uint32_t n, const char *what) {
    struct sm_shader s;
    char err[256] = "";
    if (sm_decode(t, n, &s, err, sizeof err) == 0) { printf("FAIL decoded, want an error naming %s\n", what); fails++; sm_free(&s); return; }
    if (!strstr(err, what)) { printf("FAIL error \"%s\" does not name %s\n", err, what); fails++; }
}
static int asm_text(const char *text, struct sm_shader *s) {
    uint32_t *t, n;
    char *errs, err[256];
    if (sm_assemble(text, strlen(text), NULL, &t, &n, &errs)) { printf("FAIL %s:\n%s", text, errs); fails++; free(errs); return -1; }
    int r = sm_decode(t, n, s, err, sizeof err);
    if (r) { printf("FAIL decode %s: %s\n", text, err); fails++; }
    free(t);
    return r;
}

static void group_decode_roundtrip(const char *dir) {
    char **names;
    int n = list_dir(dir, &names);
    for (int i = 0; i < n; i++) {
        char path[1024], *errs = NULL;
        snprintf(path, sizeof path, "%s/%s", dir, names[i]);
        size_t len;
        char *src = read_file(path, &len);
        uint32_t *t = NULL, nt = 0;
        if (!src) continue;
        if (sm_assemble(src, len, NULL, &t, &nt, &errs)) { printf("FAIL %s:\n%s", names[i], errs); fails++; }
        else if (roundtrip(names[i], t, nt, t, nt)) printf("%s: roundtrip ok\n", names[i]);
        free(t), free(errs), free(src);
    }
    /* the decoded fields */
    struct sm_shader s;
    if (!asm_text("vs_1_1\ndcl_position v0\nm4x4 oPos, v0, c0\n", &s)) {
        CHECKEQ(s.version, 0xFFFE0101u);
        CHECKEQ(s.nins, 2);
        CHECKEQ(s.ntokens, 9);
        CHECK(!s.is_ps && s.major == 1 && s.minor == 1);
        CHECKEQ(s.ins[0].op, SM_DCL);
        CHECKEQ(s.ins[0].usage, SM_USAGE_POSITION);
        CHECKEQ(s.ins[0].dst.type, SM_R_INPUT);
        CHECKEQ(s.ins[0].offset, 1);
        CHECKEQ(s.ins[1].op, SM_M4x4);
        CHECKEQ(s.ins[1].offset, 4);
        CHECK(s.ins[1].ndst == 1 && s.ins[1].nsrc == 2);
        CHECK(s.ins[1].dst.type == SM_R_RASTOUT && s.ins[1].dst.index == SM_RAST_POSITION && s.ins[1].dst.mask_or_swizzle == 0xF);
        CHECK(s.ins[1].src[0].type == SM_R_INPUT && s.ins[1].src[0].mask_or_swizzle == 0xE4);
        CHECK(s.ins[1].src[1].type == SM_R_CONST && s.ins[1].src[1].index == 0 && !s.ins[1].src[1].rel);
        /* FNV-1a 64 over the 36 token bytes */
        uint64_t h = 0xCBF29CE484222325ull;
        for (int k = 0; k < 36; k++) h = (h ^ ((const uint8_t *)g_m4x4)[k]) * 0x100000001B3ull;
        CHECKEQ(s.hash, h);
        sm_free(&s);
        CHECK(s.ins == NULL && s.nins == 0);
    }
    if (!asm_text("vs_1_1\nmov r0, c[a0.x + 5]\nmov oPos, -r0.xz\n", &s)) {
        CHECK(s.ins[0].src[0].rel && s.ins[0].src[0].rel_type == SM_R_ADDR && s.ins[0].src[0].rel_index == 0);
        CHECK(s.ins[0].src[0].index == 5 && s.ins[0].src[0].rel_comp == 0);
        CHECK(s.ins[1].src[0].mod == SM_SRC_NEG && s.ins[1].src[0].mask_or_swizzle == 0xA8);
        sm_free(&s);
    }
    if (!asm_text("vs_2_0\nmov r0, c[a0.w + 2]\nmov r1, c[aL + 3]\nsincos r1.xy, r0.x, c0, c1\ndefi i1, 1, -2, 3, 4\n", &s)) {
        CHECK(s.ins[0].src[0].rel && s.ins[0].src[0].rel_type == SM_R_ADDR && s.ins[0].src[0].rel_comp == 3);
        CHECK(s.ins[1].src[0].rel && s.ins[1].src[0].rel_type == SM_R_LOOP && s.ins[1].src[0].index == 3);
        CHECK(s.ins[2].op == SM_SINCOS && s.ins[2].nsrc == 3 && s.ins[2].dst.mask_or_swizzle == 3);
        CHECK(s.ins[3].op == SM_DEFI && s.ins[3].defi[1] == -2 && s.ins[3].dst.index == 1);
        sm_free(&s);
    }
    if (!asm_text("ps_1_4\ntexld r1, t1\nmul_x4_sat r0.rgb, r1, 1-r1\n+mov_d2 r0.a, -c0_bias\n", &s)) {
        CHECK(s.ins[0].op == SM_TEX && s.ins[0].nsrc == 1 && s.ins[0].src[0].type == SM_R_TEXTURE);
        CHECK(s.ins[1].dst.shift == 2 && s.ins[1].sat && s.ins[1].src[1].mod == SM_SRC_COMP && !s.ins[1].coissue);
        CHECK(s.ins[2].coissue && s.ins[2].dst.shift == -1 && s.ins[2].src[0].mod == SM_SRC_BIASNEG);
        sm_free(&s);
    }
    if (!asm_text("ps_2_0\ndef c3, 0.5, -1, 0, 2\ndcl_volume s2\ndcl_centroid t1.xy\ntexldb_pp r0, t1, s2\n", &s)) {
        CHECK(s.ins[0].op == SM_DEF && s.ins[0].def[0] == 0.5f && s.ins[0].def[1] == -1.0f && s.ins[0].dst.index == 3);
        CHECK(s.ins[1].sampler_type == SM_STT_VOLUME && s.ins[1].dst.type == SM_R_SAMPLER && s.ins[1].dst.index == 2);
        CHECK(s.ins[2].centroid && s.ins[2].dst.type == SM_R_TEXTURE && s.ins[2].dst.mask_or_swizzle == 3);
        CHECK(s.ins[3].control == SM_TEXLD_BIAS && s.ins[3].pp && s.ins[3].nsrc == 2);
        sm_free(&s);
    }
    /* what the decoder refuses */
    expect_bad((const uint32_t[]){ 0xFFFE0300, 0xFFFF }, 2, "vs_3_0");
    expect_bad((const uint32_t[]){ 0xFFFF0300, 0xFFFF }, 2, "ps_3_0");
    expect_bad((const uint32_t[]){ 0xFFFE0201, 0xFFFF }, 2, "vs_2_x");
    expect_bad((const uint32_t[]){ 0xFFFF0201, 0xFFFF }, 2, "ps_2_x");
    expect_bad((const uint32_t[]){ 0xFFFE0100, 0xFFFF }, 2, "vs_1_0");
    expect_bad((const uint32_t[]){ 0x12345678, 0xFFFF }, 2, "version");
    expect_bad((const uint32_t[]){ 0xFFFE0101, 0x00000001, 0x800F0000, 0x90E40000 }, 4, "end");
    expect_bad((const uint32_t[]){ 0xFFFE0200, 0x13000001, 0x800F0000, 0xB0E01000, 0x90E40000, 0xFFFF }, 6, "predicat");
    expect_bad((const uint32_t[]){ 0xFFFF0200, 0x04000050, 0x800F0000, 0x80E40000, 0x80E40000, 0x80E40000, 0xFFFF }, 7, "cnd");
    expect_bad((const uint32_t[]){ 0xFFFE0200, 0x02010029, 0xA0000000, 0xA0000001, 0xFFFF }, 5, "ifc");
    expect_bad((const uint32_t[]){ 0xFFFE0200, 0x03000001, 0x800F0000, 0x90E40000, 0xFFFF }, 5, "length");
    expect_bad((const uint32_t[]){ 0xFFFF0200, 0x02000001, 0xC00F0000, 0x80E40000, 0xFFFF }, 5, "oPos");
    expect_bad((const uint32_t[]){ 0xFFFE0101, 0x00000001, 0x800F0000, 0x94E40000, 0xFFFF }, 5, "bx2");
    expect_bad((const uint32_t[]){ 0xFFFE0101, 0x00000001, 0x801F0000, 0x90E40000, 0xFFFF }, 5, "_sat");
    expect_bad((const uint32_t[]){ 0xFFFE0200, 0x02000001, 0x810F0000, 0x90E40000, 0xFFFF }, 5, "_x2");
    expect_bad((const uint32_t[]){ 0xFFFF0200, 0x02000001, 0x800F000C, 0x80E40000, 0xFFFF }, 5, "r12");
    expect_bad((const uint32_t[]){ 0xFFFF0101, 0x40000001, 0x800F0000, 0x90E40000, 0xFFFF }, 5, "co-issue");
    expect_bad((const uint32_t[]){ 0xFFFE0101, 0x00000001, 0x800F0000, 0x00E40000, 0xFFFF }, 5, "parameter");
    expect_bad((const uint32_t[]){ 0xFFFE0200, 0x02000001, 0x800F2000, 0xB0000000, 0x90E40000, 0xFFFF }, 6, "relative");
    expect_bad((const uint32_t[]){ 0xFFFE0101, 0x0000005D, 0xFFFF }, 3, "texldd");
    expect_bad((const uint32_t[]){ 0xFFFE0101, 0x00000063, 0xFFFF }, 3, "opcode 99");
    /* the error names the token offset */
    expect_bad((const uint32_t[]){ 0xFFFE0101, 0x0000001F, 0x80000000, 0x900F0000, 0x0000005D, 0xFFFF }, 6, "token 4");
}

static void group_asm_game_files(const char *dir) {
    char **names;
    int n = list_dir(dir, &names);
    for (int i = 0; i < n; i++) {
        char path[1024];
        snprintf(path, sizeof path, "%s/%s", dir, names[i]);
        size_t len;
        char *src = read_file(path, &len);
        if (!src) continue;
        uint32_t ps = gmem(4), pe = gmem(4);
        uint32_t hr = call("D3DXAssembleShader", 7, gbytes(src, (uint32_t)len), (uint32_t)len, 0u, 0u, 1u, ps, pe);
        if (hr) {
            const char *e = buf_text(MEM32(pe));
            printf("%s: E_FAIL %.*s\n", names[i], (int)strcspn(e, "\n"), e);
        } else {
            uint32_t b = MEM32(ps), size = vcall(b, B_GetBufferSize, 0);
            const uint32_t *t = hle_ptr(vcall(b, B_GetBufferPointer, 0));
            char vb[16];
            int ok = roundtrip(names[i], t, size / 4, t, size / 4);
            printf("%s: %s %u tokens%s\n", names[i], sm_version_name(t[0], vb, sizeof vb), size / 4,
                   ok ? " roundtrip ok" : " ROUNDTRIP FAILED");
        }
        free(src);
    }
}

static void group_decode_files(const char *dir) {
    char **names;
    int n = list_dir(dir, &names);
    for (int i = 0; i < n; i++) {
        char path[1024], err[256], vb[16];
        snprintf(path, sizeof path, "%s/%s", dir, names[i]);
        size_t len;
        uint32_t *t = (uint32_t *)read_file(path, &len), nt = (uint32_t)(len / 4);
        if (!t) continue;
        struct sm_shader s;
        if (sm_decode(t, nt, &s, err, sizeof err)) { printf("FAIL %s: %s\n", names[i], err); fails++; free(t); continue; }
        /* the tokens without comments: the version, each instruction's own tokens, the end */
        uint32_t *want = malloc(4 * nt), nw = 0;
        want[nw++] = t[0];
        for (uint32_t k = 0; k < s.nins; k++)
            memcpy(want + nw, t + s.ins[k].offset, 4u * s.ins[k].length), nw += s.ins[k].length;
        want[nw++] = 0xFFFF;
        if (roundtrip(names[i], t, nt, want, nw))
            printf("%s: %s roundtrip ok\n", names[i], sm_version_name(t[0], vb, sizeof vb));
        sm_free(&s);
        free(want), free(t);
    }
}

static void group_profiles(void) {
    uint32_t d3d = call("Direct3DCreate9", 1, 32u), pp = gmem(56), out = gmem(4);
    MEM32(pp + 0) = 64, MEM32(pp + 4) = 64, MEM32(pp + 8) = 22, MEM32(pp + 12) = 1, MEM32(pp + 24) = 1;
    MEM32(pp + 28) = 0x10001u, MEM32(pp + 32) = 1;
    CHECKEQ(vcall(d3d, 16, 6, 0u, 1u, 0x10001u, 0x40u, pp, out), D3D_OK);
    uint32_t dev = MEM32(out);
    uint32_t vs = call("D3DXGetVertexShaderProfile", 1, dev), ps = call("D3DXGetPixelShaderProfile", 1, dev);
    CHECK(vs && !strcmp(hle_ptr(vs), "vs_2_0"));
    CHECK(ps && !strcmp(hle_ptr(ps), "ps_2_0"));
    CHECKEQ(call("D3DXGetVertexShaderProfile", 1, 0u), 0);
    CHECKEQ(call("D3DXGetPixelShaderProfile", 1, 0u), 0);
    uint32_t t = gbytes(g_m4x4, sizeof g_m4x4);
    CHECKEQ(call("D3DXGetShaderVersion", 1, t), 0xFFFE0101u);
    MEM32(t) = 0xFFFF0200u;
    CHECKEQ(call("D3DXGetShaderVersion", 1, t), 0xFFFF0200u);
    CHECKEQ(call("D3DXGetShaderVersion", 1, 0u), 0);
}

static void group_asm_loud(const char *what) {
    if (!strcmp(what, "include")) {
        uint32_t sh, er;
        asm_guest("vs_1_1\n#include \"common.vsh\"\nmov oPos, v0\n", 0, &sh, &er);
    } else { printf("FAIL no loud case %s\n", what); fails++; return; }
    printf("asm_loud: returned\n");
}

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: sm_selftest <group> [arg]\n"); return 2; }
    hle_mem_init();
    bw2_register_shims();
    mach_enter();
    g_esp = hle_va(hle_alloc(1u << 20)) + (1u << 20) - 64;
    const char *arg = argc > 2 ? argv[2] : NULL;
    if (!strcmp(argv[1], "asm_tokens") && arg) group_asm_tokens(arg);
    else if (!strcmp(argv[1], "asm_errors")) group_asm_errors();
    else if (!strcmp(argv[1], "decode_roundtrip") && arg) group_decode_roundtrip(arg);
    else if (!strcmp(argv[1], "asm_game_files") && arg) group_asm_game_files(arg);
    else if (!strcmp(argv[1], "decode_files") && arg) group_decode_files(arg);
    else if (!strcmp(argv[1], "profiles")) group_profiles();
    else if (!strcmp(argv[1], "asm_loud") && arg) group_asm_loud(arg);
    else { fprintf(stderr, "sm_selftest: no group %s\n", argv[1]); return 2; }
    printf("%s: %d failed\n", argv[1], fails);
    fflush(stdout);
    return fails != 0;
}
