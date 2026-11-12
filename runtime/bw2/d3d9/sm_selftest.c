/*
 * sm_selftest.c - the shader assembler and decoder (plan 5 Task 2), called
 * directly and through D3DX's imports as lifted code calls them, and vertex
 * (Task 3) and pixel (Task 4) shaders drawn on a headless device. Built with
 * the engine (minus main.c) by tests/test_shaders.py;
 * `sm_selftest <group> [arg]` runs one group and prints "<group>: <n> failed".
 *
 * The roundtrip: assemble (or take bytecode), decode, print the decoded form
 * as assembly, assemble that, and compare the tokens.
 */
#include <dirent.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "win32hle.h"
#include "bw2.h"
#include "d3d_com.h"
#include "sm.h"
#include "ff.h"

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

/* ---- Task 3: vertex shaders on Metal ---- */

/* IDirect3DDevice9 slots */
enum { DV_GetDeviceCaps = 7, DV_Release = 2, DV_GetBackBuffer = 18, DV_CreateTexture = 23, DV_CreateRenderTarget = 28,
       DV_GetRenderTargetData = 32, DV_CreateOffscreenPlainSurface = 36, DV_SetRenderTarget = 37, DV_Clear = 43,
       DV_SetClipPlane = 55, DV_SetRenderState = 57, DV_SetTexture = 65, DV_SetTextureStageState = 67,
       DV_SetSamplerState = 69, DV_DrawPrimitiveUP = 83, DV_CreateVertexDeclaration = 86,
       DV_SetVertexDeclaration = 87, DV_SetFVF = 89, DV_CreateVertexShader = 91, DV_SetVertexShader = 92,
       DV_GetVertexShader = 93, DV_SetVertexShaderConstantF = 94, DV_SetVertexShaderConstantI = 96,
       DV_SetVertexShaderConstantB = 98 };
enum { SF_GetDesc = 12, SF_LockRect, SF_UnlockRect };
enum { TX_LockRect = 19, TX_UnlockRect };
enum { SH_GetDevice = 3, SH_GetFunction };
#define D3DERR_INVALIDCALL_ 0x8876086Cu
#define FMT_A8R8G8B8 21u

static uint32_t f2u(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }

static uint32_t new_device(uint32_t w, uint32_t h) {
    static uint32_t d3d;
    if (!d3d) d3d = call("Direct3DCreate9", 1, 32u);
    uint32_t pp = gmem(56), out = gmem(4);
    MEM32(pp + 0) = w, MEM32(pp + 4) = h, MEM32(pp + 8) = 22, MEM32(pp + 12) = 1, MEM32(pp + 24) = 1;
    MEM32(pp + 28) = 0x10001u, MEM32(pp + 32) = 1, MEM32(pp + 36) = 1, MEM32(pp + 40) = 75;   /* D24S8 */
    CHECKEQ(vcall(d3d, 16, 6, 0u, 1u, 0x10001u, 0x40u, pp, out), D3D_OK);
    return MEM32(out);
}
static void rs(uint32_t dev, uint32_t s, uint32_t v) { CHECKEQ(vcall(dev, DV_SetRenderState, 2, s, v), D3D_OK); }
static void tss(uint32_t dev, uint32_t st, uint32_t s, uint32_t v) {
    CHECKEQ(vcall(dev, DV_SetTextureStageState, 3, st, s, v), D3D_OK);
}
/* an A8R8G8B8 target of the device's size, bound as target 0, the fixed pixel pipeline passing diffuse */
static uint32_t argb_target(uint32_t dev, uint32_t w, uint32_t h) {
    uint32_t out = gmem(4);
    CHECKEQ(vcall(dev, DV_CreateRenderTarget, 8, w, h, FMT_A8R8G8B8, 0u, 0u, 0u, out, 0u), D3D_OK);
    CHECKEQ(vcall(dev, DV_SetRenderTarget, 2, 0u, MEM32(out)), D3D_OK);
    rs(dev, 7, 0);                                           /* no depth test */
    tss(dev, 0, 1, 2), tss(dev, 0, 2, 0), tss(dev, 0, 4, 2), tss(dev, 0, 5, 0);   /* SELECTARG1 DIFFUSE */
    return MEM32(out);
}
static void read_rt(uint32_t dev, uint32_t rt, uint32_t w, uint32_t h, uint32_t *px) {
    uint32_t desc = gmem(32), out = gmem(4);
    CHECKEQ(vcall(rt, SF_GetDesc, 1, desc), D3D_OK);
    CHECKEQ(vcall(dev, DV_CreateOffscreenPlainSurface, 6, w, h, MEM32(desc), 2u, out, 0u), D3D_OK);
    uint32_t s = MEM32(out), lr = gmem(8);
    CHECKEQ(vcall(dev, DV_GetRenderTargetData, 2, rt, s), D3D_OK);
    CHECKEQ(vcall(s, SF_LockRect, 3, lr, 0u, 0x10u), D3D_OK);
    for (uint32_t y = 0; y < h; y++) memcpy(px + y * w, hle_ptr(MEM32(lr + 4) + y * MEM32(lr)), w * 4);
    CHECKEQ(vcall(s, SF_UnlockRect, 0), D3D_OK);
    vcall(s, 2, 0);
}
static void clear(uint32_t dev, uint32_t c) {
    CHECKEQ(vcall(dev, DV_Clear, 6, 0u, 0u, 7u, c, f2u(1.0f), 0u), D3D_OK);
}
/* a vertex declaration from (stream, offset, type, usage, index) rows */
static uint32_t vdecl(uint32_t dev, int n, const uint8_t (*rows)[5]) {
    uint32_t el = gmem(8 * (uint32_t)(n + 1)), out = gmem(4);
    for (int i = 0; i <= n; i++) {
        uint32_t p = el + 8 * (uint32_t)i;
        if (i == n) { MEM16(p) = 0xFF, MEM16(p + 2) = 0, MEM8(p + 4) = 17, MEM8(p + 5) = 0, MEM8(p + 6) = 0, MEM8(p + 7) = 0; break; }
        MEM16(p) = rows[i][0], MEM16(p + 2) = rows[i][1], MEM8(p + 4) = rows[i][2], MEM8(p + 5) = 0;
        MEM8(p + 6) = rows[i][3], MEM8(p + 7) = rows[i][4];
    }
    CHECKEQ(vcall(dev, DV_CreateVertexDeclaration, 2, el, out), D3D_OK);
    return MEM32(out);
}
/* assemble on the host, create through the device */
static uint32_t vs_new(uint32_t dev, const char *src) {
    uint32_t *t = NULL, nt = 0, out = gmem(4);
    char *errs = NULL;
    if (sm_assemble(src, strlen(src), NULL, &t, &nt, &errs)) {
        printf("FAIL does not assemble:\n%s\n%s", src, errs);
        fails++;
        free(errs);
        return 0;
    }
    uint32_t g = gbytes(t, 4 * nt);
    free(t);
    CHECKEQ(vcall(dev, DV_CreateVertexShader, 2, g, out), D3D_OK);
    return MEM32(out);
}
static void vsf(uint32_t dev, uint32_t start, const float *v, uint32_t n) {
    CHECKEQ(vcall(dev, DV_SetVertexShaderConstantF, 3, start, gbytes(v, 16 * n), n), D3D_OK);
}
static void draw_up(uint32_t dev, uint32_t type, uint32_t prims, const void *v, uint32_t nverts, uint32_t stride) {
    CHECKEQ(vcall(dev, DV_DrawPrimitiveUP, 4, type, prims, gbytes(v, nverts * stride), stride), D3D_OK);
}
struct xyz { float x, y, z; };
static const uint8_t g_pos_decl[1][5] = { { 0, 0, 2, 0, 0 } };      /* POSITION float3 */
/* a strip over clip space x0..x1, y0..y1 at z 0.5 (position only) */
static void quad_clip(uint32_t dev, float x0, float y0, float x1, float y1) {
    struct xyz q[4] = { { x0, y1, 0.5f }, { x1, y1, 0.5f }, { x0, y0, 0.5f }, { x1, y0, 0.5f } };
    draw_up(dev, 5, 2, q, 4, sizeof q[0]);
}
static int chan_near(uint32_t px, const float *want, int tol) {
    int c[4] = { (int)(px >> 16 & 0xFF), (int)(px >> 8 & 0xFF), (int)(px & 0xFF), (int)(px >> 24) };
    for (int i = 0; i < 4; i++) {
        float w = want[i] < 0 ? 0 : want[i] > 1 ? 1 : want[i];
        int e = (int)(w * 255.0f + 0.5f);
        if (abs(c[i] - e) > tol) return 0;
    }
    return 1;
}
static uint32_t g_px[64 * 64];
/* draw a full-target quad through the shader and read the centre pixel */
static uint32_t draw_centre(uint32_t dev, uint32_t rt, uint32_t vs) {
    CHECKEQ(vcall(dev, DV_SetVertexShader, 1, vs), D3D_OK);
    clear(dev, 0x00000000u);
    quad_clip(dev, -1, -1, 1, 1);
    read_rt(dev, rt, 64, 64, g_px);
    return g_px[32 * 64 + 32];
}
static void expect_px(const char *what, uint32_t px, const float *want) {
    if (!chan_near(px, want, 1)) {
        printf("FAIL %s: pixel 0x%08X, want (%g, %g, %g, %g)\n", what, px, want[0], want[1], want[2], want[3]);
        fails++;
    }
}
static uint32_t pos_device(uint32_t *rt) {
    uint32_t dev = new_device(64, 64);
    *rt = argb_target(dev, 64, 64);
    uint32_t d = vdecl(dev, 1, g_pos_decl);
    CHECKEQ(vcall(dev, DV_SetVertexDeclaration, 1, d), D3D_OK);
    return dev;
}

static void group_vs_basic(void) {
    uint32_t rt, dev = pos_device(&rt);
    uint32_t vs = vs_new(dev, "vs_1_1\ndcl_position v0\nm4x4 oPos, v0, c0\nmov oD0, c4\n");
    const float c[5][4] = { { 1, 0, 0, 0 }, { 0, 1, 0, 0 }, { 0, 0, 1, 0 }, { 0, 0, 0, 1 }, { 1, 0.5f, 0.25f, 1 } };
    vsf(dev, 0, c[0], 5);
    CHECKEQ(vcall(dev, DV_SetVertexShader, 1, vs), D3D_OK);
    clear(dev, 0xFF000000u);
    quad_clip(dev, -0.5f, -0.5f, 0.5f, 0.5f);
    read_rt(dev, rt, 64, 64, g_px);
    int inside = 0, outside = 0;
    for (int y = 0; y < 64; y++)
        for (int x = 0; x < 64; x++) {
            uint32_t p = g_px[y * 64 + x];
            if (x >= 16 && x < 48 && y >= 16 && y < 48) inside += chan_near(p, c[4], 1);
            else outside += p == 0xFF000000u;
        }
    CHECKEQ(inside, 32 * 32);
    CHECKEQ(outside, 64 * 64 - 32 * 32);
}

