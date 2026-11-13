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

/* ---- Task 9: textures ---- */
#include "d3d_com.h"
#include "d3d9_device.h"

static uint32_t vcall(uint32_t obj, int slot, int n, ...) {
    uint32_t va = MEM32(MEM32(obj) + 4u * (uint32_t)slot);
    recomp_func_t f = recomp_lookup_import(va);
    if (!f) { printf("FAIL slot %d of 0x%08X: no method\n", slot, obj); fails++; return 0xDEADBEEFu; }
    uint32_t a[16] = { obj };
    va_list ap;
    va_start(ap, n);
    for (int i = 0; i < n; i++) a[1 + i] = va_arg(ap, uint32_t);
    va_end(ap);
    uint32_t esp0 = g_esp;
    for (int i = n; i >= 0; i--) { g_esp -= 4; MEM32(g_esp) = a[i]; }
    g_esp -= 4;
    MEM32(g_esp) = 0x12345678u;
    f();
    if (g_esp != esp0) { printf("FAIL %s: stack off by %d\n", g_cur_import, (int)(g_esp - esp0)); fails++; g_esp = esp0; }
    return g_eax;
}
static uint32_t call(const char *name, int n, ...) {
    uint32_t a[16];
    va_list ap;
    va_start(ap, n);
    for (int i = 0; i < n; i++) a[i] = va_arg(ap, uint32_t);
    va_end(ap);
    return callv(name, n, a);
}
#define CHECKEQ(a, b) do { uint64_t _a = (uint64_t)(a), _b = (uint64_t)(b); if (_a != _b) { \
    printf("FAIL %s:%d %s == %s: 0x%llX != 0x%llX\n", __FILE__, __LINE__, #a, #b, \
           (unsigned long long)_a, (unsigned long long)_b); fails++; } } while (0)
static int near_color(uint32_t a, uint32_t b, int tol) {
    for (int i = 0; i < 32; i += 8) {
        int x = (int)((a >> i) & 0xFF), y = (int)((b >> i) & 0xFF);
        if (x - y > tol || y - x > tol) return 0;
    }
    return 1;
}

#define D3DX_DEFAULT 0xFFFFFFFFu
#define D3DX_FILTER_NONE 1u
#define D3DX_FILTER_POINT 2u
#define D3DX_FILTER_BOX 5u
#define FMT_A8R8G8B8 21u
#define FMT_X8R8G8B8 22u
#define FMT_R5G6B5 23u
#define FMT_DXT1 0x31545844u
#define POOL_MANAGED 1u
#define POOL_SYSTEMMEM 2u
enum { TX_GetLevelCount = 13, TX_GetLevelDesc = 17, TX_GetSurfaceLevel, TX_LockRect, TX_UnlockRect };
enum { CT_GetCubeMapSurface = 18, CT_LockRect, CT_UnlockRect };
enum { SF_GetDesc = 12, SF_LockRect, SF_UnlockRect };

static const char *g_dir;
static uint32_t g_dev;

static uint32_t device(void) {
    if (g_dev) return g_dev;
    uint32_t d3d = call("Direct3DCreate9", 1, 32u), pp = gmem(56), out = gmem(4);
    MEM32(pp) = 16, MEM32(pp + 4) = 16, MEM32(pp + 8) = FMT_X8R8G8B8, MEM32(pp + 12) = 1, MEM32(pp + 24) = 1;
    MEM32(pp + 28) = 0x10001u, MEM32(pp + 32) = 1;
    CHECKEQ(vcall(d3d, 16, 6, 0u, 1u, 0x10001u, 0x40u, pp, out), 0);
    return g_dev = MEM32(out);
}
/* A file of the test's into guest memory; its size in *n. */
static uint32_t file(const char *name, uint32_t *n) {
    char path[1024];
    snprintf(path, sizeof path, "%s/%s", g_dir, name);
    FILE *f = fopen(path, "rb");
    if (!f) { printf("FAIL no file %s\n", path); fails++; *n = 0; return 0; }
    fseek(f, 0, SEEK_END);
    *n = (uint32_t)ftell(f);
    fseek(f, 0, SEEK_SET);
    uint32_t p = gmem(*n);
    if (fread(hle_ptr(p), 1, *n, f) != *n) fails++;
    fclose(f);
    return p;
}
/* D3DXCreateTextureFromFileInMemoryEx with the given format, sizes from the file. */
static uint32_t load_ex(const char *name, uint32_t fmt, uint32_t mips, uint32_t key, uint32_t info) {
    uint32_t n, p = file(name, &n), out = gmem(4);
    uint32_t hr = call("D3DXCreateTextureFromFileInMemoryEx", 15, device(), p, n, D3DX_DEFAULT, D3DX_DEFAULT, mips,
                       0u, fmt, POOL_MANAGED, D3DX_FILTER_NONE, D3DX_DEFAULT, key, info, 0u, out);
    CHECKEQ(hr, 0);
    return hr ? 0 : MEM32(out);
}
/* Texel (x, y) of a level, as stored (32-bit formats). */
static uint32_t texel(uint32_t t, uint32_t level, uint32_t x, uint32_t y) {
    uint32_t lr = gmem(8);
    CHECKEQ(vcall(t, TX_LockRect, 4, level, lr, 0u, 0x10u), 0);
    uint32_t v = MEM32(MEM32(lr + 4) + y * MEM32(lr) + 4 * x);
    vcall(t, TX_UnlockRect, 1, level);
    return v;
}

