/*
 * d3dx_selftest.c - D3DX's math and textures called as lifted code calls them
 * (plan 4 Tasks 8-9). Built with the engine (minus main.c) by tests/test_d3dx.py;
 * `d3dx_selftest <group> [args]` prints "<group>: <n> failed".
 *
 * math: a file of cases written by the test (the expected values are the
 * test's own, from the documented formulas). Each function's arguments are
 * described by a spec: a return kind, then one token per argument.
 */
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "win32hle.h"
#include "bw2.h"

const recomp_dispatch_entry_t recomp_dispatch_table[1] = { { 0, 0 } };
const uint32_t recomp_dispatch_count = 0;

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

static uint32_t callv(const char *name, int n, const uint32_t *a) {
    uint32_t va = hle_resolve(name);
    recomp_func_t f = va ? recomp_lookup_import(va) : NULL;
    if (!f) { printf("FAIL %s: not answered\n", name); fails++; return 0xDEADBEEFu; }
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
static uint32_t gmem(uint32_t n) { return hle_va(hle_alloc(n)); }
static uint32_t fbits(double d) { float f = (float)d; uint32_t u; memcpy(&u, &f, 4); return u; }
static float gf(uint32_t va) { float f; memcpy(&f, hle_ptr(va), 4); return f; }

/*
 * Specs. Return: P (the first output pointer), F (a float in st(0)), Z (NULL).
 * Arguments: oN an output of N floats; a<k>N an output that is input argument k (0-based) too;
 * mN a pointer to N input floats; f a float; n a NULL pointer; dN an output of N floats after
 * the others in the expected list; vp a D3DVIEWPORT9 (4 DWORDs, 2 floats, from 6 inputs);
 * hN a pointer to N halves (inputs as integers); cN the count N.
 */
static const struct { const char *name, *fn, *spec; } g_math[] = {
    { "D3DXMatrixMultiply", 0, "P o16 m16 m16" },
    { "D3DXMatrixMultiply/alias", "D3DXMatrixMultiply", "P a1:16 m16 m16" },
    { "D3DXMatrixTranspose", 0, "P o16 m16" },
    { "D3DXMatrixTranspose/alias", "D3DXMatrixTranspose", "P a1:16 m16" },
    { "D3DXMatrixDeterminant", 0, "F m16" },
    { "D3DXMatrixInverse", 0, "P o16 d1 m16" },
    { "D3DXMatrixInverse/alias", "D3DXMatrixInverse", "P a2:16 n m16" },
    { "D3DXMatrixInverse/singular", "D3DXMatrixInverse", "Z o16 n m16" },
    { "D3DXMatrixPerspectiveFovLH", 0, "P o16 f f f f" },
    { "D3DXMatrixOrthoLH", 0, "P o16 f f f f" },
    { "D3DXMatrixOrthoOffCenterLH", 0, "P o16 f f f f f f" },
    { "D3DXMatrixScaling", 0, "P o16 f f f" },
    { "D3DXMatrixTranslation", 0, "P o16 f f f" },
    { "D3DXMatrixRotationX", 0, "P o16 f" },
    { "D3DXMatrixRotationY", 0, "P o16 f" },
    { "D3DXMatrixRotationZ", 0, "P o16 f" },
    { "D3DXMatrixLookAtLH", 0, "P o16 m3 m3 m3" },
    { "D3DXMatrixRotationQuaternion", 0, "P o16 m4" },
    { "D3DXMatrixAffineTransformation", 0, "P o16 f m3 m4 m3" },
    { "D3DXMatrixAffineTransformation/nulls", "D3DXMatrixAffineTransformation", "P o16 f n m4 n" },
    { "D3DXVec2Normalize", 0, "P o2 m2" },
    { "D3DXVec3Normalize", 0, "P o3 m3" },
    { "D3DXVec4Normalize", 0, "P o4 m4" },
    { "D3DXVec3TransformCoord", 0, "P o3 m3 m16" },
    { "D3DXVec3Transform", 0, "P o4 m3 m16" },
    { "D3DXVec4Transform", 0, "P o4 m4 m16" },
    { "D3DXVec3Project", 0, "P o3 m3 vp m16 m16 m16" },
    { "D3DXVec3Project/nullworld", "D3DXVec3Project", "P o3 m3 vp m16 m16 n" },
    { "D3DXVec3Unproject", 0, "P o3 m3 vp m16 m16 m16" },
    { "D3DXQuaternionMultiply", 0, "P o4 m4 m4" },
    { "D3DXQuaternionInverse", 0, "P o4 m4" },
    { "D3DXQuaternionNormalize", 0, "P o4 m4" },
    { "D3DXQuaternionRotationYawPitchRoll", 0, "P o4 f f f" },
    { "D3DXQuaternionSlerp", 0, "P o4 m4 m4 f" },
    { "D3DXQuaternionRotationMatrix", 0, "P o4 m16" },
    { "D3DXFloat16To32Array", 0, "P o10 h10 c10" },
};

static int close_enough(double got, double want) {
    double d = fabs(got - want), m = fabs(want);
    return d <= 1e-5 || d <= 1e-5 * m || (isinf(want) && got == want) || (isnan(want) && isnan(got));
}

static void run_case(const char *name, const double *in, int nin, const double *want, int nwant, int line) {
    unsigned k = 0;
    while (k < sizeof g_math / sizeof *g_math && strcmp(g_math[k].name, name)) k++;
    if (k == sizeof g_math / sizeof *g_math) { printf("FAIL line %d: no spec for %s\n", line, name); fails++; return; }
    const char *fn = g_math[k].fn ? g_math[k].fn : name;
    char spec[128];
    snprintf(spec, sizeof spec, "%s", g_math[k].spec);
    char *save, *tok = strtok_r(spec, " ", &save);
    char ret = tok[0];
    uint32_t args[8], outs[8], outn[8], douts[8], doutn[8];
    int na = 0, no = 0, nd = 0, ii = 0;
    struct { int arg; int out; int n; } alias = { -1, -1, 0 };
    while ((tok = strtok_r(NULL, " ", &save))) {
        char t = tok[0];
        if (t == 'o') {
            uint32_t n = (uint32_t)atoi(tok + 1);
            outs[no] = args[na] = gmem(4 * n), outn[no++] = n, na++;
        } else if (t == 'a') {
            alias.arg = atoi(tok + 1), alias.n = atoi(strchr(tok, ':') + 1), alias.out = na;
            args[na++] = 0;                               /* filled in when that argument is made */
        } else if (t == 'd') {
            uint32_t n = (uint32_t)atoi(tok + 1);
            douts[nd] = args[na] = gmem(4 * n), doutn[nd++] = n, na++;
        } else if (t == 'm') {
            uint32_t n = (uint32_t)atoi(tok + 1), p = gmem(4 * n);
            for (uint32_t i = 0; i < n; i++) MEM32(p + 4 * i) = fbits(in[ii++]);
            args[na++] = p;
        } else if (t == 'f') args[na++] = fbits(in[ii++]);
        else if (t == 'n') args[na++] = 0;
        else if (t == 'v') {
            uint32_t p = gmem(24);
            for (int i = 0; i < 4; i++) MEM32(p + 4 * i) = (uint32_t)in[ii++];
            for (int i = 4; i < 6; i++) MEM32(p + 4 * i) = fbits(in[ii++]);
            args[na++] = p;
        } else if (t == 'h') {
            uint32_t n = (uint32_t)atoi(tok + 1), p = gmem(2 * n);
            for (uint32_t i = 0; i < n; i++) MEM16(p + 2 * i) = (uint16_t)in[ii++];
            args[na++] = p;
        } else if (t == 'c') args[na++] = (uint32_t)atoi(tok + 1);
    }
    if (alias.arg >= 0) {
        args[alias.out] = args[alias.arg];
        outs[no] = args[alias.out], outn[no++] = (uint32_t)alias.n;
    }
    if (ii != nin) { printf("FAIL line %d %s: %d inputs given, spec reads %d\n", line, name, nin, ii); fails++; return; }
    int top0 = g_fp_top;
    uint32_t r = callv(fn, na, args);
    int w = 0, bad = 0;
    if (ret == 'P' && r != (alias.arg >= 0 ? args[alias.out] : outs[0])) printf("FAIL line %d %s: returned 0x%08X\n", line, name, r), bad = 1;
    if (ret == 'Z' && r != 0) printf("FAIL line %d %s: returned 0x%08X, want NULL\n", line, name, r), bad = 1;
    if (ret == 'F') {
        if (g_fp_top == top0) { printf("FAIL line %d %s: nothing on the x87 stack\n", line, name); fails++; return; }
        double got = fp_pop_impl(g_st, &g_fp_top);
        if (!close_enough(got, want[w])) printf("FAIL line %d %s: %.9g, want %.9g\n", line, name, got, want[w]), bad = 1;
        w++;
    }
    for (int o = (alias.arg >= 0 ? no - 1 : 0); ret != 'Z' && o < no; o++)
        for (uint32_t i = 0; i < outn[o] && w < nwant; i++, w++) {
            double got = gf(outs[o] + 4 * i);
            if (!close_enough(got, want[w])) printf("FAIL line %d %s out[%u]: %.9g, want %.9g\n", line, name, i, got, want[w]), bad = 1;
        }
    for (int o = 0; ret != 'Z' && o < nd; o++)
        for (uint32_t i = 0; i < doutn[o] && w < nwant; i++, w++) {
            double got = gf(douts[o] + 4 * i);
            if (!close_enough(got, want[w])) printf("FAIL line %d %s det: %.9g, want %.9g\n", line, name, got, want[w]), bad = 1;
        }
    if (w != nwant) printf("FAIL line %d %s: %d outputs checked of %d\n", line, name, w, nwant), bad = 1;
    fails += bad;
}

static void group_math(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) { printf("FAIL no cases file %s\n", path); fails++; return; }
    char *line = NULL;
    size_t cap = 0;
    int ln = 0, ncases = 0;
    while (getline(&line, &cap, f) > 0) {
        ln++;
        char name[96];
        double in[128], out[128];
        int nin = 0, nout = 0, mode = 0;
        char *save, *tok = strtok_r(line, " \n", &save);
        if (!tok) continue;
        snprintf(name, sizeof name, "%s", tok);
        while ((tok = strtok_r(NULL, " \n", &save))) {
            if (!strcmp(tok, "in")) mode = 1;
            else if (!strcmp(tok, "out")) mode = 2;
            else if (mode == 1 && nin < 128) in[nin++] = strtod(tok, NULL);
            else if (mode == 2 && nout < 128) out[nout++] = strtod(tok, NULL);
        }
        run_case(name, in, nin, out, nout, ln);
        ncases++;
    }
    free(line);
    fclose(f);
    printf("math: %d cases\n", ncases);
}

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: d3dx_selftest <group> [args]\n"); return 2; }
    hle_mem_init();
    bw2_register_shims();
    mach_enter();
    g_esp = hle_va(hle_alloc(1u << 20)) + (1u << 20) - 64;
    if (!strcmp(argv[1], "math") && argc > 2) group_math(argv[2]);
    else { fprintf(stderr, "d3dx_selftest: no group %s\n", argv[1]); return 2; }
    printf("%s: %d failed\n", argv[1], fails);
    fflush(stdout);
    return fails != 0;
}