/* One instruction (or a few) per case; its result reaches oD0. c0-c3 are set, the rest def'd. */
struct vs_case { const char *name, *src; float c[4][4]; float want[4]; };
#define V11 "vs_1_1\ndcl_position v0\nmov oPos, v0\n"
#define V20 "vs_2_0\ndcl_position v0\nmov oPos, v0\n"
static const struct vs_case g_vs_cases[] = {
    { "add", V11 "add oD0, c0, c1\n", { { 0.1f, 0.2f, 0.3f, 0.4f }, { 0.2f, 0.3f, 0.4f, 0.5f } }, { 0.3f, 0.5f, 0.7f, 0.9f } },
    { "add_neg", V11 "add oD0, c1, -c0\n", { { 0.1f, 0.2f, 0.3f, 0.4f }, { 0.2f, 0.4f, 0.6f, 0.8f } }, { 0.1f, 0.2f, 0.3f, 0.4f } },
    { "sub", V11 "sub oD0, c1, c0\n", { { 0.1f, 0.2f, 0.3f, 0.4f }, { 0.2f, 0.4f, 0.6f, 0.8f } }, { 0.1f, 0.2f, 0.3f, 0.4f } },
    { "swizzle_mask", V11 "mov r0, c1\nmov r0.yw, c0.wzyx\nmov oD0, r0\n",
      { { 0.1f, 0.2f, 0.3f, 0.4f }, { 0.5f, 0.6f, 0.7f, 0.8f } }, { 0.5f, 0.3f, 0.7f, 0.1f } },
    { "abs", V20 "abs oD0, c0\n", { { -0.25f, 0.5f, -0.75f, 1 } }, { 0.25f, 0.5f, 0.75f, 1 } },
    { "dp3", V11 "dp3 oD0, c0, c1\n", { { 0.1f, 0.2f, 0.3f, 0.4f }, { 0.2f, 0.3f, 0.4f, 0.5f } }, { 0.2f, 0.2f, 0.2f, 0.2f } },
    { "dp4", V11 "dp4 oD0, c0, c1\n", { { 0.1f, 0.2f, 0.3f, 0.4f }, { 0.2f, 0.3f, 0.4f, 0.5f } }, { 0.4f, 0.4f, 0.4f, 0.4f } },
    /* vs_1_1 expp: (2^floor(w), frac(w), 2^w, 1) */
    { "expp11", V11 "expp oD0, c0.x\n", { { -0.5f } }, { 0.5f, 0.5f, 0.70710678f, 1 } },
    { "expp20", V20 "expp oD0, c0.x\n", { { -0.5f } }, { 0.70710678f, 0.70710678f, 0.70710678f, 0.70710678f } },
    { "exp", V11 "exp oD0, c0.y\n", { { 0, -1 } }, { 0.5f, 0.5f, 0.5f, 0.5f } },
    { "frc11", V11 "mov r0, c1\nfrc r0.xy, c0\nmov oD0, r0\n", { { 1.25f, -0.25f }, { 0, 0, 0.5f, 1 } }, { 0.25f, 0.75f, 0.5f, 1 } },
    { "frc20", V20 "frc oD0, c0\n", { { 1.25f, -0.25f, 2.5f, 3.75f } }, { 0.25f, 0.75f, 0.5f, 0.75f } },
    { "lrp", V20 "lrp oD0, c0, c1, c2\n", { { 0, 1, 0.5f, 0.25f }, { 0.8f, 0.8f, 0.8f, 0.8f }, { 0.4f, 0.4f, 0.4f, 0.4f } },
      { 0.4f, 0.8f, 0.6f, 0.5f } },
    { "mad", V11 "mad oD0, c0, c1, c2\n", { { 0.5f, 0.5f, 0.5f, 0.5f }, { 0.2f, 0.4f, 0.6f, 0.8f }, { 0.1f, 0.1f, 0.1f, 0.1f } },
      { 0.2f, 0.3f, 0.4f, 0.5f } },
    { "max", V11 "max oD0, c0, c1\n", { { 0.1f, 0.9f, 0.3f, 0.6f }, { 0.2f, 0.3f, 0.4f, 0.5f } }, { 0.2f, 0.9f, 0.4f, 0.6f } },
    { "min", V11 "min oD0, c0, c1\n", { { 0.1f, 0.9f, 0.3f, 0.6f }, { 0.2f, 0.3f, 0.4f, 0.5f } }, { 0.1f, 0.3f, 0.3f, 0.5f } },
    { "mov", V11 "mov oD0, c0\n", { { 0.1f, 0.9f, 0.3f, 0.6f } }, { 0.1f, 0.9f, 0.3f, 0.6f } },
    { "mul", V11 "mul oD0, c0, c1\n", { { 0.5f, 0.5f, 0.25f, 1 }, { 0.2f, 0.4f, 0.8f, 0.6f } }, { 0.1f, 0.2f, 0.2f, 0.6f } },
    { "nrm", V20 "nrm oD0, c0\n", { { 0.3f, 0, 0.4f, 0.25f } }, { 0.6f, 0, 0.8f, 0.5f } },
    { "nrm_zero", V20 "nrm r0, c0\nadd oD0, r0, c1\n", { { 0, 0, 0, 0.5f }, { 0.25f, 0.25f, 0.25f, 0.25f } },
      { 0.25f, 0.25f, 0.25f, 0.25f } },
    { "rcp", V11 "rcp oD0, c0.z\n", { { 0, 0, 4 } }, { 0.25f, 0.25f, 0.25f, 0.25f } },
    /* rcp(0) is +infinity: above 1e30 */
    { "rcp_zero", V11 "rcp r0, c0.x\nslt oD0, c1, r0\n", { { 0 }, { 1e30f, 1e30f, 1e30f, 1e30f } }, { 1, 1, 1, 1 } },
    { "rsq", V11 "rsq oD0, c0.w\n", { { 0, 0, 0, 4 } }, { 0.5f, 0.5f, 0.5f, 0.5f } },
    { "rsq_abs", V11 "rsq oD0, c0.w\n", { { 0, 0, 0, -16 } }, { 0.25f, 0.25f, 0.25f, 0.25f } },
    { "sincos", "vs_2_0\ndcl_position v0\nmov oPos, v0\ndef c10, -1.5500992e-006, -2.1701389e-005, 0.0026041667, 0.00026041668\n"
      "def c11, -0.020833334, -0.12500000, 1, 0.5\nmov r0, c1\nsincos r0.xy, c0.x, c10, c11\nmov oD0, r0\n",
      { { 0.5f }, { 0, 0, 0.25f, 0.75f } }, { 0.87758256f, 0.47942554f, 0.25f, 0.75f } },
    { "slt", V11 "slt oD0, c0, c1\n", { { 0.1f, 0.5f, 0.3f, 0.3f }, { 0.2f, 0.3f, 0.3f, 0.4f } }, { 1, 0, 0, 1 } },
    { "sge", V11 "sge oD0, c0, c1\n", { { 0.1f, 0.5f, 0.3f, 0.3f }, { 0.2f, 0.3f, 0.3f, 0.4f } }, { 0, 1, 1, 0 } },
    /* lit: (1, max(x, 0), x > 0 && y > 0 ? y^w : 0, 1) */
    { "lit", V11 "lit oD0, c0\n", { { 0.5f, 0.25f, 0, 2 } }, { 1, 0.5f, 0.0625f, 1 } },
    { "lit_back", V11 "lit oD0, c0\n", { { -0.5f, 0.25f, 0, 2 } }, { 1, 0, 0, 1 } },
    /* dst: (1, a.y * b.y, a.z, b.w) */
    { "dst", V11 "dst oD0, c0, c1\n", { { 9, 0.5f, 0.25f, 9 }, { 9, 0.8f, 9, 0.4f } }, { 1, 0.4f, 0.25f, 0.4f } },
    { "log", V11 "log oD0, c0.x\n", { { 1.5f } }, { 0.5849625f, 0.5849625f, 0.5849625f, 0.5849625f } },
    { "log_abs", V11 "log oD0, c0.x\n", { { -1.5f } }, { 0.5849625f, 0.5849625f, 0.5849625f, 0.5849625f } },
    /* the matrix macros: rows from consecutive constants (c4-c7 def'd) */
    { "m4x4", V11 "def c4, 0.5, 0, 0, 0\ndef c5, 0, 0.25, 0, 0\ndef c6, 0.1, 0.1, 0.1, 0.1\ndef c7, 0, 0, 0, 1\n"
      "m4x4 oD0, c0, c4\n", { { 1, 1, 1, 0.5f } }, { 0.5f, 0.25f, 0.35f, 0.5f } },
    { "m4x3", V11 "def c4, 0.5, 0, 0, 0\ndef c5, 0, 0.25, 0, 0\ndef c6, 0.1, 0.1, 0.1, 0.1\n"
      "mov r0, c1\nm4x3 r0.xyz, c0, c4\nmov oD0, r0\n", { { 1, 1, 1, 0.5f }, { 0, 0, 0, 0.75f } }, { 0.5f, 0.25f, 0.35f, 0.75f } },
    { "m3x4", V11 "def c4, 0.5, 0, 0, 0\ndef c5, 0, 0.25, 0, 0\ndef c6, 0.1, 0.1, 0.1, 0.1\ndef c7, 0.2, 0.2, 0.2, 1\n"
      "m3x4 oD0, c0, c4\n", { { 1, 1, 1, 0.5f } }, { 0.5f, 0.25f, 0.3f, 0.6f } },
    { "m3x3", V11 "def c4, 0.5, 0, 0, 0\ndef c5, 0, 0.25, 0, 0\ndef c6, 0.1, 0.1, 0.1, 0.1\n"
      "mov r0, c1\nm3x3 r0.xyz, c0, c4\nmov oD0, r0\n", { { 1, 1, 1, 0.5f }, { 0, 0, 0, 0.75f } }, { 0.5f, 0.25f, 0.3f, 0.75f } },
    { "m3x2", V11 "def c4, 0.5, 0, 0, 0\ndef c5, 0, 0.25, 0, 0\n"
      "mov r0, c1\nm3x2 r0.xy, c0, c4\nmov oD0, r0\n", { { 1, 1, 1, 0.5f }, { 0, 0, 0.6f, 0.75f } }, { 0.5f, 0.25f, 0.6f, 0.75f } },
    { "crs", V20 "mov r0, c2\ncrs r0.xyz, c0, c1\nmov oD0, r0\n", { { 0.5f, 0, 0 }, { 0, 0.5f, 0 }, { 0.9f, 0.9f, 0.9f, 0.6f } },
      { 0, 0, 0.25f, 0.6f } },
    /* sgn: -1, 0, 1 mapped to 0, 0.5, 1 */
    { "sgn", V20 "sgn r0, c0, r1, r2\nmad oD0, r0, c1, c1\n", { { -0.5f, 0, 0.7f, 2 }, { 0.5f, 0.5f, 0.5f, 0.5f } },
      { 0, 0.5f, 1, 1 } },
    { "pow", V20 "pow oD0, c0.x, c0.y\n", { { 0.25f, 0.5f } }, { 0.5f, 0.5f, 0.5f, 0.5f } },
    { "pow_abs", V20 "pow oD0, c0.x, c0.y\n", { { -0.25f, 0.5f } }, { 0.5f, 0.5f, 0.5f, 0.5f } },
    /* unwritten colour outputs read as white */
    { "od1_default", "vs_1_1\ndcl_position v0\nmov oPos, v0\nmov oD1, c0\n", { { 0 } }, { 1, 1, 1, 1 } },
};

static void group_vs_ops(void) {
    uint32_t rt, dev = pos_device(&rt);
    for (size_t i = 0; i < sizeof g_vs_cases / sizeof *g_vs_cases; i++) {
        const struct vs_case *k = &g_vs_cases[i];
        uint32_t vs = vs_new(dev, k->src);
        if (!vs) continue;
        vsf(dev, 0, k->c[0], 4);
        uint32_t px = draw_centre(dev, rt, vs);
        expect_px(k->name, px, k->want);
        CHECKEQ(vcall(dev, DV_SetVertexShader, 1, 0u), D3D_OK);
        CHECKEQ(vcall(vs, 2, 0), 0);
    }
}