static void group_dds(void) {
    uint32_t info = gmem(28), n, p = file("dxt1_mips.dds", &n);
    CHECKEQ(call("D3DXGetImageInfoFromFileInMemory", 3, p, n, info), 0);
    CHECKEQ(MEM32(info), 8);
    CHECKEQ(MEM32(info + 4), 8);
    CHECKEQ(MEM32(info + 12), 3);
    CHECKEQ(MEM32(info + 16), FMT_DXT1);
    CHECKEQ(MEM32(info + 20), 3);                            /* D3DRTYPE_TEXTURE */
    CHECKEQ(MEM32(info + 24), 4);                            /* D3DXIFF_DDS */
    uint32_t t = load_ex("dxt1_mips.dds", D3DX_DEFAULT, 0xFFFFFFFDu /* D3DX_FROM_FILE: the file's 3 levels */, 0, 0);
    if (!t) return;
    CHECKEQ(vcall(t, TX_GetLevelCount, 0), 3);
    uint32_t desc = gmem(32), lr = gmem(8);
    vcall(t, TX_GetLevelDesc, 2, 0u, desc);
    CHECKEQ(MEM32(desc), FMT_DXT1);
    const uint8_t *src = (const uint8_t *)hle_ptr(p) + 128;
    uint32_t off = 0, sizes[3] = { 32, 8, 8 };
    for (uint32_t l = 0; l < 3; l++) {
        CHECKEQ(vcall(t, TX_LockRect, 4, l, lr, 0u, 0x10u), 0);
        CHECK(!memcmp(hle_ptr(MEM32(lr + 4)), src + off, l == 0 ? 16 : 8));   /* row 0 of each level */
        if (l == 0) CHECK(!memcmp((uint8_t *)hle_ptr(MEM32(lr + 4)) + MEM32(lr), src + 16, 16));
        vcall(t, TX_UnlockRect, 1, l);
        off += sizes[l];
    }
    vcall(t, 2, 0);
    /* the cube: six faces */
    p = file("cube.dds", &n);
    CHECKEQ(call("D3DXGetImageInfoFromFileInMemory", 3, p, n, info), 0);
    CHECKEQ(MEM32(info + 20), 5);                            /* D3DRTYPE_CUBETEXTURE */
    uint32_t out = gmem(4);
    CHECKEQ(call("D3DXCreateCubeTextureFromFileInMemoryEx", 14, device(), p, n, D3DX_DEFAULT, D3DX_DEFAULT, 0u,
                 D3DX_DEFAULT, POOL_MANAGED, D3DX_FILTER_NONE, D3DX_DEFAULT, 0u, 0u, 0u, out), 0);
    uint32_t c = MEM32(out);
    for (uint32_t f = 0; f < 6; f++) {
        CHECKEQ(vcall(c, CT_LockRect, 5, f, 0u, lr, 0u, 0x10u), 0);
        CHECKEQ(MEM32(MEM32(lr + 4)), 0xFF000000u | (f * 40) << 16 | 0x1020);
        vcall(c, CT_UnlockRect, 2, f, 0u);
    }
    vcall(c, 2, 0);
}

static void group_tga_bmp_png(void) {
    uint32_t want[15], n;
    char path[1024];
    snprintf(path, sizeof path, "%s/expected.txt", g_dir);
    FILE *f = fopen(path, "r");
    for (int i = 0; i < 15 && f; i++) if (fscanf(f, "%x", &want[i]) != 1) fails++;
    if (f) fclose(f);
    static const struct { const char *name; uint32_t iff; } imgs[] = {
        { "raw_bl.tga", 2 }, { "rle_tl.tga", 2 }, { "raw_tl.tga", 2 }, { "img.bmp", 0 }, { "img.png", 3 } };
    for (unsigned i = 0; i < sizeof imgs / sizeof *imgs; i++) {
        uint32_t info = gmem(28), p = file(imgs[i].name, &n);
        CHECKEQ(call("D3DXGetImageInfoFromFileInMemory", 3, p, n, info), 0);
        if (MEM32(info) != 5 || MEM32(info + 4) != 3 || MEM32(info + 24) != imgs[i].iff || MEM32(info + 12) != 1)
            printf("FAIL %s: info %ux%u iff %u levels %u\n", imgs[i].name, MEM32(info), MEM32(info + 4),
                   MEM32(info + 24), MEM32(info + 12)), fails++;
        uint32_t t = load_ex(imgs[i].name, FMT_A8R8G8B8, 1, 0, 0);
        if (!t) continue;
        uint32_t desc = gmem(32);
        vcall(t, TX_GetLevelDesc, 2, 0u, desc);
        CHECK(MEM32(desc + 24) == 5 && MEM32(desc + 28) == 3);
        for (uint32_t y = 0; y < 3; y++)
            for (uint32_t x = 0; x < 5; x++) {
                uint32_t got = texel(t, 0, x, y);
                if (got != want[y * 5 + x]) printf("FAIL %s (%u,%u): 0x%08X want 0x%08X\n", imgs[i].name, x, y, got, want[y * 5 + x]), fails++;
            }
        vcall(t, 2, 0);
    }
}

