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

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: d3dx_selftest <group> [args]\n"); return 2; }
    hle_mem_init();
    bw2_register_shims();
    mach_enter();
    g_esp = hle_va(hle_alloc(1u << 20)) + (1u << 20) - 64;
    static const struct { const char *name; void (*fn)(void); } groups[] = {
        { "dds", group_dds }, { "tga_bmp_png", group_tga_bmp_png }, { "convert", group_convert },
        { "filter", group_filter }, { "fill", group_fill }, { "save", group_save }, { "colorkey", group_colorkey },
        { "malformed", group_malformed } };
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