/* c[a0.x + n] at the ends of a 20-entry array, and outside the register file (zero) */
static void group_vs_reladdr(void) {
    uint32_t rt, dev = pos_device(&rt);
    static float c[256][4];
    for (int i = 0; i < 256; i++)
        c[i][0] = (float)(i % 16) / 16.0f, c[i][1] = (float)(i / 16) / 16.0f, c[i][2] = 0.5f, c[i][3] = 1;
    vsf(dev, 0, c[0], 256);
    uint32_t v20lo = vs_new(dev, V20 "mova a0.x, c100.x\nmov oD0, c[a0.x]\n");
    uint32_t v20hi = vs_new(dev, V20 "mova a0.x, c100.x\nmov oD0, c[a0.x + 236]\n");
    uint32_t v20mid = vs_new(dev, V20 "mova a0.x, c100.y\nmov oD0, c[a0.x + 50]\n");
    uint32_t v11lo = vs_new(dev, V11 "mov a0.x, c100.x\nmov oD0, c[a0.x + 0]\n");
    uint32_t v11hi = vs_new(dev, V11 "mov a0.x, c100.x\nmov oD0, c[a0.x + 236]\n");
    static const float zero[4] = { 0, 0, 0, 0 };
    struct { uint32_t vs; float a; int reg; } k[] = {
        { v20lo, 0, 0 }, { v20lo, 19, 19 }, { v20lo, -1, -1 }, { v20lo, 18.5f, 19 }, { v20lo, -0.4f, 0 },
        { v20lo, -0.5f, -1 }, { v20lo, 1.5f, 2 },
        { v20hi, 0, 236 }, { v20hi, 19, 255 }, { v20hi, 20, -1 }, { v20hi, -1, 235 },
        { v11lo, 19.7f, 19 }, { v11lo, -0.3f, -1 }, { v11lo, 0.9f, 0 },
        { v11hi, 19.7f, 255 }, { v11hi, 20, -1 }, { v11hi, -0.3f, 235 },
    };
    for (size_t i = 0; i < sizeof k / sizeof *k; i++) {
        float idx[4] = { k[i].a, -1, 0, 0 };
        vsf(dev, 100, idx, 1);
        char what[64];
        snprintf(what, sizeof what, "case %zu (a %g)", i, k[i].a);
        expect_px(what, draw_centre(dev, rt, k[i].vs), k[i].reg < 0 ? zero : c[k[i].reg]);
    }
    float idx[4] = { 0, -1, 0, 0 };
    vsf(dev, 100, idx, 1);
    expect_px("mid -1", draw_centre(dev, rt, v20mid), c[49]);
}

static void group_vs_flow(void) {
    uint32_t rt, dev = pos_device(&rt);
    uint32_t vs = vs_new(dev, V20
        "def c20, 0, 0, 0, 0\ndef c21, 0.25, 0, 0, 0\ndef c22, 0, 0.25, 0, 0\ndef c23, 0, 0, 0, 0.125\n"
        "def c24, 0, 0, 0, 0.5\n"
        "mov r0, c20\nif b0\nadd r0, r0, c21\nelse\nadd r0, r0, c22\nendif\n"
        "loop aL, i0\nadd r0, r0, c[aL]\nendloop\n"
        "rep i1\nadd r0, r0, c23\nendrep\n"
        "call l0\nmov oD0, r0\nret\n"
        "label l0\nadd r0, r0, c24\nret\n");
    float c[7][4] = { { 0 } };
    for (int i = 2; i <= 6; i++) c[i][2] = i % 2 ? 0.4f : 0.1f;      /* aL 2, 4, 6 read 0.1 each */
    vsf(dev, 0, c[0], 7);
    int32_t ii[2][4] = { { 3, 2, 2, 0 }, { 2, 0, 0, 0 } };
    CHECKEQ(vcall(dev, DV_SetVertexShaderConstantI, 3, 0u, gbytes(ii, sizeof ii), 2u), D3D_OK);
    uint32_t b = 1;
    CHECKEQ(vcall(dev, DV_SetVertexShaderConstantB, 3, 0u, gbytes(&b, 4), 1u), D3D_OK);
    const float want_t[4] = { 0.25f, 0, 0.3f, 0.75f }, want_f[4] = { 0, 0.25f, 0.3f, 0.75f };
    expect_px("b0 true", draw_centre(dev, rt, vs), want_t);
    b = 0;
    CHECKEQ(vcall(dev, DV_SetVertexShaderConstantB, 3, 0u, gbytes(&b, 4), 1u), D3D_OK);
    expect_px("b0 false", draw_centre(dev, rt, vs), want_f);
    /* a loop count of 0 runs nothing; rep 1 */
    int32_t i0[2][4] = { { 0, 2, 2, 0 }, { 1, 0, 0, 0 } };
    CHECKEQ(vcall(dev, DV_SetVertexShaderConstantI, 3, 0u, gbytes(i0, sizeof i0), 2u), D3D_OK);
    const float want_0[4] = { 0, 0.25f, 0, 0.625f };
    expect_px("loop 0", draw_centre(dev, rt, vs), want_0);
}

/* the declaration's usages to the shader's input registers */
struct vin { float x, y, z, nx, ny, nz, u, v; uint32_t col; uint8_t bi[4]; int16_t s2[2]; };
static void group_vs_inputs(void) {
    uint32_t dev = new_device(64, 64), rt = argb_target(dev, 64, 64);
    static const uint8_t rows[6][5] = { { 0, 0, 2, 0, 0 }, { 0, 12, 2, 3, 0 }, { 0, 24, 1, 5, 1 }, { 0, 32, 4, 10, 0 },
                                        { 0, 36, 5, 2, 0 }, { 0, 40, 6, 5, 3 } };
    uint32_t d = vdecl(dev, 6, rows), dpos = vdecl(dev, 1, g_pos_decl);
    struct vin q[4];
    const float xy[4][2] = { { -1, 1 }, { 1, 1 }, { -1, -1 }, { 1, -1 } };
    for (int i = 0; i < 4; i++)
        q[i] = (struct vin){ xy[i][0], xy[i][1], 0.5f, 0.5f, 0.25f, 0.125f, 0.25f, 0.75f, 0x80FF4000u, { 1, 2, 3, 4 },
                             { 64, 32 } };
    const char *head = "vs_1_1\ndcl_position v0\ndcl_normal v1\ndcl_color v2\ndcl_texcoord1 v3\n"
                       "dcl_blendindices v4\ndcl_texcoord3 v5\nmov oPos, v0\n";
    struct { const char *body; float c0[4]; float want[4]; } k[] = {
        { "mov oD0, v3\n", { 0 }, { 0.25f, 0.75f, 0, 1 } },
        { "mov oD0, v2\n", { 0 }, { 1, 0.25f, 0, 128 / 255.0f } },
        { "mov oD0, v1\n", { 0 }, { 0.5f, 0.25f, 0.125f, 1 } },
        { "mul oD0, v4, c0\n", { 0.125f, 0.125f, 0.125f, 0.125f }, { 0.125f, 0.25f, 0.375f, 0.5f } },
        { "mul oD0, v5, c0\n", { 1 / 128.0f, 1 / 128.0f, 1, 1 }, { 0.5f, 0.25f, 0, 1 } },
    };
    CHECKEQ(vcall(dev, DV_SetVertexDeclaration, 1, d), D3D_OK);
    char src[512];
    for (size_t i = 0; i < sizeof k / sizeof *k; i++) {
        snprintf(src, sizeof src, "%s%s", head, k[i].body);
        uint32_t vs = vs_new(dev, src);
        vsf(dev, 0, k[i].c0, 1);
        CHECKEQ(vcall(dev, DV_SetVertexShader, 1, vs), D3D_OK);
        clear(dev, 0);
        draw_up(dev, 5, 2, q, 4, sizeof q[0]);
        read_rt(dev, rt, 64, 64, g_px);
        expect_px(k[i].body, g_px[32 * 64 + 32], k[i].want);
        CHECKEQ(vcall(dev, DV_SetVertexShader, 1, 0u), D3D_OK);
        vcall(vs, 2, 0);
    }
    /* a dcl with no element reads (0, 0, 0, 1) */
    CHECKEQ(vcall(dev, DV_SetVertexDeclaration, 1, dpos), D3D_OK);
    uint32_t vs = vs_new(dev, "vs_1_1\ndcl_position v0\ndcl_normal v1\nmov oPos, v0\nmov oD0, v1\n");
    const float want[4] = { 0, 0, 0, 1 };
    expect_px("no normal element", draw_centre(dev, rt, vs), want);
}

/* a 1:1 textured quad through a vertex shader: texel centres on pixel centres */
struct xyzuv { float x, y, z, u, v; };
static void group_vs_halfpixel(void) {
    uint32_t dev = new_device(4, 4), out = gmem(4), lr = gmem(8), tx[16], px[16];
    for (int i = 0; i < 16; i++) tx[i] = 0xFF000000u | (uint32_t)(i * 16) << 16 | (uint32_t)(255 - i * 16) << 8 | (uint32_t)(i * 5);
    CHECKEQ(vcall(dev, DV_CreateTexture, 8, 4u, 4u, 1u, 0u, FMT_A8R8G8B8, 1u, out, 0u), D3D_OK);
    uint32_t t = MEM32(out);
    CHECKEQ(vcall(t, TX_LockRect, 4, 0u, lr, 0u, 0u), D3D_OK);
    for (int y = 0; y < 4; y++) memcpy((uint8_t *)hle_ptr(MEM32(lr + 4)) + y * MEM32(lr), tx + 4 * y, 16);
    CHECKEQ(vcall(t, TX_UnlockRect, 1, 0u), D3D_OK);
    CHECKEQ(vcall(dev, DV_SetTexture, 2, 0u, t), D3D_OK);
    CHECKEQ(vcall(dev, DV_SetSamplerState, 3, 0u, 5u, 2u), D3D_OK);     /* linear: an offset would mix texels */
    CHECKEQ(vcall(dev, DV_SetSamplerState, 3, 0u, 6u, 2u), D3D_OK);
    tss(dev, 0, 1, 2), tss(dev, 0, 2, 2), tss(dev, 0, 4, 2), tss(dev, 0, 5, 2);   /* SELECTARG1 TEXTURE */
    static const uint8_t rows[2][5] = { { 0, 0, 2, 0, 0 }, { 0, 12, 1, 5, 0 } };
    CHECKEQ(vcall(dev, DV_SetVertexDeclaration, 1, vdecl(dev, 2, rows)), D3D_OK);
    /* screen coordinates to clip space: x / 2 - 1, 1 - y / 2 */
    uint32_t vs = vs_new(dev, "vs_1_1\ndcl_position v0\ndcl_texcoord v1\ndef c0, 0.5, -0.5, 1, 1\ndef c1, -1, 1, 0, 0\n"
                              "mad oPos, v0, c0, c1\nmov oT0, v1\n");
    CHECKEQ(vcall(dev, DV_SetVertexShader, 1, vs), D3D_OK);
    rs(dev, 7, 0);
    clear(dev, 0xFF000000u);
    struct xyzuv q[4] = { { -0.5f, -0.5f, 0.5f, 0, 0 }, { 3.5f, -0.5f, 0.5f, 1, 0 }, { -0.5f, 3.5f, 0.5f, 0, 1 },
                          { 3.5f, 3.5f, 0.5f, 1, 1 } };
    draw_up(dev, 5, 2, q, 4, sizeof q[0]);
    uint32_t bb = gmem(4);
    CHECKEQ(vcall(dev, DV_GetBackBuffer, 4, 0u, 0u, 0u, bb), D3D_OK);
    read_rt(dev, MEM32(bb), 4, 4, px);
    for (int i = 0; i < 16; i++) {
        int ok = 1;
        for (int s = 0; s < 24; s += 8) ok &= abs((int)(px[i] >> s & 0xFF) - (int)(tx[i] >> s & 0xFF)) <= 1;
        if (!ok) printf("FAIL pixel %d 0x%08X want 0x%08X\n", i, px[i], tx[i]), fails++;
    }
}