static uint32_t sys_surface(uint32_t w, uint32_t h, uint32_t fmt) {
    uint32_t out = gmem(4);
    CHECKEQ(vcall(device(), 36, 6, w, h, fmt, POOL_SYSTEMMEM, out, 0u), 0);   /* CreateOffscreenPlainSurface */
    return MEM32(out);
}
static void group_convert(void) {
    /* A8R8G8B8 -> R5G6B5 -> A8R8G8B8: channels that are exact 5/6-bit expansions come back as they were */
    uint32_t src = gmem(8), r565 = sys_surface(2, 1, FMT_R5G6B5), back = sys_surface(2, 1, FMT_A8R8G8B8), lr = gmem(8);
    MEM32(src) = 0xFF8441C6u, MEM32(src + 4) = 0xFFFFFFFFu;
    uint32_t r21 = gmem(16), r44 = gmem(16);                 /* the source rect may not be NULL */
    MEM32(r21) = 0, MEM32(r21 + 4) = 0, MEM32(r21 + 8) = 2, MEM32(r21 + 12) = 1;
    MEM32(r44) = 0, MEM32(r44 + 4) = 0, MEM32(r44 + 8) = 4, MEM32(r44 + 12) = 4;
    CHECKEQ(call("D3DXLoadSurfaceFromMemory", 10, r565, 0u, 0u, src, FMT_A8R8G8B8, 8u, 0u, 0u, D3DX_FILTER_NONE, 0u), 0x8876086Cu);
    CHECKEQ(call("D3DXLoadSurfaceFromMemory", 10, r565, 0u, 0u, src, FMT_A8R8G8B8, 8u, 0u, r21, D3DX_FILTER_NONE, 0u), 0);
    vcall(r565, SF_LockRect, 3, lr, 0u, 0x10u);
    CHECKEQ(MEM16(MEM32(lr + 4)), (16u << 11) | (16u << 5) | 24u);
    CHECKEQ(MEM16(MEM32(lr + 4) + 2), 0xFFFFu);
    vcall(r565, SF_UnlockRect, 0);
    CHECKEQ(call("D3DXLoadSurfaceFromSurface", 8, back, 0u, 0u, r565, 0u, 0u, D3DX_FILTER_NONE, 0u), 0);
    vcall(back, SF_LockRect, 3, lr, 0u, 0x10u);
    CHECKEQ(MEM32(MEM32(lr + 4)), 0xFF8441C6u);
    vcall(back, SF_UnlockRect, 0);
    /* a DXT1 block decodes: colour 0 red, colour 1 blue, and the two thirds between */
    uint32_t blk = gmem(8), dst = sys_surface(4, 4, FMT_A8R8G8B8);
    MEM16(blk) = 0xF800, MEM16(blk + 2) = 0x001F;
    MEM32(blk + 4) = 0x000000E4u;                            /* row 0: indices 0, 1, 2, 3 */
    CHECKEQ(call("D3DXLoadSurfaceFromMemory", 10, dst, 0u, 0u, blk, FMT_DXT1, 8u, 0u, r44, D3DX_FILTER_NONE, 0u), 0);
    vcall(dst, SF_LockRect, 3, lr, 0u, 0x10u);
    uint32_t *px = hle_ptr(MEM32(lr + 4));
    CHECK(near_color(px[0], 0xFFFF0000u, 1));
    CHECK(near_color(px[1], 0xFF0000FFu, 1));
    CHECK(near_color(px[2], 0xFFAA0055u, 2));
    CHECK(near_color(px[3], 0xFF5500AAu, 2));
    vcall(dst, SF_UnlockRect, 0);
    /* a sub-rect with a point filter: a 2x1 rect of a 4x1 source scaled into 4x2 */
    uint32_t s4 = gmem(16), d42 = sys_surface(4, 2, FMT_A8R8G8B8), r = gmem(16);
    MEM32(s4) = 0xFF000001u, MEM32(s4 + 4) = 0xFF000002u, MEM32(s4 + 8) = 0xFF000003u, MEM32(s4 + 12) = 0xFF000004u;
    MEM32(r) = 1, MEM32(r + 4) = 0, MEM32(r + 8) = 3, MEM32(r + 12) = 1;
    CHECKEQ(call("D3DXLoadSurfaceFromMemory", 10, d42, 0u, 0u, s4, FMT_A8R8G8B8, 16u, 0u, r, D3DX_FILTER_POINT, 0u), 0);
    vcall(d42, SF_LockRect, 3, lr, 0u, 0x10u);
    px = hle_ptr(MEM32(lr + 4));
    CHECK(px[0] == 0xFF000002u && px[1] == 0xFF000002u && px[2] == 0xFF000003u && px[3] == 0xFF000003u);
    vcall(d42, SF_UnlockRect, 0);
}

static void group_filter(void) {
    uint32_t out = gmem(4);
    CHECKEQ(call("D3DXCreateTexture", 8, device(), 4u, 4u, 3u, 0u, FMT_A8R8G8B8, POOL_MANAGED, out), 0);
    uint32_t t = MEM32(out), lr = gmem(8);
    vcall(t, TX_LockRect, 4, 0u, lr, 0u, 0u);
    for (uint32_t y = 0; y < 4; y++)
        for (uint32_t x = 0; x < 4; x++) MEM32(MEM32(lr + 4) + y * MEM32(lr) + 4 * x) = (x + y) & 1 ? 0xFFFFFFFFu : 0xFF000000u;
    vcall(t, TX_UnlockRect, 1, 0u);
    CHECKEQ(call("D3DXFilterTexture", 4, t, 0u, 0u, D3DX_FILTER_BOX), 0);
    CHECK(near_color(texel(t, 1, 0, 0), 0xFF808080u, 1));
    CHECK(near_color(texel(t, 1, 1, 1), 0xFF808080u, 1));
    CHECK(near_color(texel(t, 2, 0, 0), 0xFF808080u, 1));
    /* D3DXCreateTexture's defaults: 256 square, A8R8G8B8, a full chain */
    CHECKEQ(call("D3DXCreateTexture", 8, device(), D3DX_DEFAULT, D3DX_DEFAULT, D3DX_DEFAULT, 0u, 0u, POOL_MANAGED, out), 0);
    uint32_t d = MEM32(out), desc = gmem(32);
    vcall(d, TX_GetLevelDesc, 2, 0u, desc);
    CHECK(MEM32(desc) == FMT_A8R8G8B8 && MEM32(desc + 24) == 256 && MEM32(desc + 28) == 256);
    CHECKEQ(vcall(d, TX_GetLevelCount, 0), 9);
    vcall(d, 2, 0), vcall(t, 2, 0);
}

/* LPD3DXFILL2D: (out vec4, texcoord vec2, texel size vec2, data) -> (u, v, 0, 1) */
static int g_fill_calls;
static void fill_cb(void) {
    float *o = APTR(0), *tc = APTR(1);
    o[0] = tc[0], o[1] = tc[1], o[2] = 0, o[3] = 1;
    g_fill_calls++;
    RET(0, 4);
}
static const win32hle_shim g_test_shims[] = { { "selftest_fill_cb", fill_cb }, { 0, 0 } };