/* one user clip plane in clip space keeps x >= -1/64: the right half */
static void group_vs_clip(void) {
    uint32_t rt, dev = pos_device(&rt);
    uint32_t vs = vs_new(dev, V11 "def c0, 1, 1, 1, 1\nmov oD0, c0\n");
    const float plane[4] = { 1, 0, 0, 1 / 64.0f };
    CHECKEQ(vcall(dev, DV_SetClipPlane, 2, 0u, gbytes(plane, 16)), D3D_OK);
    rs(dev, 152, 1);
    draw_centre(dev, rt, vs);
    int white = 0, left = 0;
    for (int y = 0; y < 64; y++)
        for (int x = 0; x < 64; x++) {
            uint32_t p = g_px[y * 64 + x];
            white += p == 0xFFFFFFFFu;
            if (x < 32) left += p == 0;
        }
    CHECKEQ(white, 32 * 64);
    CHECKEQ(left, 32 * 64);
    rs(dev, 152, 0);
    draw_centre(dev, rt, vs);
    int all = 0;
    for (int i = 0; i < 64 * 64; i++) all += g_px[i] == 0xFFFFFFFFu;
    CHECKEQ(all, 64 * 64);
}

static void check_caps(uint32_t caps) {
    CHECKEQ(MEM32(caps + 0xC4), 0xFFFE0200u);                /* VertexShaderVersion */
    CHECKEQ(MEM32(caps + 0xC8), 256);                        /* MaxVertexShaderConst */
    CHECKEQ(MEM32(caps + 0xCC), 0xFFFF0200u);                /* PixelShaderVersion */
    static const uint32_t vs20[4] = { 0, 0, 12, 1 }, ps20[5] = { 0, 0, 12, 0, 96 };
    for (int i = 0; i < 4; i++) CHECKEQ(MEM32(caps + 0xF8 + 4u * (uint32_t)i), vs20[i]);
    for (int i = 0; i < 5; i++) CHECKEQ(MEM32(caps + 0x108 + 4u * (uint32_t)i), ps20[i]);
    CHECKEQ(MEM32(caps + 0x120), 65535);                     /* MaxVShaderInstructionsExecuted */
    CHECKEQ(MEM32(caps + 0x124), 96);                        /* MaxPShaderInstructionsExecuted */
    CHECKEQ(MEM32(caps + 0x128), 0);
    CHECKEQ(MEM32(caps + 0x12C), 0);
}
static void group_caps_sm2(void) {
    uint32_t d3d = call("Direct3DCreate9", 1, 32u), caps = gmem(304);
    CHECKEQ(vcall(d3d, 14, 3, 0u, 1u, caps), D3D_OK);
    check_caps(caps);
    uint32_t dev = new_device(16, 16);
    memset(hle_ptr(caps), 0, 304);
    CHECKEQ(vcall(dev, DV_GetDeviceCaps, 1, caps), D3D_OK);
    check_caps(caps);
}

static uint32_t refs(uint32_t o) { vcall(o, 1, 0); return vcall(o, 2, 0); }

static void group_shader_objects(void) {
    uint32_t rt, dev = pos_device(&rt), out = gmem(4), size = gmem(4);
    /* tokens with a comment block: GetFunction gives them back as given */
    uint32_t *t = NULL, nt = 0;
    char *errs = NULL;
    const char *src = V11 "def c0, 0, 1, 0, 1\nmov oD0, c0\n";
    CHECKEQ(sm_assemble(src, strlen(src), NULL, &t, &nt, &errs), 0);
    uint32_t tok[64];
    tok[0] = t[0], tok[1] = 0xFFFE | 2u << 16, tok[2] = 0x12345678u, tok[3] = 0x9ABCDEF0u;
    memcpy(tok + 4, t + 1, 4 * (nt - 1));
    uint32_t n = nt + 3, g = gbytes(tok, 4 * n);
    free(t);
    CHECKEQ(vcall(dev, DV_CreateVertexShader, 2, g, out), D3D_OK);
    uint32_t vs = MEM32(out);
    CHECK(vs != 0);
    CHECKEQ(refs(vs), 1);
    MEM32(size) = 0;
    CHECKEQ(vcall(vs, SH_GetFunction, 2, 0u, size), D3D_OK);
    CHECKEQ(MEM32(size), 4 * n);
    uint32_t buf = gmem(4 * n);
    MEM32(size) = 4 * n - 4;
    CHECKEQ(vcall(vs, SH_GetFunction, 2, buf, size), D3DERR_INVALIDCALL_);
    MEM32(size) = 4 * n;
    CHECKEQ(vcall(vs, SH_GetFunction, 2, buf, size), D3D_OK);
    CHECK(!memcmp(hle_ptr(buf), tok, 4 * n));
    uint32_t r0 = refs(dev);
    CHECKEQ(vcall(vs, SH_GetDevice, 1, out), D3D_OK);
    CHECKEQ(MEM32(out), dev);
    CHECKEQ(refs(dev), r0 + 1);
    vcall(dev, 2, 0);
    /* bound: the device holds a reference; Get AddRefs */
    CHECKEQ(vcall(dev, DV_SetVertexShader, 1, vs), D3D_OK);
    CHECKEQ(refs(vs), 2);
    MEM32(out) = 0;
    CHECKEQ(vcall(dev, DV_GetVertexShader, 1, out), D3D_OK);
    CHECKEQ(MEM32(out), vs);
    CHECKEQ(refs(vs), 3);
    vcall(vs, 2, 0);
    /* drawn through the shader, then SetVertexShader(NULL) is the fixed pipeline again */
    const float green[4] = { 0, 1, 0, 1 };
    expect_px("shader", draw_centre(dev, rt, vs), green);
    CHECKEQ(vcall(dev, DV_SetVertexShader, 1, 0u), D3D_OK);
    CHECKEQ(refs(vs), 1);
    MEM32(out) = 0xDEAD;
    CHECKEQ(vcall(dev, DV_GetVertexShader, 1, out), D3D_OK);
    CHECKEQ(MEM32(out), 0);
    struct { float x, y, z, rhw; uint32_t c; } q[4] = { { -0.5f, -0.5f, 0.5f, 1, 0xFFFF0000u },
        { 63.5f, -0.5f, 0.5f, 1, 0xFFFF0000u }, { -0.5f, 63.5f, 0.5f, 1, 0xFFFF0000u }, { 63.5f, 63.5f, 0.5f, 1, 0xFFFF0000u } };
    CHECKEQ(vcall(dev, DV_SetFVF, 1, 0x044u), D3D_OK);
    clear(dev, 0);
    draw_up(dev, 5, 2, q, 4, sizeof q[0]);
    read_rt(dev, rt, 64, 64, g_px);
    CHECKEQ(g_px[32 * 64 + 32], 0xFFFF0000u);
    /* bad calls */
    CHECKEQ(vcall(dev, DV_CreateVertexShader, 2, 0u, out), D3DERR_INVALIDCALL_);
    CHECKEQ(vcall(dev, DV_CreateVertexShader, 2, g, 0u), D3DERR_INVALIDCALL_);
    static const uint32_t ps[] = { 0xFFFF0200u, 0x02000001, 0x800F0800, 0xA0E40000, 0x0000FFFF };
    CHECKEQ(vcall(dev, DV_CreateVertexShader, 2, gbytes(ps, sizeof ps), out), D3DERR_INVALIDCALL_);
    CHECKEQ(vcall(vs, SH_GetFunction, 2, buf, 0u), D3DERR_INVALIDCALL_);
    CHECKEQ(vcall(vs, 2, 0), 0);
}

/* the same shader on a second device: no new MSL compile; after a cache drop, the pipeline from the archive */
static void cache_draw(void) {
    uint32_t dev = new_device(64, 64), bb = gmem(4);
    rs(dev, 7, 0);
    tss(dev, 0, 1, 2), tss(dev, 0, 2, 0), tss(dev, 0, 4, 2), tss(dev, 0, 5, 0);
    uint32_t d = vdecl(dev, 1, g_pos_decl);
    CHECKEQ(vcall(dev, DV_SetVertexDeclaration, 1, d), D3D_OK);
    uint32_t vs = vs_new(dev, V20 "def c5, 0.5, 0.25, 1, 1\nmul oD0, c0, c5\n");
    const float c0[4] = { 1, 1, 1, 1 }, want[4] = { 0.5f, 0.25f, 1, 1 };
    vsf(dev, 0, c0, 1);
    CHECKEQ(vcall(dev, DV_GetBackBuffer, 4, 0u, 0u, 0u, bb), D3D_OK);
    expect_px("cache", draw_centre(dev, MEM32(bb), vs), want);
    CHECKEQ(vcall(dev, DV_SetVertexShader, 1, 0u), D3D_OK);
    CHECKEQ(vcall(dev, DV_SetVertexDeclaration, 1, 0u), D3D_OK);
    vcall(vs, 2, 0), vcall(d, 2, 0), vcall(MEM32(bb), 2, 0);
    CHECKEQ(vcall(dev, 2, 0), 0);                            /* the release saves the archive */
}
static void group_vs_cache(void) {
    struct mtl_stats s0, s1, s2;
    cache_draw();
    mtl_get_stats(&s0);
    CHECK(s0.libraries >= 2 && s0.pipelines >= 1);
    cache_draw();
    mtl_get_stats(&s1);
    CHECKEQ(s1.libraries, s0.libraries);
    CHECKEQ(s1.pipelines, s0.pipelines);
    char path[1024];
    snprintf(path, sizeof path, "%s/ShaderCache", getenv("BW2_SUPPORT"));
    mtl_pipeline_cache_drop();
    mtl_archive_open(path);
    mtl_archive_require_hits(1);
    cache_draw();
    mtl_get_stats(&s2);
    CHECK(s2.archive_hits > s1.archive_hits);
    CHECKEQ(s2.archive_misses, s1.archive_misses);
    mtl_archive_require_hits(0);
}

/* shader model 3.0 is not this device's: creating one stops the run, naming it */
static void group_vs_loud(void) {
    uint32_t dev = new_device(16, 16), out = gmem(4);
    static const uint32_t vs30[] = { 0xFFFE0300u, 0x0200001F, 0x80000000, 0x900F0000, 0x0200001F, 0x80000000,
                                     0xE00F0000, 0x02000001, 0xE00F0000, 0x90E40000, 0x0000FFFF };
    vcall(dev, DV_CreateVertexShader, 2, gbytes(vs30, sizeof vs30), out);
    printf("vs_loud: returned\n");
}

/* unbalanced flow control (an end with no open block of its kind) stops the run, naming it */
static void group_flow_loud(const char *which) {
    uint32_t dev = new_device(16, 16), out = gmem(4);
    uint32_t t[16], n = 0;
    t[n++] = 0xFFFE0200u;
    t[n++] = 0x0200001Fu, t[n++] = 0x80000000u, t[n++] = 0x900F0000u;         /* dcl_position v0 */
    if (!strcmp(which, "rep_endif")) t[n++] = 0x01000026u, t[n++] = 0xF0E40000u;  /* rep i0 */
    if (!strcmp(which, "if_endloop") || !strcmp(which, "open")) t[n++] = 0x01000028u, t[n++] = 0xE0E40800u; /* if b0 */
    if (strcmp(which, "open")) {
        t[n++] = !strcmp(which, "endrep") ? 0x00000027u : !strcmp(which, "if_endloop") ? 0x0000001Du : 0x0000002Bu;
        t[n++] = 0x0000002Bu;                                                   /* endif */
    }
    t[n++] = 0x02000001u, t[n++] = 0xC00F0000u, t[n++] = 0x90E40000u;          /* mov oPos, v0 */
    t[n++] = 0x0000FFFFu;
    vcall(dev, DV_CreateVertexShader, 2, gbytes(t, 4 * n), out);
    printf("flow_loud: returned\n");
}

/* ---- Task 4: pixel shaders on Metal ---- */

enum { DV_CreateCubeTexture = 25, DV_CreatePixelShader = 106, DV_SetPixelShader = 107, DV_GetPixelShader = 108,
       DV_SetPixelShaderConstantF = 109 };
enum { CT_LockRect = 19, CT_UnlockRect };
#define U8(x) ((float)(x) / 255.0f)

/* pretransformed vertices: XYZRHW, DIFFUSE, SPECULAR and two 4-component texture coordinate sets */
#define PV_FVF 0xA02C4u
struct pv { float x, y, z, rhw; uint32_t c0, c1; float t0[4], t1[4]; };
#define PV_DIFFUSE 0x80C04020u
#define PV_SPECULAR 0x40FF8000u

static uint32_t ps_new(uint32_t dev, const char *src) {
    uint32_t *t = NULL, nt = 0, out = gmem(4);
    char *errs = NULL;
    if (sm_assemble(src, strlen(src), NULL, &t, &nt, &errs)) {
        printf("FAIL does not assemble:\n%s\n%s", src, errs);
        fails++;
        free(errs);
        return 0;
    }
    uint32_t g = gbytes(t, 4 * nt);
    free(t);
    CHECKEQ(vcall(dev, DV_CreatePixelShader, 2, g, out), D3D_OK);
    return MEM32(out);
}
static void psf(uint32_t dev, uint32_t start, const float *v, uint32_t n) {
    CHECKEQ(vcall(dev, DV_SetPixelShaderConstantF, 3, start, gbytes(v, 16 * n), n), D3D_OK);
}
/* the whole 64x64 target at z 0.5; t0 runs from t0a (left edge) to t0b (right edge), t1 is constant */
static void pquad(uint32_t dev, const float *t0a, const float *t0b, const float *t1) {
    static const float zero[4];
    struct pv q[4];
    const float xy[4][2] = { { -0.5f, -0.5f }, { 63.5f, -0.5f }, { -0.5f, 63.5f }, { 63.5f, 63.5f } };
    for (int i = 0; i < 4; i++) {
        q[i] = (struct pv){ xy[i][0], xy[i][1], 0.5f, 1, PV_DIFFUSE, PV_SPECULAR, { 0 }, { 0 } };
        memcpy(q[i].t0, i & 1 ? (t0b ? t0b : zero) : (t0a ? t0a : zero), 16);
        memcpy(q[i].t1, t1 ? t1 : zero, 16);
    }
    CHECKEQ(vcall(dev, DV_SetFVF, 1, PV_FVF), D3D_OK);
    draw_up(dev, 5, 2, q, 4, sizeof q[0]);
}
static uint32_t tex2d(uint32_t dev, uint32_t w, uint32_t h, const uint32_t *px) {
    uint32_t out = gmem(4), lr = gmem(8);
    CHECKEQ(vcall(dev, DV_CreateTexture, 8, w, h, 1u, 0u, FMT_A8R8G8B8, 1u, out, 0u), D3D_OK);
    uint32_t t = MEM32(out);
    CHECKEQ(vcall(t, TX_LockRect, 4, 0u, lr, 0u, 0u), D3D_OK);
    for (uint32_t y = 0; y < h; y++) memcpy((uint8_t *)hle_ptr(MEM32(lr + 4)) + y * MEM32(lr), px + w * y, 4 * w);
    CHECKEQ(vcall(t, TX_UnlockRect, 1, 0u), D3D_OK);
    return t;
}
/* a 1x1 cube texture, one colour per face (+X -X +Y -Y +Z -Z) */
static uint32_t texcube(uint32_t dev, const uint32_t *faces) {
    uint32_t out = gmem(4), lr = gmem(8);
    CHECKEQ(vcall(dev, DV_CreateCubeTexture, 7, 1u, 1u, 0u, FMT_A8R8G8B8, 1u, out, 0u), D3D_OK);
    uint32_t t = MEM32(out);
    for (uint32_t f = 0; f < 6; f++) {
        CHECKEQ(vcall(t, CT_LockRect, 5, f, 0u, lr, 0u, 0u), D3D_OK);
        MEM32(MEM32(lr + 4)) = faces[f];
        CHECKEQ(vcall(t, CT_UnlockRect, 2, f, 0u), D3D_OK);
    }
    return t;
}
static void px_float(uint32_t px, float *f) {
    f[0] = U8(px >> 16 & 0xFF), f[1] = U8(px >> 8 & 0xFF), f[2] = U8(px & 0xFF), f[3] = U8(px >> 24);
}
/* one pixel shader drawn over the target through pretransformed vertices; the centre pixel */
static uint32_t ps_draw(uint32_t dev, uint32_t rt, uint32_t ps, const float *t0a, const float *t0b, const float *t1) {
    CHECKEQ(vcall(dev, DV_SetPixelShader, 1, ps), D3D_OK);
    clear(dev, 0xFF0000FFu);
    pquad(dev, t0a, t0b, t1);
    read_rt(dev, rt, 64, 64, g_px);
    return g_px[32 * 64 + 32];
}
static uint32_t ps_run(uint32_t dev, uint32_t rt, const char *src, const float (*c)[4], int nc, const float *t0,
                       const float *t1) {
    uint32_t ps = ps_new(dev, src);
    if (!ps) return 0;
    if (nc) psf(dev, 0, c[0], (uint32_t)nc);
    uint32_t px = ps_draw(dev, rt, ps, t0, t0, t1);
    CHECKEQ(vcall(dev, DV_SetPixelShader, 1, 0u), D3D_OK);
    CHECKEQ(vcall(ps, 2, 0), 0);
    return px;
}
static uint32_t ps_device(uint32_t *rt) {
    uint32_t dev = new_device(64, 64);
    *rt = argb_target(dev, 64, 64);
    return dev;
}

/* Each ps instruction the game uses: c0-c3 set, v0 PV_DIFFUSE, v1 PV_SPECULAR. */
#define P20 "ps_2_0\n"
static const struct vs_case g_ps_cases[] = {
    { "add", "ps_1_1\nadd r0, c0, c1\n", { { 0.1f, 0.2f, 0.3f, 0.4f }, { 0.2f, 0.3f, 0.4f, 0.5f } }, { 0.3f, 0.5f, 0.7f, 0.9f } },
    { "sub", "ps_1_1\nsub r0, c1, c0\n", { { 0.1f, 0.2f, 0.3f, 0.4f }, { 0.2f, 0.4f, 0.6f, 0.8f } }, { 0.1f, 0.2f, 0.3f, 0.4f } },
    { "mul", "ps_1_1\nmul r0, c0, c1\n", { { 0.5f, 0.5f, 0.25f, 1 }, { 0.2f, 0.4f, 0.8f, 0.6f } }, { 0.1f, 0.2f, 0.2f, 0.6f } },
    { "mad", "ps_1_1\nmad r0, c0, c1, c2\n", { { 0.5f, 0.5f, 0.5f, 0.5f }, { 0.2f, 0.4f, 0.6f, 0.8f }, { 0.1f, 0.1f, 0.1f, 0.1f } },
      { 0.2f, 0.3f, 0.4f, 0.5f } },
    { "mov", "ps_1_1\nmov r0, c0\n", { { 0.1f, 0.9f, 0.3f, 0.6f } }, { 0.1f, 0.9f, 0.3f, 0.6f } },
    { "mov_v0", "ps_1_1\nmov r0, v0\n", { { 0 } }, { U8(0xC0), U8(0x40), U8(0x20), U8(0x80) } },
    { "mov_v1", "ps_1_1\nmov r0, v1\n", { { 0 } }, { 1, U8(0x80), 0, U8(0x40) } },
    { "lrp", "ps_1_1\nlrp r0, c0, c1, c2\n", { { 0, 1, 0.5f, 0.25f }, { 0.8f, 0.8f, 0.8f, 0.8f }, { 0.4f, 0.4f, 0.4f, 0.4f } },
      { 0.4f, 0.8f, 0.6f, 0.5f } },
    { "dp3", "ps_1_1\ndp3 r0, c0, c1\n", { { 0.1f, 0.2f, 0.3f, 0.4f }, { 0.2f, 0.3f, 0.4f, 0.5f } }, { 0.2f, 0.2f, 0.2f, 0.2f } },
    { "def11", "ps_1_1\ndef c5, 0.5, 0.25, 0.125, 1\nmov r0, c5\n", { { 0 } }, { 0.5f, 0.25f, 0.125f, 1 } },
    /* 1.1-1.3 cnd reads r0.a only; 1.4 tests each component */
    { "cnd11_true", "ps_1_1\nmov r0, c0\ncnd r0, r0.a, c1, c2\n",
      { { 0, 0, 0, 0.75f }, { 0.1f, 0.2f, 0.3f, 0.4f }, { 0.5f, 0.6f, 0.7f, 0.8f } }, { 0.1f, 0.2f, 0.3f, 0.4f } },
    { "cnd11_half", "ps_1_1\nmov r0, c0\ncnd r0, r0.a, c1, c2\n",
      { { 1, 1, 1, 0.5f }, { 0.1f, 0.2f, 0.3f, 0.4f }, { 0.5f, 0.6f, 0.7f, 0.8f } }, { 0.5f, 0.6f, 0.7f, 0.8f } },
    { "cnd14", "ps_1_4\ncnd r0, c0, c1, c2\n",
      { { 0.75f, 0.25f, 0.6f, 0.5f }, { 0.1f, 0.2f, 0.3f, 0.4f }, { 0.5f, 0.6f, 0.7f, 0.8f } }, { 0.1f, 0.6f, 0.3f, 0.8f } },
    { "cmp12", "ps_1_2\ncmp r0, c0, c1, c2\n",
      { { 0.5f, -0.5f, 0, -0.01f }, { 0.1f, 0.2f, 0.3f, 0.4f }, { 0.5f, 0.6f, 0.7f, 0.8f } }, { 0.1f, 0.6f, 0.3f, 0.8f } },
    { "dp4_14", "ps_1_4\ndp4 r0, c0, c1\n", { { 0.1f, 0.2f, 0.3f, 0.4f }, { 0.2f, 0.3f, 0.4f, 0.5f } }, { 0.4f, 0.4f, 0.4f, 0.4f } },
    /* ps 1.x constants are clamped to [-1, 1]; 2.0's are not */
    { "const_clamp11", "ps_1_1\nmul r0, c0, c1\n", { { 2, 2, -2, 2 }, { 0.25f, 0.25f, 0.25f, 0.25f } }, { 0.25f, 0.25f, 0, 0.25f } },
    { "const20", P20 "mul r0, c0, c1\nmov oC0, r0\n", { { 2, 2, 2, 2 }, { 0.25f, 0.125f, 0.25f, 0.25f } }, { 0.5f, 0.25f, 0.5f, 0.5f } },
    { "abs", P20 "abs r0, c0\nmov oC0, r0\n", { { -0.25f, 0.5f, -0.75f, 1 } }, { 0.25f, 0.5f, 0.75f, 1 } },
    { "dp2add", P20 "dp2add r0, c0, c1, c2.x\nmov oC0, r0\n", { { 0.5f, 0.25f, 9, 9 }, { 0.4f, 0.8f, 9, 9 }, { 0.1f } },
      { 0.5f, 0.5f, 0.5f, 0.5f } },
    { "dp3_20", P20 "dp3 r0, c0, c1\nmov oC0, r0\n", { { 0.1f, 0.2f, 0.3f, 0.4f }, { 0.2f, 0.3f, 0.4f, 0.5f } }, { 0.2f, 0.2f, 0.2f, 0.2f } },
    { "dp4", P20 "dp4 r0, c0, c1\nmov oC0, r0\n", { { 0.1f, 0.2f, 0.3f, 0.4f }, { 0.2f, 0.3f, 0.4f, 0.5f } }, { 0.4f, 0.4f, 0.4f, 0.4f } },
    { "max", P20 "max r0, c0, c1\nmov oC0, r0\n", { { 0.1f, 0.9f, 0.3f, 0.6f }, { 0.2f, 0.3f, 0.4f, 0.5f } }, { 0.2f, 0.9f, 0.4f, 0.6f } },
    { "min", P20 "min r0, c0, c1\nmov oC0, r0\n", { { 0.1f, 0.9f, 0.3f, 0.6f }, { 0.2f, 0.3f, 0.4f, 0.5f } }, { 0.1f, 0.3f, 0.3f, 0.5f } },
    { "nrm", P20 "nrm r0, c0\nmov oC0, r0\n", { { 0.3f, 0, 0.4f, 0.25f } }, { 0.6f, 0, 0.8f, 0.5f } },
    { "pow", P20 "pow r0, c0.x, c0.y\nmov oC0, r0\n", { { 0.25f, 0.5f } }, { 0.5f, 0.5f, 0.5f, 0.5f } },
    { "rsq", P20 "rsq r0, c0.w\nmov oC0, r0\n", { { 0, 0, 0, 4 } }, { 0.5f, 0.5f, 0.5f, 0.5f } },
    { "rcp", P20 "rcp r0, c0.z\nmov oC0, r0\n", { { 0, 0, 4 } }, { 0.25f, 0.25f, 0.25f, 0.25f } },
    { "cmp20", P20 "cmp r0, c0, c1, c2\nmov oC0, r0\n",
      { { 0.5f, -0.5f, 0, -0.01f }, { 0.1f, 0.2f, 0.3f, 0.4f }, { 0.5f, 0.6f, 0.7f, 0.8f } }, { 0.1f, 0.6f, 0.3f, 0.8f } },
    { "lrp20", P20 "lrp r0, c0, c1, c2\nmov oC0, r0\n",
      { { 0, 1, 0.5f, 0.25f }, { 0.8f, 0.8f, 0.8f, 0.8f }, { 0.4f, 0.4f, 0.4f, 0.4f } }, { 0.4f, 0.8f, 0.6f, 0.5f } },
    { "mad20", P20 "mad_pp r0, c0, c1, c2\nmov oC0, r0\n",
      { { 0.5f, 0.5f, 0.5f, 0.5f }, { 0.2f, 0.4f, 0.6f, 0.8f }, { 0.1f, 0.1f, 0.1f, 0.1f } }, { 0.2f, 0.3f, 0.4f, 0.5f } },
    { "add_neg20", P20 "add r0, c1, -c0\nmov oC0, r0\n", { { 0.1f, 0.2f, 0.3f, 0.4f }, { 0.2f, 0.4f, 0.6f, 0.8f } },
      { 0.1f, 0.2f, 0.3f, 0.4f } },
    { "v0_20", P20 "dcl v0\ndcl v1\nadd r0, v0, v1\nmul oC0, r0, c0\n", { { 0.5f, 0.5f, 0.5f, 0.5f } },
      { 0.5f * (U8(0xC0) + 1), 0.5f * (U8(0x40) + U8(0x80)), 0.5f * U8(0x20), 0.5f * (U8(0x80) + U8(0x40)) } },
    { "def20", P20 "def c5, 0.5, 0.25, 0.125, 1\nmov oC0, c5\n", { { 0 } }, { 0.5f, 0.25f, 0.125f, 1 } },
    { "sat20", P20 "add_sat r0, c0, c0\nmul oC0, r0, c1\n", { { 0.75f, 0.25f, 0, 1 }, { 0.5f, 0.5f, 0.5f, 0.5f } },
      { 0.5f, 0.25f, 0, 0.5f } },
};