static void group_fill(void) {
    win32hle_register(g_test_shims);
    uint32_t out = gmem(4);
    CHECKEQ(call("D3DXCreateTexture", 8, device(), 4u, 4u, 1u, 0u, FMT_A8R8G8B8, POOL_MANAGED, out), 0);
    uint32_t t = MEM32(out);
    CHECKEQ(call("D3DXFillTexture", 3, t, hle_resolve("selftest_fill_cb"), 0u), 0);
    CHECKEQ(g_fill_calls, 16);
    for (uint32_t y = 0; y < 4; y++)
        for (uint32_t x = 0; x < 4; x++) {
            uint32_t u8 = (uint32_t)(((x + 0.5) / 4) * 255 + 0.5), v8 = (uint32_t)(((y + 0.5) / 4) * 255 + 0.5);
            uint32_t want = 0xFF000000u | u8 << 16 | v8 << 8, got = texel(t, 0, x, y);
            if (!near_color(got, want, 1)) printf("FAIL fill (%u,%u): 0x%08X want 0x%08X\n", x, y, got, want), fails++;
        }
    vcall(t, 2, 0);
}

static void group_save(void) {
    uint32_t t = load_ex("img.png", FMT_A8R8G8B8, 1, 0, 0);
    if (!t) return;
    char *g = "C:\\shot.bmp";
    uint32_t name = gmem(32);
    strcpy(hle_ptr(name), g);
    CHECKEQ(call("D3DXSaveTextureToFileA", 4, name, 0u /* BMP */, t, 0u), 0);
    char host[1024];
    snprintf(host, sizeof host, "%s/C/shot.bmp", getenv("BW2_SUPPORT"));
    FILE *f = fopen(host, "rb");
    CHECK(f != NULL);
    if (f) fclose(f);
    uint32_t out = gmem(4);
    CHECKEQ(call("D3DXCreateTextureFromFileA", 3, device(), name, out), 0);
    uint32_t r = MEM32(out);
    for (uint32_t y = 0; y < 3; y++)
        for (uint32_t x = 0; x < 5; x++) CHECKEQ(texel(r, 0, x, y) | 0xFF000000u, texel(t, 0, x, y));
    /* into memory: an ID3DXBuffer holding a DDS */
    CHECKEQ(call("D3DXSaveTextureToFileInMemory", 4, out, 4u /* DDS */, t, 0u), 0);
    uint32_t buf = MEM32(out), ptr = vcall(buf, 3, 0), size = vcall(buf, 4, 0);
    CHECK(size == 128 + 5 * 3 * 4 && !memcmp(hle_ptr(ptr), "DDS ", 4));
    uint32_t info = gmem(28);
    CHECKEQ(call("D3DXGetImageInfoFromFileInMemory", 3, ptr, size, info), 0);
    CHECK(MEM32(info) == 5 && MEM32(info + 4) == 3 && MEM32(info + 16) == FMT_A8R8G8B8);
    CHECKEQ(vcall(buf, 2, 0), 0);
    /* D3DXCreateBuffer */
    CHECKEQ(call("D3DXCreateBuffer", 2, 100u, out), 0);
    buf = MEM32(out);
    CHECKEQ(vcall(buf, 4, 0), 100);
    CHECK(vcall(buf, 3, 0) != 0);
    vcall(buf, 2, 0);
    vcall(r, 2, 0), vcall(t, 2, 0);
}

static void group_colorkey(void) {
    uint32_t t = load_ex("key.tga", FMT_A8R8G8B8, 1, 0xFFFF00FFu, 0);
    if (!t) return;
    CHECKEQ(texel(t, 0, 0, 0), 0x00000000u);
    CHECKEQ(texel(t, 0, 1, 0), 0xFF00FF00u);
    vcall(t, 2, 0);
}

static void group_malformed(void) {
    static const char *names[] = { "huge.tga", "huge_rle.tga", "huge.bmp", "huge.dds", "short.dds" };
    for (unsigned i = 0; i < sizeof names / sizeof *names; i++) {
        uint32_t n, p = file(names[i], &n), info = gmem(28);
        uint32_t hr = call("D3DXGetImageInfoFromFileInMemory", 3, p, n, info);
        if (hr != 0x88760B59u) printf("FAIL %s: 0x%08X, want D3DXERR_INVALIDDATA\n", names[i], hr), fails++;
    }
}

/* ---- plan 10 Task 3: DXT compression ---- */
#include <signal.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>
#include "d3dx.h"

#define FMT_DXT2 0x32545844u
#define FMT_DXT3 0x33545844u
#define FMT_DXT4 0x34545844u
#define FMT_DXT5 0x35545844u
enum { VT_GetLevelDesc = 17 };

static float x5(uint32_t v) { return (float)((v << 3) | (v >> 2)) / 255.0f; }
static float x6(uint32_t v) { return (float)((v << 2) | (v >> 4)) / 255.0f; }
static void rgb(float *o, uint32_t r5, uint32_t g6, uint32_t b5, float a) { o[0] = x5(r5), o[1] = x6(g6), o[2] = x5(b5), o[3] = a; }

/* bytes that end where an inaccessible page starts: a read or write past them stops the run */
static void *guarded(size_t bytes) {
    size_t pg = (size_t)getpagesize(), span = (bytes + pg - 1) / pg * pg;
    uint8_t *m = mmap(NULL, span + pg, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0);
    mprotect(m + span, pg, PROT_NONE);
    return m + span - bytes;
}
/* one block through the encoder and back through plan 4's decoder */
static void dxt_round(uint32_t fmt, const float *in, uint8_t *blk, float *out) {
    d3dx_dxt_encode(fmt, in, blk);
    d3dx_read(fmt, blk, 16, 4, 4, out);
}
static double rms(const float *a, const float *b, size_t n, int c) {
    double s = 0;
    for (size_t i = 0; i < n; i++) { double d = a[4 * i + c] - b[4 * i + c]; s += d * d; }
    return sqrt(s / (double)n);
}
#define CHECK_RMS(a, b, n, c, lim) do { double _r = rms(a, b, n, c); if (_r > (lim) / 255.0) { \
    printf("FAIL %s:%d channel %d RMS %.2f/255 > %g/255\n", __FILE__, __LINE__, c, _r * 255, (double)(lim)); fails++; } } while (0)