static void group_ps_ops(void) {
    uint32_t rt, dev = ps_device(&rt);
    for (size_t i = 0; i < sizeof g_ps_cases / sizeof *g_ps_cases; i++) {
        const struct vs_case *k = &g_ps_cases[i];
        expect_px(k->name, ps_run(dev, rt, k->src, k->c, 4, NULL, NULL), k->want);
    }
}

/* the source modifiers on a texture read, the shifts, saturate and co-issue */
static void group_ps_modifiers(void) {
    uint32_t rt, dev = ps_device(&rt);
    const uint32_t texel = 0xA0C0E060u;                        /* a r g b */
    const float t[4] = { U8(0xC0), U8(0xE0), U8(0x60), U8(0xA0) };
    CHECKEQ(vcall(dev, DV_SetTexture, 2, 0u, tex2d(dev, 1, 1, &texel)), D3D_OK);
    struct { const char *name, *src; float c[2][4]; float want[4]; } k[16];
    int n = 0;
#define K(NAME, SRC, C0, C1, W) do { k[n].name = NAME, k[n].src = SRC; \
        const float c0_[4] = C0, c1_[4] = C1, w_[4] = W; memcpy(k[n].c[0], c0_, 16), memcpy(k[n].c[1], c1_, 16); \
        memcpy(k[n].want, w_, 16); n++; } while (0)
#define F4(a, b, c, d) { a, b, c, d }
#define ALL4(e) { e(0), e(1), e(2), e(3) }
#define BIAS(i) (t[i] - 0.5f)
#define BX2(i) (2 * t[i] - 1)
#define COMP(i) (1 - t[i])
#define NEGBX2(i) (2 - 2 * t[i])
#define HALF(i) (0.5f * t[i])
#define SAT2(i) (0.5f * (2 * t[i] > 1 ? 1 : 2 * t[i]))
    K("bias", "ps_1_1\ntex t0\nmov r0, t0_bias\n", F4(0, 0, 0, 0), F4(0, 0, 0, 0), ALL4(BIAS));
    K("bx2", "ps_1_1\ntex t0\nmov r0, t0_bx2\n", F4(0, 0, 0, 0), F4(0, 0, 0, 0), ALL4(BX2));
    K("neg_bias", "ps_1_1\ntex t0\nadd r0, c0, -t0_bias\n", F4(0.5f, 0.5f, 0.5f, 0.5f), F4(0, 0, 0, 0), ALL4(COMP));
    K("neg_bx2", "ps_1_1\ntex t0\nadd r0, c0, -t0_bx2\n", F4(1, 1, 1, 1), F4(0, 0, 0, 0), ALL4(NEGBX2));
    K("comp", "ps_1_1\ntex t0\nmov r0, 1-t0\n", F4(0, 0, 0, 0), F4(0, 0, 0, 0), ALL4(COMP));
    K("negate", "ps_1_1\ntex t0\nadd r0, c0, -t0\n", F4(1, 1, 1, 1), F4(0, 0, 0, 0), ALL4(COMP));
    K("x2", "ps_1_4\ntexld r0, t0\nmul r0, r0_x2, c0\n", F4(0.5f, 0.25f, 1, 0.5f), F4(0, 0, 0, 0),
      F4(t[0], 0.5f * t[1], 2 * t[2], t[3]));
    K("abs", "ps_2_0\ndcl t0\ndcl_2d s0\ntexld r0, t0, s0\nadd r0, r0, c0\nabs r0, r0\nmov oC0, r0\n",
      F4(-1, -1, -1, -1), F4(0, 0, 0, 0), ALL4(COMP));
    K("shift_x2", "ps_1_1\ntex t0\nmul_x2 r0, t0, c0\n", F4(0.25f, 0.25f, 0.25f, 0.25f), F4(0, 0, 0, 0), ALL4(HALF));
    K("shift_x4", "ps_1_1\ntex t0\nmul_x4 r0, t0, c0\n", F4(0.25f, 0.25f, 0.25f, 0.25f), F4(0, 0, 0, 0), F4(t[0], t[1], t[2], t[3]));
    K("shift_d2", "ps_1_1\ntex t0\nmov_d2 r0, t0\n", F4(0, 0, 0, 0), F4(0, 0, 0, 0), ALL4(HALF));
    K("shift_x8_d8", "ps_1_4\nmov_x8 r1, c0\nmov_d8 r0, r1\n", F4(0.1f, 0.05f, 0.025f, 0.125f), F4(0, 0, 0, 0),
      F4(0.1f, 0.05f, 0.025f, 0.125f));
    K("saturate", "ps_1_1\ntex t0\nadd_sat r1, t0, t0\nmul r0, r1, c0\n", F4(0.5f, 0.5f, 0.5f, 0.5f), F4(0, 0, 0, 0), ALL4(SAT2));
    K("coissue", "ps_1_1\ntex t0\nmul r0.rgb, t0, c0\n+mov r0.a, c1\n", F4(0.5f, 0.5f, 0.5f, 0.5f), F4(0, 0, 0, 0.3f),
      F4(0.5f * t[0], 0.5f * t[1], 0.5f * t[2], 0.3f));
    K("coissue14", "ps_1_4\ntexld r0, t0\nmul r0.rgb, r0, c0\n+add r0.a, r0, c1\n", F4(0.5f, 0.5f, 0.5f, 0.5f),
      F4(0, 0, 0, -0.5f), F4(0.5f * t[0], 0.5f * t[1], 0.5f * t[2], t[3] - 0.5f));
    const float t0[4] = { 0.5f, 0.5f, 0, 1 };
    for (int i = 0; i < n; i++) expect_px(k[i].name, ps_run(dev, rt, k[i].src, k[i].c, 2, t0, NULL), k[i].want);
    /* _dz and _dw divide the coordinates before ps_1_4 reads the texture (2x2, point sampled) */
    static const uint32_t tx2[4] = { 0xFF102030u, 0xFF405060u, 0xFF708090u, 0xFFA0B0C0u };
    CHECKEQ(vcall(dev, DV_SetTexture, 2, 0u, tex2d(dev, 2, 2, tx2)), D3D_OK);
    float want[4];
    px_float(tx2[1], want);
    const float tw[4] = { 2.25f, 0.75f, 0, 3 }, tz[4] = { 2.25f, 0.75f, 3, 0 };
    expect_px("dw", ps_run(dev, rt, "ps_1_4\ntexld r0, t0_dw.xyw\n", NULL, 0, tw, NULL), want);
    expect_px("dz", ps_run(dev, rt, "ps_1_4\ntexld r0, t0_dz.xyz\n", NULL, 0, tz, NULL), want);
    expect_px("dw texcrd", ps_run(dev, rt, "ps_1_4\ntexcrd r1.rgb, t0_dw.xyw\nphase\ntexld r0, r1\n", NULL, 0,
                                  tw, NULL), want);
#undef K
}