static void group_dxt_exact(void) {
    static const uint32_t fmts[3] = { FMT_DXT1, FMT_DXT3, FMT_DXT5 };
    float in[64], out[64];
    uint8_t blk[16];
    for (int f = 0; f < 3; f++) {
        uint32_t fmt = fmts[f];
        for (int i = 0; i < 16; i++) rgb(in + 4 * i, 20, 41, 9, 1);
        dxt_round(fmt, in, blk, out);
        CHECK(!memcmp(in, out, sizeof in));
        uint32_t cb = fmt == FMT_DXT1 ? 0 : 8;                   /* equal endpoints: all-zero indices */
        CHECKEQ((uint32_t)(blk[cb + 4] | blk[cb + 5] << 8 | blk[cb + 6] << 16 | (uint32_t)blk[cb + 7] << 24), 0);
        for (int i = 0; i < 16; i++) rgb(in + 4 * i, i % 3 ? 20 : 3, i % 3 ? 41 : 60, i % 3 ? 9 : 28, 1);
        dxt_round(fmt, in, blk, out);
        if (memcmp(in, out, sizeof in)) printf("FAIL two colours, format 0x%08X\n", fmt), fails++;
    }
    for (int i = 0; i < 16; i++) rgb(in + 4 * i, 31, 0, 17, (float)(i & 1 || i == 6));
    dxt_round(FMT_DXT5, in, blk, out);
    CHECK(!memcmp(in, out, sizeof in));
    for (int i = 0; i < 16; i++) rgb(in + 4 * i, 31, 0, 17, (float)((i * 7 % 16) * 17) / 255.0f);
    dxt_round(FMT_DXT3, in, blk, out);
    CHECK(!memcmp(in, out, sizeof in));
}

static void group_dxt_punch(void) {
    float in[64], out[64];
    uint8_t blk[8];
    for (int i = 0; i < 16; i++) rgb(in + 4 * i, i & 2 ? 25 : 2, i & 2 ? 10 : 50, i & 2 ? 4 : 30, 1);
    static const int clear[5] = { 0, 3, 5, 10, 15 };
    for (int k = 0; k < 5; k++) in[4 * clear[k] + 3] = 0.2f;
    dxt_round(FMT_DXT1, in, blk, out);
    CHECK((blk[0] | blk[1] << 8) <= (blk[2] | blk[3] << 8));    /* the 3-colour mode */
    for (int i = 0; i < 16; i++) {
        int t = i == 0 || i == 3 || i == 5 || i == 10 || i == 15;
        if (out[4 * i + 3] != (t ? 0.0f : 1.0f)) printf("FAIL texel %d alpha %g\n", i, out[4 * i + 3]), fails++;
        if (!t && memcmp(in + 4 * i, out + 4 * i, 12)) printf("FAIL texel %d colour\n", i), fails++;
    }
    for (int i = 0; i < 16; i++) in[4 * i + 3] = 0;               /* all transparent */
    dxt_round(FMT_DXT1, in, blk, out);
    for (int i = 0; i < 16; i++) CHECK(out[4 * i + 3] == 0);
}

static void gradient(float *px, uint32_t w, uint32_t h, int alpha) {
    for (uint32_t y = 0; y < h; y++)
        for (uint32_t x = 0; x < w; x++) {
            float *o = px + 4 * (y * w + x);
            o[0] = (float)x / (float)(w - 1), o[1] = (float)y / (float)(h - 1), o[2] = (float)(x + y) / (float)(w + h - 2);
            o[3] = alpha ? (float)(w - 1 - x + y) / (float)(w + h - 2) : 1;
        }
}
static void group_dxt_quality(void) {
    uint32_t w = 64, h = 64;
    float *in = malloc(16 * w * h), *out = malloc(16 * w * h);
    uint8_t *blk = malloc(w * h);
    gradient(in, w, h, 0);
    d3dx_write(FMT_DXT1, in, w, h, blk, w / 4 * 8);
    d3dx_read(FMT_DXT1, blk, w / 4 * 8, w, h, out);
    for (int c = 0; c < 4; c++) CHECK_RMS(in, out, w * h, c, 6);
    gradient(in, w, h, 1);
    d3dx_write(FMT_DXT5, in, w, h, blk, w / 4 * 16);
    d3dx_read(FMT_DXT5, blk, w / 4 * 16, w, h, out);
    for (int c = 0; c < 3; c++) CHECK_RMS(in, out, w * h, c, 6);
    CHECK_RMS(in, out, w * h, 3, 3);
    free(in), free(out), free(blk);
}