static void group_ps_tex(void) {
    uint32_t rt, dev = ps_device(&rt);
    static const uint32_t tx2[4] = { 0xFF102030u, 0xFF405060u, 0xFF708090u, 0xFFA0B0C0u };
    static const uint32_t faces[6] = { 0xFFFF0000u, 0xFF00FF00u, 0xFF0000FFu, 0xFFFFFF00u, 0xFF00FFFFu, 0xFFFF00FFu };
    uint32_t t2 = tex2d(dev, 2, 2, tx2), tc = texcube(dev, faces);
    float want[4];
    const float c10[4] = { 0.75f, 0.25f, 0, 1 }, c01[4] = { 0.25f, 0.75f, 0, 1 }, c00[4] = { 0.25f, 0.25f, 0, 1 };
    /* ps_1_1 tex: the stage's texture with the set's coordinates, 2D and cube (a variant each) */
    CHECKEQ(vcall(dev, DV_SetTexture, 2, 0u, t2), D3D_OK);
    px_float(tx2[1], want);
    expect_px("tex 2d", ps_run(dev, rt, "ps_1_1\ntex t0\nmov r0, t0\n", NULL, 0, c10, NULL), want);
    CHECKEQ(vcall(dev, DV_SetTexture, 2, 0u, tc), D3D_OK);
    const float dy[4] = { 0, -1, 0.2f, 1 }, dx[4] = { 1, 0.1f, -0.2f, 1 };
    px_float(faces[3], want);
    expect_px("tex cube -y", ps_run(dev, rt, "ps_1_1\ntex t0\nmov r0, t0\n", NULL, 0, dy, NULL), want);
    px_float(faces[0], want);
    expect_px("tex cube +x", ps_run(dev, rt, "ps_1_1\ntex t0\nmov r0, t0\n", NULL, 0, dx, NULL), want);
    CHECKEQ(vcall(dev, DV_SetTexture, 2, 0u, t2), D3D_OK);
    /* ps_1_4: phase 1 copies the coordinates, phase 2 reads through them */
    px_float(tx2[2], want);
    expect_px("ps_1_4 phase", ps_run(dev, rt, "ps_1_4\ntexcrd r1.rgb, t0\nphase\ntexld r0, r1\n", NULL, 0, c01, NULL),
              want);
    px_float(tx2[1], want);
    expect_px("ps_1_4 texld", ps_run(dev, rt, "ps_1_4\ntexld r0, t0\n", NULL, 0, c10, NULL), want);
    /* ps_2_0 texld and texldp */
    px_float(tx2[0], want);
    expect_px("texld", ps_run(dev, rt, P20 "dcl t0\ndcl_2d s0\ntexld r0, t0, s0\nmov oC0, r0\n", NULL, 0, c00, NULL), want);
    const float proj[4] = { 0.75f, 2.25f, 0, 3 };
    px_float(tx2[2], want);
    expect_px("texldp", ps_run(dev, rt, P20 "dcl t0\ndcl_2d s0\ntexldp r0, t0, s0\nmov oC0, r0\n", NULL, 0, proj, NULL),
              want);
    /* ps_1_1 texcoord: the coordinates as a colour, clamped, alpha 1 */
    const float tcc[4] = { 1.5f, 0.25f, -0.5f, 0.3f }, wcc[4] = { 1, 0.25f, 0, 1 };
    expect_px("texcoord", ps_run(dev, rt, "ps_1_1\ntexcoord t0\nmov r0, t0\n", NULL, 0, tcc, NULL), wcc);
    /* a sampler with no texture reads (0, 0, 0, 1) */
    const float black[4] = { 0, 0, 0, 1 };
    expect_px("unbound", ps_run(dev, rt, P20 "dcl t0\ndcl_2d s1\ntexld r0, t0, s1\nmov oC0, r0\n", NULL, 0, c00, NULL),
              black);
    /* texkill: t0.x runs from -1 to 1; the left half keeps the clear colour */
    const char *kills[2] = { P20 "dcl t0\ntexkill t0\ndef c0, 1, 1, 1, 1\nmov oC0, c0\n",
                             "ps_1_1\ntexkill t0\nmov r0, c0\n" };
    const float one[4] = { 1, 1, 1, 1 }, ka[4] = { -1, 0.5f, 0.5f, 1 }, kb[4] = { 1, 0.5f, 0.5f, 1 };
    for (int i = 0; i < 2; i++) {
        uint32_t ps = ps_new(dev, kills[i]);
        psf(dev, 0, one, 1);
        ps_draw(dev, rt, ps, ka, kb, NULL);
        int kept = 0, killed = 0;
        for (int y = 0; y < 64; y++)
            for (int x = 0; x < 64; x++) {
                uint32_t p = g_px[y * 64 + x];
                if (x < 32) killed += p == 0xFF0000FFu;
                else kept += p == 0xFFFFFFFFu;
            }
        if (killed != 32 * 64 || kept != 32 * 64) printf("FAIL texkill %d: killed %d kept %d\n", i, killed, kept), fails++;
        CHECKEQ(vcall(dev, DV_SetPixelShader, 1, 0u), D3D_OK);
        vcall(ps, 2, 0);
    }
}

/* texbem: stage 1 read at its coordinates moved by stage 1's bump matrix times t0's red and green */
static void group_ps_bump(void) {
    uint32_t rt, dev = ps_device(&rt);
    const uint32_t du = 0xFF804020u;                           /* r 0x80, g 0x40, b 0x20 */
    uint32_t tx4[16];
    for (int i = 0; i < 16; i++) tx4[i] = 0xFF000000u | (uint32_t)(i * 16) << 16 | (uint32_t)(255 - i * 16) << 8 | (uint32_t)(i * 7);
    CHECKEQ(vcall(dev, DV_SetTexture, 2, 0u, tex2d(dev, 1, 1, &du)), D3D_OK);
    CHECKEQ(vcall(dev, DV_SetTexture, 2, 1u, tex2d(dev, 4, 4, tx4)), D3D_OK);
    const float tc1[4] = { 0.1f, 0.1f, 0, 1 };
    struct { float m[4]; int texel; } k[] = {
        { { 0, 0, 0, 0 }, 0 },                                 /* no movement: texel (0, 0) */
        { { 0.5f, 0, 0, 0.5f }, 1 },                           /* (0.1 + 0.5 r, 0.1 + 0.5 g): (1, 0) */
        { { 0, 1, 1, 0 }, 9 },                                 /* 01 and 10: (0.1 + g, 0.1 + r): (1, 2) */
    };
    float want[4];
    for (size_t i = 0; i < sizeof k / sizeof *k; i++) {
        for (int j = 0; j < 4; j++) tss(dev, 1, 7 + (uint32_t)j, f2u(k[i].m[j]));
        px_float(tx4[k[i].texel], want);
        char what[32];
        snprintf(what, sizeof what, "texbem %zu", i);
        expect_px(what, ps_run(dev, rt, "ps_1_1\ntex t0\ntexbem t1, t0\nmov r0, t1\n", NULL, 0, NULL, tc1), want);
    }
    /* texbeml: the colour times saturate(t0.b * lscale + loffset) */
    tss(dev, 1, 22, f2u(0.5f)), tss(dev, 1, 23, f2u(0.25f));
    float l = U8(0x20) * 0.5f + 0.25f;
    px_float(tx4[1], want);
    for (int j = 0; j < 3; j++) want[j] *= l;
    for (int j = 0; j < 4; j++) tss(dev, 1, 7 + (uint32_t)j, f2u(k[1].m[j]));
    expect_px("texbeml", ps_run(dev, rt, "ps_1_1\ntex t0\ntexbeml t1, t0\nmov r0, t1\n", NULL, 0, NULL, tc1), want);
    /* ps_1_4 bem: r1.rg = c0.rg + the matrix of stage 1 (bem's destination) times r0.rg */
    for (int j = 0; j < 4; j++) tss(dev, 1, 7 + (uint32_t)j, f2u(k[2].m[j]));
    const float c[1][4] = { { 0.25f, 0.125f, 0.5f, 1 } };
    const float wb[4] = { 0.25f + U8(0x40), 0.125f + U8(0x80), 0, 0 };
    uint32_t px = ps_run(dev, rt, "ps_1_4\ntexld r0, t0\nbem r1.rg, c0, r0\nphase\nmov r0.rg, r1\nmov r0.ba, c0\n",
                         c, 1, tc1, tc1);
    float got[4];
    px_float(px, got);
    if (fabsf(got[0] - wb[0]) > 1.5f / 255 || fabsf(got[1] - wb[1]) > 1.5f / 255 || fabsf(got[2] - 0.5f) > 1.5f / 255)
        printf("FAIL bem: pixel 0x%08X\n", px), fails++;
}

/* fog after the shader (vertex fog from oFog; linear table fog) and the alpha test after it */
static void group_ps_fog_alpha(void) {
    uint32_t rt, dev = ps_device(&rt);
    rs(dev, 28, 1);                                            /* FOGENABLE */
    rs(dev, 34, 0xFF00FF00u);                                  /* FOGCOLOR green */
    rs(dev, 140, 3);                                           /* FOGVERTEXMODE LINEAR */
    uint32_t ps = ps_new(dev, P20 "def c0, 0.25, 0.5, 1, 0.75\nmov oC0, c0\n");
    /* vs_1_1 writes the fog factor 0.25: three quarters fog colour */
    uint32_t d = vdecl(dev, 1, g_pos_decl);
    CHECKEQ(vcall(dev, DV_SetVertexDeclaration, 1, d), D3D_OK);
    uint32_t vs = vs_new(dev, V11 "mov oFog, c0.x\n");
    const float f[4] = { 0.25f, 0, 0, 0 };
    vsf(dev, 0, f, 1);
    CHECKEQ(vcall(dev, DV_SetPixelShader, 1, ps), D3D_OK);
    const float want_v[4] = { 0.0625f, 0.75f + 0.125f, 0.25f, 0.75f };
    expect_px("vertex fog", draw_centre(dev, rt, vs), want_v);
    CHECKEQ(vcall(dev, DV_SetVertexShader, 1, 0u), D3D_OK);
    /* table fog LINEAR 0..1 at z 0.5: half */
    rs(dev, 35, 3);
    rs(dev, 36, f2u(0.0f)), rs(dev, 37, f2u(1.0f));
    const float want_t[4] = { 0.125f, 0.75f, 0.5f, 0.75f };
    expect_px("table fog", ps_draw(dev, rt, ps, NULL, NULL, NULL), want_t);
    rs(dev, 28, 0);
    /* alpha test GREATER 0x80: alpha 0.25 is dropped, 0.75 kept */
    rs(dev, 15, 1), rs(dev, 24, 0x80), rs(dev, 25, 5);
    const float c[2][4] = { { 0.5f, 0.5f, 0.5f, 0.25f }, { 0.5f, 0.5f, 0.5f, 0.75f } };
    uint32_t pa = ps_new(dev, P20 "mov oC0, c0\n");
    psf(dev, 0, c[0], 1);
    ps_draw(dev, rt, pa, NULL, NULL, NULL);
    int clear_px = 0;
    for (int i = 0; i < 64 * 64; i++) clear_px += g_px[i] == 0xFF0000FFu;
    CHECKEQ(clear_px, 64 * 64);
    psf(dev, 0, c[1], 1);
    expect_px("alpha kept", ps_draw(dev, rt, pa, NULL, NULL, NULL), c[1]);
    rs(dev, 15, 0);
}