/* A level of a texture (all its slices), decoded from its staging copy. */
static float *level_px(uint32_t tex, uint32_t l, uint32_t *w, uint32_t *h, uint32_t *d) {
    struct d3d_texture *t = d3d_tex(tex);
    *w = t->w >> l ? t->w >> l : 1, *h = t->h >> l ? t->h >> l : 1, *d = t->d >> l ? t->d >> l : 1;
    uint32_t p = d3d_format_pitch(t->f, *w), rows = d3d_format_rows(t->f, *h);
    const uint8_t *m = hle_ptr(d3d_tex_mem(t, l));
    float *o = malloc(16 * (size_t)*w * *h * *d);
    for (uint32_t z = 0; z < *d; z++) d3dx_read(t->fmt, m + (size_t)z * p * rows, p, *w, *h, o + 4 * (size_t)z * *w * *h);
    return o;
}
/* The box filter's next level of a w x h x d image, as the definition gives it. */
static float *box_down(const float *s, uint32_t w, uint32_t h, uint32_t d) {
    uint32_t dw = w > 1 ? w / 2 : 1, dh = h > 1 ? h / 2 : 1, dd = d > 1 ? d / 2 : 1;
    float *o = calloc(4 * (size_t)dw * dh * dd, sizeof(float));
    for (uint32_t z = 0; z < dd; z++)
        for (uint32_t y = 0; y < dh; y++)
            for (uint32_t x = 0; x < dw; x++) {
                float *q = o + 4 * ((z * dh + y) * dw + x);
                int n = 0;
                for (uint32_t k = z * d / dd; k < (z + 1) * d / dd; k++)
                    for (uint32_t j = y * h / dh; j < (y + 1) * h / dh; j++)
                        for (uint32_t i = x * w / dw; i < (x + 1) * w / dw; i++, n++)
                            for (int c = 0; c < 4; c++) q[c] += s[4 * ((k * h + j) * w + i) + c];
                for (int c = 0; c < 4; c++) q[c] /= (float)n;
            }
    return o;
}
static void fill_surface(uint32_t surf, uint32_t w, uint32_t h, const float *px) {
    uint32_t lr = gmem(8);
    CHECKEQ(vcall(surf, SF_LockRect, 3, lr, 0u, 0u), 0);
    for (uint32_t y = 0; y < h; y++)
        for (uint32_t x = 0; x < w; x++) {
            const float *c = px + 4 * (y * w + x);
            uint32_t v = 0;
            for (int k = 0; k < 4; k++) v |= (uint32_t)lrintf(c[k] * 255) << (k == 3 ? 24 : 16 - 8 * k);
            MEM32(MEM32(lr + 4) + y * MEM32(lr) + 4 * x) = v;
        }
    vcall(surf, SF_UnlockRect, 0);
}
/* What an A8R8G8B8 surface holds, as floats (the source as D3DX sees it). */
static void quantize8(float *px, size_t n) { for (size_t i = 0; i < 4 * n; i++) px[i] = (float)lrintf(px[i] * 255) / 255.0f; }
static uint32_t level_surface(uint32_t tex, uint32_t l) {
    uint32_t out = gmem(4);
    CHECKEQ(vcall(tex, TX_GetSurfaceLevel, 2, l, out), 0);
    return MEM32(out);
}
/* A 512x512 DXT5 texture with 10 levels whose level 0 is loaded from a smooth A8R8G8B8 picture. Its
 * colours lie near one line (a brightness picture with a little colour of its own), as a real
 * texture's small levels do: a block of the 4x4 level holds the whole picture, and no DXT block comes
 * within 67/255 RMS of a full-range two-colour gradient (measured by an exhaustive endpoint search). */
static uint32_t dxt5_chain(float **src) {
    uint32_t w = 512, out = gmem(4);
    float *px = malloc(16 * w * w);
    for (uint32_t y = 0; y < w; y++)
        for (uint32_t x = 0; x < w; x++) {
            float *o = px + 4 * (y * w + x);
            float l = 0.12f * sinf(x / 37.0f + y / 53.0f) + 0.08f * cosf(x / 61.0f);
            o[0] = 0.45f + 0.8f * l + 0.03f * sinf(y / 29.0f), o[1] = 0.5f + l, o[2] = 0.35f + 0.6f * l + 0.03f * cosf(x / 43.0f);
            o[3] = 0.3f + 0.5f * (float)(x + y) / 1022;
        }
    quantize8(px, (size_t)w * w);
    uint32_t s = sys_surface(w, w, FMT_A8R8G8B8);
    fill_surface(s, w, w, px);
    CHECKEQ(call("D3DXCreateTexture", 8, device(), w, w, 10u, 0u, FMT_DXT5, POOL_MANAGED, out), 0);
    uint32_t t = MEM32(out), desc = gmem(32);
    vcall(t, TX_GetLevelDesc, 2, 0u, desc);
    CHECKEQ(MEM32(desc), FMT_DXT5);
    CHECKEQ(vcall(t, TX_GetLevelCount, 0), 10);
    uint32_t d0 = level_surface(t, 0);
    CHECKEQ(call("D3DXLoadSurfaceFromSurface", 8, d0, 0u, 0u, s, 0u, 0u, D3DX_FILTER_NONE, 0u), 0);
    vcall(d0, 2, 0), vcall(s, 2, 0);
    *src = px;
    return t;
}

static void group_dxt_load_surface(void) {
    float *px;
    uint32_t t = dxt5_chain(&px), w, h, d;
    float *got = level_px(t, 0, &w, &h, &d);
    for (int c = 0; c < 3; c++) CHECK_RMS(px, got, (size_t)w * h, c, 6);
    CHECK_RMS(px, got, (size_t)w * h, 3, 3);
    free(got), free(px), vcall(t, 2, 0);
    /* a 6x6 source into rects of a 16x16 DXT5 level that holds a smooth picture already */
    float big[16 * 16 * 4], small[6 * 6 * 4];
    for (int i = 0; i < 256; i++) {
        float *o = big + 4 * i, x = (float)(i % 16), y = (float)(i / 16);
        o[0] = 0.3f + 0.2f * x / 15, o[1] = 0.4f + 0.1f * y / 15, o[2] = 0.5f, o[3] = 0.6f + 0.2f * (x + y) / 30;
    }
    quantize8(big, 256);
    for (int i = 0; i < 36; i++) {
        float *o = small + 4 * i;
        o[0] = 0.9f - 0.02f * (float)(i % 6), o[1] = 0.2f + 0.01f * (float)(i / 6), o[2] = 0.6f, o[3] = 0.8f;
    }
    quantize8(small, 36);
    uint32_t sb = sys_surface(16, 16, FMT_A8R8G8B8), ss = sys_surface(6, 6, FMT_A8R8G8B8), out = gmem(4), r = gmem(16);
    fill_surface(sb, 16, 16, big), fill_surface(ss, 6, 6, small);
    static const uint32_t at[2][2] = { { 8, 8 }, { 2, 2 } };
    for (int k = 0; k < 2; k++) {
        CHECKEQ(call("D3DXCreateTexture", 8, device(), 16u, 16u, 1u, 0u, FMT_DXT5, POOL_MANAGED, out), 0);
        uint32_t tx = MEM32(out), dl = level_surface(tx, 0);
        CHECKEQ(call("D3DXLoadSurfaceFromSurface", 8, dl, 0u, 0u, sb, 0u, 0u, D3DX_FILTER_NONE, 0u), 0);
        uint8_t before[256];
        memcpy(before, hle_ptr(d3d_tex_mem(d3d_tex(tx), 0)), 256);
        float *was = level_px(tx, 0, &w, &h, &d);
        uint32_t x0 = at[k][0], y0 = at[k][1];
        MEM32(r) = x0, MEM32(r + 4) = y0, MEM32(r + 8) = x0 + 6, MEM32(r + 12) = y0 + 6;
        CHECKEQ(call("D3DXLoadSurfaceFromSurface", 8, dl, 0u, r, ss, 0u, 0u, D3DX_FILTER_NONE, 0u), 0);
        const uint8_t *now = hle_ptr(d3d_tex_mem(d3d_tex(tx), 0));
        float *is = level_px(tx, 0, &w, &h, &d), inr[36 * 4], gotr[36 * 4], outw[256 * 4], outg[256 * 4];
        int nr = 0, no = 0;
        for (uint32_t by = 0; by < 4; by++)
            for (uint32_t bx = 0; bx < 4; bx++) {
                int touched = bx * 4 < x0 + 6 && bx * 4 + 4 > x0 && by * 4 < y0 + 6 && by * 4 + 4 > y0;
                int same = !memcmp(before + by * 64 + bx * 16, now + by * 64 + bx * 16, 16);
                if (!touched && !same) printf("FAIL rect at (%u,%u): block (%u,%u) changed\n", x0, y0, bx, by), fails++;
                for (uint32_t y = by * 4; touched && y < by * 4 + 4; y++)
                    for (uint32_t x = bx * 4; x < bx * 4 + 4; x++) {
                        int in = x >= x0 && x < x0 + 6 && y >= y0 && y < y0 + 6;
                        if (in) memcpy(inr + 4 * nr, small + 4 * ((y - y0) * 6 + x - x0), 16), memcpy(gotr + 4 * nr++, is + 4 * (y * 16 + x), 16);
                        else memcpy(outw + 4 * no, was + 4 * (y * 16 + x), 16), memcpy(outg + 4 * no++, is + 4 * (y * 16 + x), 16);
                    }
            }
        CHECKEQ(nr, 36);
        for (int c = 0; c < 4; c++) CHECK_RMS(inr, gotr, (size_t)nr, c, 6);
        for (int c = 0; c < 4; c++) CHECK_RMS(outw, outg, (size_t)no, c, 6);
        free(was), free(is), vcall(dl, 2, 0), vcall(tx, 2, 0);
    }
    vcall(sb, 2, 0), vcall(ss, 2, 0);
}