/* a shader stage paired with the fixed-function stage of the other kind */
static void group_mixed_stages(void) {
    uint32_t rt, dev = ps_device(&rt);
    /* the fixed vertex pipeline (diffuse, one texture coordinate) and ps_2_0 reading v0 and t0 */
    rs(dev, 137, 0);                                           /* no lighting */
    struct { float x, y, z; uint32_t c; float u, v; } q[4];
    const float xy[4][2] = { { -1, 1 }, { 1, 1 }, { -1, -1 }, { 1, -1 } };
    for (int i = 0; i < 4; i++) q[i] = (__typeof__(q[0])){ xy[i][0], xy[i][1], 0.5f, 0xFF804020u, 0.25f, 0.75f };
    uint32_t ps = ps_new(dev, P20 "dcl v0\ndcl t0.xy\nmov r0, v0\nmov r0.xy, t0\nmov oC0, r0\n");
    CHECKEQ(vcall(dev, DV_SetPixelShader, 1, ps), D3D_OK);
    CHECKEQ(vcall(dev, DV_SetFVF, 1, 0x142u), D3D_OK);
    clear(dev, 0);
    draw_up(dev, 5, 2, q, 4, sizeof q[0]);
    read_rt(dev, rt, 64, 64, g_px);
    const float want1[4] = { 0.25f, 0.75f, U8(0x20), 1 };
    expect_px("ff vertex + ps_2_0", g_px[32 * 64 + 32], want1);
    /* the second texture coordinate set comes through too, as stage 1's (ps_1_1 texcoord t1) */
    struct { float x, y, z; uint32_t c; float u0, v0, u1, v1; } q2[4];
    for (int i = 0; i < 4; i++) q2[i] = (__typeof__(q2[0])){ xy[i][0], xy[i][1], 0.5f, 0xFF804020u, 0.25f, 0.75f, 0.5f, 0.125f };
    uint32_t ps11 = ps_new(dev, "ps_1_1\ntexcoord t1\nmul r0, t1, c0\n");
    const float mask[4] = { 1, 1, 0, 1 };
    psf(dev, 0, mask, 1);
    CHECKEQ(vcall(dev, DV_SetPixelShader, 1, ps11), D3D_OK);
    CHECKEQ(vcall(dev, DV_SetFVF, 1, 0x242u), D3D_OK);
    clear(dev, 0);
    draw_up(dev, 5, 2, q2, 4, sizeof q2[0]);
    read_rt(dev, rt, 64, 64, g_px);
    const float want2[4] = { 0.5f, 0.125f, 0, 1 };
    expect_px("ff vertex + ps_1_1 t1", g_px[32 * 64 + 32], want2);
    CHECKEQ(vcall(dev, DV_SetPixelShader, 1, 0u), D3D_OK);
    /* vs_1_1 and the fixed pixel pipeline: MODULATE TEXTURE DIFFUSE */
    static const uint32_t tx2[4] = { 0xFF102030u, 0x80405060u, 0xFF708090u, 0xFFA0B0C0u };
    CHECKEQ(vcall(dev, DV_SetTexture, 2, 0u, tex2d(dev, 2, 2, tx2)), D3D_OK);
    tss(dev, 0, 1, 4), tss(dev, 0, 2, 2), tss(dev, 0, 3, 0);  /* colour MODULATE TEXTURE DIFFUSE */
    tss(dev, 0, 4, 4), tss(dev, 0, 5, 2), tss(dev, 0, 6, 0);  /* alpha the same */
    static const uint8_t rows[2][5] = { { 0, 0, 2, 0, 0 }, { 0, 12, 1, 5, 0 } };
    CHECKEQ(vcall(dev, DV_SetVertexDeclaration, 1, vdecl(dev, 2, rows)), D3D_OK);
    uint32_t vs = vs_new(dev, "vs_1_1\ndcl_position v0\ndcl_texcoord v1\nmov oPos, v0\nmov oD0, c0\nmov oT0, v1\n");
    const float c0[4] = { 0.5f, 1, 0.25f, 1 };
    vsf(dev, 0, c0, 1);
    CHECKEQ(vcall(dev, DV_SetVertexShader, 1, vs), D3D_OK);
    clear(dev, 0);
    struct xyzuv v[4] = { { -1, 1, 0.5f, 0.75f, 0.25f }, { 1, 1, 0.5f, 0.75f, 0.25f }, { -1, -1, 0.5f, 0.75f, 0.25f },
                          { 1, -1, 0.5f, 0.75f, 0.25f } };
    draw_up(dev, 5, 2, v, 4, sizeof v[0]);
    read_rt(dev, rt, 64, 64, g_px);
    float want3[4];
    px_float(tx2[1], want3);
    for (int i = 0; i < 4; i++) want3[i] *= c0[i];
    expect_px("vs_1_1 + fixed pixel", g_px[32 * 64 + 32], want3);
}

/* SRGBWRITEENABLE: the shader's result is written sRGB-encoded */
static void group_ps_srgb(void) {
    uint32_t rt, dev = ps_device(&rt);
    rs(dev, 194, 1);
    const float want[4] = { 0.7353570f, 0.5370987f, 0.04045f, 1 };
    expect_px("srgb", ps_run(dev, rt, P20 "def c0, 0.5, 0.25, 0.0031308, 1\nmov oC0, c0\n", NULL, 0, NULL, NULL), want);
    rs(dev, 194, 0);
    const float lin[4] = { 0.5f, 0.25f, 0.0031308f, 1 };
    expect_px("linear", ps_run(dev, rt, P20 "def c0, 0.5, 0.25, 0.0031308, 1\nmov oC0, c0\n", NULL, 0, NULL, NULL), lin);
}

/* IDirect3DPixelShader9: GetFunction, GetDevice, references, Set/Get */
static void group_ps_objects(void) {
    uint32_t rt, dev = ps_device(&rt), out = gmem(4), size = gmem(4);
    uint32_t *t = NULL, nt = 0;
    char *errs = NULL;
    const char *src = P20 "def c0, 0, 1, 0, 1\nmov oC0, c0\n";
    CHECKEQ(sm_assemble(src, strlen(src), NULL, &t, &nt, &errs), 0);
    uint32_t g = gbytes(t, 4 * nt);
    CHECKEQ(vcall(dev, DV_CreatePixelShader, 2, g, out), D3D_OK);
    uint32_t ps = MEM32(out);
    CHECK(ps != 0);
    CHECKEQ(refs(ps), 1);
    CHECKEQ(vcall(ps, SH_GetFunction, 2, 0u, size), D3D_OK);
    CHECKEQ(MEM32(size), 4 * nt);
    uint32_t buf = gmem(4 * nt);
    CHECKEQ(vcall(ps, SH_GetFunction, 2, buf, size), D3D_OK);
    CHECK(!memcmp(hle_ptr(buf), t, 4 * nt));
    free(t);
    uint32_t r0 = refs(dev);
    CHECKEQ(vcall(ps, SH_GetDevice, 1, out), D3D_OK);
    CHECKEQ(MEM32(out), dev);
    CHECKEQ(refs(dev), r0 + 1);
    vcall(dev, 2, 0);
    CHECKEQ(vcall(dev, DV_SetPixelShader, 1, ps), D3D_OK);
    CHECKEQ(refs(ps), 2);
    CHECKEQ(vcall(dev, DV_GetPixelShader, 1, out), D3D_OK);
    CHECKEQ(MEM32(out), ps);
    CHECKEQ(refs(ps), 3);
    vcall(ps, 2, 0);
    const float green[4] = { 0, 1, 0, 1 };
    expect_px("shader", ps_draw(dev, rt, ps, NULL, NULL, NULL), green);
    /* SetPixelShader(NULL): the fixed pixel pipeline again (stage 0 SELECTARG1 DIFFUSE) */
    CHECKEQ(vcall(dev, DV_SetPixelShader, 1, 0u), D3D_OK);
    CHECKEQ(refs(ps), 1);
    float dif[4];
    px_float(PV_DIFFUSE, dif);
    expect_px("fixed", ps_draw(dev, rt, 0, NULL, NULL, NULL), dif);
    CHECKEQ(vcall(dev, DV_CreatePixelShader, 2, 0u, out), D3DERR_INVALIDCALL_);
    CHECKEQ(vcall(dev, DV_CreatePixelShader, 2, g, 0u), D3DERR_INVALIDCALL_);
    CHECKEQ(vcall(dev, DV_CreatePixelShader, 2, gbytes(g_m4x4, sizeof g_m4x4), out), D3DERR_INVALIDCALL_);
    CHECKEQ(vcall(ps, 2, 0), 0);
}

static void group_ps_loud(void) {
    uint32_t dev = new_device(16, 16), out = gmem(4);
    static const uint32_t ps30[] = { 0xFFFF0300u, 0x02000001, 0x800F0800, 0xA0E40000, 0x0000FFFF };
    vcall(dev, DV_CreatePixelShader, 2, gbytes(ps30, sizeof ps30), out);
    printf("ps_loud: returned\n");
}

/* Every shader file in dir (token streams) decodes, translates (the usual variant, and for pixel shaders a
 * fogged, alpha-tested, flat, sRGB one) and compiles with Metal. */
static void group_compile_files(const char *dir) {
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
        char *msl[2] = { NULL, NULL };
        if (!s.is_ps) {
            struct sm_vs_variant v;
            memset(&v, 0, sizeof v);
            for (uint32_t k = 0; k < s.nins; k++)
                if (s.ins[k].op == SM_DCL && s.ins[k].dst.type == SM_R_INPUT) v.input[s.ins[k].dst.index] = SM_IN_FLOAT;
            msl[0] = sm_msl_vertex(&s, &v);
        } else {
            struct sm_ps_variant v;
            sm_ps_variant_default(&s, &v);
            msl[0] = sm_msl_fragment(&s, &v);
            v.fog = 4, v.alpha_func = 5, v.flat = 1, v.srgb = 1;
            msl[1] = sm_msl_fragment(&s, &v);
        }
        int ok = 1;
        for (int j = 0; j < 2 && msl[j]; j++) {
            mtl_obj lib = mtl_library_async(msl[j], names[i]);
            const char *e = mtl_library_error(lib);
            if (e) { printf("FAIL %s: %s\n%s\n", names[i], e, msl[j]); fails++; ok = 0; }
            mtl_release(lib);
            free(msl[j]);
        }
        if (ok) printf("%s: %s compiled\n", names[i], sm_version_name(t[0], vb, sizeof vb));
        sm_free(&s);
        free(t);
    }
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
    else if (!strcmp(argv[1], "vs_basic")) group_vs_basic();
    else if (!strcmp(argv[1], "vs_ops")) group_vs_ops();
    else if (!strcmp(argv[1], "vs_reladdr")) group_vs_reladdr();
    else if (!strcmp(argv[1], "vs_flow")) group_vs_flow();
    else if (!strcmp(argv[1], "vs_inputs")) group_vs_inputs();
    else if (!strcmp(argv[1], "vs_halfpixel")) group_vs_halfpixel();
    else if (!strcmp(argv[1], "vs_clip")) group_vs_clip();
    else if (!strcmp(argv[1], "caps_sm2")) group_caps_sm2();
    else if (!strcmp(argv[1], "shader_objects")) group_shader_objects();
    else if (!strcmp(argv[1], "vs_cache")) group_vs_cache();
    else if (!strcmp(argv[1], "vs_loud")) group_vs_loud();
    else if (!strcmp(argv[1], "flow_loud") && arg) group_flow_loud(arg);
    else if (!strcmp(argv[1], "ps_ops")) group_ps_ops();
    else if (!strcmp(argv[1], "ps_modifiers")) group_ps_modifiers();
    else if (!strcmp(argv[1], "ps_tex")) group_ps_tex();
    else if (!strcmp(argv[1], "ps_bump")) group_ps_bump();
    else if (!strcmp(argv[1], "ps_fog_alpha")) group_ps_fog_alpha();
    else if (!strcmp(argv[1], "mixed_stages")) group_mixed_stages();
    else if (!strcmp(argv[1], "ps_srgb")) group_ps_srgb();
    else if (!strcmp(argv[1], "ps_objects")) group_ps_objects();
    else if (!strcmp(argv[1], "ps_loud")) group_ps_loud();
    else if (!strcmp(argv[1], "compile_files") && arg) group_compile_files(arg);
    else { fprintf(stderr, "sm_selftest: no group %s\n", argv[1]); return 2; }
    printf("%s: %d failed\n", argv[1], fails);
    fflush(stdout);
    return fails != 0;
}