/* Each level from l0 on against the box filter of the decoded level above. */
static void check_levels(uint32_t t, uint32_t l0, uint32_t l1, double lim) {
    for (uint32_t l = l0; l <= l1; l++) {
        uint32_t w, h, d, w1, h1, d1;
        float *up = level_px(t, l - 1, &w, &h, &d), *got = level_px(t, l, &w1, &h1, &d1), *want = box_down(up, w, h, d);
        for (int c = 0; c < 4; c++) {
            double r = rms(want, got, (size_t)w1 * h1 * d1, c);
            if (r > lim / 255.0) printf("FAIL level %u (%ux%ux%u) channel %d: RMS %.2f/255 > %g/255\n", l, w1, h1, d1, c, r * 255, lim), fails++;
        }
        free(up), free(got), free(want);
    }
}
static void group_dxt_filter(void) {
    float *px;
    uint32_t t = dxt5_chain(&px);
    free(px);
    CHECKEQ(call("D3DXFilterTexture", 4, t, 0u, 0u, D3DX_FILTER_BOX), 0);
    check_levels(t, 1, 9, 8);
    vcall(t, 2, 0);
}

static void group_dxt_small_levels(void) {
    /* the encoder pads a partial block from the level's own pixels and reads nothing past it */
    static const uint32_t sizes[4][2] = { { 2, 2 }, { 1, 1 }, { 6, 5 }, { 1, 3 } };
    for (int k = 0; k < 4; k++) {
        uint32_t w = sizes[k][0], h = sizes[k][1], bw = (w + 3) / 4, bh = (h + 3) / 4;
        float *in = guarded(16 * w * h), out[6 * 5 * 4];
        for (uint32_t y = 0; y < h; y++)
            for (uint32_t x = 0; x < w; x++) {
                float *o = in + 4 * (y * w + x);
                float s = (float)(x + 2 * y) / (float)(w + 2 * h);   /* a level this small varies little */
                o[0] = 0.4f + 0.1f * s, o[1] = 0.3f + 0.05f * s, o[2] = 0.6f - 0.08f * s, o[3] = 0.5f + 0.1f * s;
            }
        uint8_t *blk = guarded(16 * bw * bh);
        d3dx_write(FMT_DXT5, in, w, h, blk, 16 * bw);
        d3dx_read(FMT_DXT5, blk, 16 * bw, w, h, out);
        for (int c = 0; c < 4; c++) CHECK_RMS(in, out, (size_t)w * h, c, 8);
        /* the padding: every texel of the blocks decodes near the level's last column and row */
        float full[8 * 8 * 4], want[8 * 8 * 4];
        d3dx_read(FMT_DXT5, blk, 16 * bw, 4 * bw, 4 * bh, full);
        for (uint32_t y = 0; y < 4 * bh; y++)
            for (uint32_t x = 0; x < 4 * bw; x++)
                memcpy(want + 4 * (y * 4 * bw + x), in + 4 * ((y < h ? y : h - 1) * w + (x < w ? x : w - 1)), 16);
        for (int c = 0; c < 4; c++) CHECK_RMS(want, full, (size_t)16 * bw * bh, c, 8);
        /* DXT1: an opaque level stays opaque, padding and all (no 3-colour block) */
        for (uint32_t i = 0; i < w * h; i++) in[4 * i + 3] = 1;
        d3dx_write(FMT_DXT1, in, w, h, blk, 8 * bw);
        d3dx_read(FMT_DXT1, blk, 8 * bw, 4 * bw, 4 * bh, full);
        for (uint32_t i = 0; i < 16 * bw * bh; i++)
            if (full[4 * i + 3] != 1) { printf("FAIL %ux%u DXT1: texel %u of the blocks is not opaque\n", w, h, i); fails++; break; }
    }
    float *px;
    uint32_t t = dxt5_chain(&px);
    free(px);
    CHECKEQ(call("D3DXFilterTexture", 4, t, 0u, 0u, D3DX_FILTER_BOX), 0);
    check_levels(t, 8, 9, 8);
    vcall(t, 2, 0);
    /* the volume: 128x128x4 DXT1, one level in the file, the whole chain made */
    uint32_t n, p = file("vol_dxt1.dds", &n), out = gmem(4), desc = gmem(32);
    CHECKEQ(call("D3DXCreateVolumeTextureFromFileInMemoryEx", 16, device(), p, n, D3DX_DEFAULT, D3DX_DEFAULT,
                 D3DX_DEFAULT, 0u, 0u, D3DX_DEFAULT, POOL_MANAGED, D3DX_DEFAULT, D3DX_DEFAULT, 0u, 0u, 0u, out), 0);
    uint32_t v = MEM32(out);
    if (!v) return;
    CHECKEQ(vcall(v, TX_GetLevelCount, 0), 8);
    vcall(v, VT_GetLevelDesc, 2, 0u, desc);
    CHECKEQ(MEM32(desc), FMT_DXT1);
    uint32_t w, h, d;
    float *top = level_px(v, 0, &w, &h, &d), avg[4] = { 0 };
    CHECK(w == 128 && h == 128 && d == 4);
    for (size_t i = 0; i < (size_t)w * h * d; i++) for (int c = 0; c < 4; c++) avg[c] += top[4 * i + c] / (float)(w * h * d);
    float *last = level_px(v, 7, &w, &h, &d);
    CHECK(w == 1 && h == 1 && d == 1);
    for (int c = 0; c < 4; c++)
        if (fabsf(last[c] - avg[c]) > 8 / 255.0f) printf("FAIL the 1x1x1 level, channel %d: %g, want %g\n", c, last[c], avg[c]), fails++;
    check_levels(v, 1, 7, 8);
    free(top), free(last), vcall(v, 2, 0);
}

/* The test in a child: 0 if it returned, else its exit status (134 for abort); stderr in out. */
static int in_child(void (*fn)(void), char *out, size_t n) {
    fflush(stdout), fflush(stderr);
    int p[2];
    if (pipe(p) != 0) return -1;
    pid_t pid = fork();
    if (pid == 0) {
        close(p[0]);
        dup2(p[1], 2);
        close(p[1]);
        struct sigaction sa = { 0 };
        sa.sa_handler = _exit;
        sigaction(SIGABRT, &sa, NULL);
        fn();
        fflush(stderr);
        _exit(0);
    }
    close(p[1]);
    size_t k = 0;
    for (ssize_t r; k + 1 < n && (r = read(p[0], out + k, n - 1 - k)) > 0; ) k += (size_t)r;
    out[k] = 0;
    close(p[0]);
    int st = 0;
    waitpid(pid, &st, 0);
    return WIFEXITED(st) ? (WEXITSTATUS(st) == SIGABRT ? 134 : WEXITSTATUS(st)) : -1;
}
static void into_dxt2(void) {
    uint32_t out = gmem(4), src = sys_surface(4, 4, FMT_A8R8G8B8);
    if (call("D3DXCreateTexture", 8, device(), 4u, 4u, 1u, 0u, FMT_DXT2, POOL_MANAGED, out)) return;
    uint32_t t = MEM32(out), desc = gmem(32);
    vcall(t, TX_GetLevelDesc, 2, 0u, desc);
    if (MEM32(desc) != FMT_DXT2) { fprintf(stderr, "the texture is 0x%08X, not DXT2\n", MEM32(desc)); return; }
    call("D3DXLoadSurfaceFromSurface", 8, level_surface(t, 0), 0u, 0u, src, 0u, 0u, D3DX_FILTER_NONE, 0u);
}
static void encode_dxt4(void) {
    float in[64] = { 0 };
    uint8_t blk[16];
    d3dx_dxt_encode(FMT_DXT4, in, blk);
}
static void group_dxt_unsupported(void) {
    static const struct { void (*fn)(void); const char *want; } cases[] = {
        { into_dxt2, "DXT2" }, { encode_dxt4, "DXT4" } };
    for (int i = 0; i < 2; i++) {
        char out[4096];
        int st = in_child(cases[i].fn, out, sizeof out);
        if (st == 0 || !strstr(out, "[bw2] FATAL") || !strstr(out, cases[i].want))
            printf("FAIL %s: status %d, want a stop naming it; stderr:\n%s\n", cases[i].want, st, out), fails++;
    }
}

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: d3dx_selftest <group> [args]\n"); return 2; }
    hle_mem_init();
    bw2_register_shims();
    mach_enter();
    g_esp = hle_va(hle_alloc(1u << 20)) + (1u << 20) - 64;
    static const struct { const char *name; void (*fn)(void); } groups[] = {
        { "dds", group_dds }, { "tga_bmp_png", group_tga_bmp_png }, { "convert", group_convert },
        { "filter", group_filter }, { "fill", group_fill }, { "save", group_save }, { "colorkey", group_colorkey },
        { "malformed", group_malformed }, { "dxt_exact", group_dxt_exact }, { "dxt_punch", group_dxt_punch },
        { "dxt_quality", group_dxt_quality }, { "dxt_load_surface", group_dxt_load_surface },
        { "dxt_filter", group_dxt_filter }, { "dxt_small_levels", group_dxt_small_levels },
        { "dxt_unsupported", group_dxt_unsupported } };
    if (!strcmp(argv[1], "math") && argc > 2) group_math(argv[2]);
    else {
        unsigned i = 0;
        while (i < sizeof groups / sizeof *groups && strcmp(groups[i].name, argv[1])) i++;
        if (i == sizeof groups / sizeof *groups || argc < 3) { fprintf(stderr, "d3dx_selftest: no group %s\n", argv[1]); return 2; }
        g_dir = argv[2];
        if (bw2_mount("game", getenv("BW2_SUPPORT"))) { printf("FAIL mount\n"); return 1; }
        groups[i].fn();
    }
    printf("%s: %d failed\n", argv[1], fails);
    fflush(stdout);
    return fails != 0;
}
