/*
 * ui_selftest.c - D3DX's 2D layer (plan 7): d3d_call, ID3DXSprite and the
 * fonts (font.h), called as lifted code calls them, on a headless device.
 * Built with the engine (minus main.c) by tests/test_ui.py;
 * `ui_selftest <group> [args]` runs one group and prints "<group>: <n>
 * failed". Pixel groups draw into a 64x64 A8R8G8B8 target and read it back.
 * Font groups take their files (the Mac's fonts or renamed copies in the
 * test's tmp_path) and expectations from the test as arguments.
 */
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>
#include "win32hle.h"
#include "bw2.h"
#include "d3d_com.h"
#include "sm.h"
#include "ui.h"
#include "font.h"
#include <math.h>

const recomp_dispatch_entry_t recomp_dispatch_table[1] = { { 0, 0 } };
const uint32_t recomp_dispatch_count = 0;

static int fails;
static int chk(int ok, const char *what, int line) {
    if (!ok) { printf("FAIL %s:%d %s\n", __FILE__, line, what); fails++; }
    return ok;
}
static int chk_eq(uint64_t a, uint64_t b, const char *ea, const char *eb, int line) {
    if (a != b) {
        printf("FAIL %s:%d %s == %s: 0x%llX != 0x%llX\n", __FILE__, line, ea, eb, (unsigned long long)a,
               (unsigned long long)b);
        fails++;
    }
    return a == b;
}
#define CHECK(c) chk(!!(c), #c, __LINE__)
#define CHECKEQ(a, b) chk_eq((uint64_t)(a), (uint64_t)(b), #a, #b, __LINE__)

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
/* a method by interface and name, through the object's vtable (the slot from d3d_ifaces) */
static int slot_of(const char *iface, const char *method) {
    for (const struct d3d_iface *f = d3d_ifaces; f->name; f++)
        if (!strcmp(f->name, iface))
            for (int i = 0; f->methods[i]; i++)
                if (!strcmp(f->methods[i], method)) return i;
    printf("FAIL no method %s::%s\n", iface, method);
    fails++;
    return -1;
}
static uint32_t vcallv(uint32_t obj, int slot, int n, va_list ap) {
    if (slot < 0) return 0xDEADBEEFu;
    uint32_t va = MEM32(MEM32(obj) + 4u * (uint32_t)slot);
    recomp_func_t f = recomp_lookup_import(va);
    if (!f) { printf("FAIL slot %d of 0x%08X: no method\n", slot, obj); fails++; return 0xDEADBEEFu; }
    uint32_t a[16] = { obj };
    for (int i = 0; i < n; i++) a[1 + i] = va_arg(ap, uint32_t);
    return run(f, g_cur_import ? g_cur_import : "?", n + 1, a);
}
static uint32_t mcall(uint32_t obj, const char *iface, const char *method, int n, ...) {
    va_list ap;
    va_start(ap, n);
    uint32_t r = vcallv(obj, slot_of(iface, method), n, ap);
    va_end(ap);
    return r;
}
#define DV(dev, m, n, ...) mcall(dev, "IDirect3DDevice9", m, n, ##__VA_ARGS__)
#define SP(s, m, n, ...) mcall(s, "ID3DXSprite", m, n, ##__VA_ARGS__)
#define TX(t, m, n, ...) mcall(t, "IDirect3DTexture9", m, n, ##__VA_ARGS__)

static uint32_t gmem(uint32_t n) { uint32_t p = hle_va(hle_alloc(n)); memset(hle_ptr(p), 0, n); return p; }
static uint32_t gbytes(const void *b, uint32_t n) { uint32_t p = gmem(n + 1); memcpy(hle_ptr(p), b, n); return p; }
static uint32_t f2u(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }
static uint32_t refs(uint32_t o) { mcall(o, "IDirect3DDevice9", "AddRef", 0); return mcall(o, "IDirect3DDevice9", "Release", 0); }
static void release(uint32_t o) { mcall(o, "IDirect3DDevice9", "Release", 0); }

/* Run fn in a child with its stderr captured: the child's exit status, 0 if fn returned,
 * 134 if it stopped with abort(). */
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
        sa.sa_handler = _exit;                  /* no crash report: exit with the signal's number */
        sigaction(SIGABRT, &sa, NULL);
        fn();
        fflush(stderr);
        _exit(0);
    }
    close(p[1]);
    size_t k = 0;
    char sink[4096];
    for (ssize_t r; (r = read(p[0], k + 1 < n ? out + k : sink, k + 1 < n ? n - 1 - k : sizeof sink)) > 0; )
        if (k + 1 < n) k += (size_t)r;
    out[k] = 0;
    close(p[0]);
    int st = 0;
    waitpid(pid, &st, 0);
    return WIFEXITED(st) ? (WEXITSTATUS(st) == SIGABRT ? 134 : WEXITSTATUS(st)) : -1;
}
static void expect_stop(void (*fn)(void), const char *want, const char *what) {
    char out[4096];
    int st = in_child(fn, out, sizeof out);
    if (st == 0 || !strstr(out, want)) {
        printf("FAIL %s: status %d, want a stop with \"%s\"; stderr:\n%s\n", what, st, want, out);
        fails++;
    }
}

/* ---- the device ---- */

static uint32_t g_pp;
static uint32_t new_device_sized(uint32_t w, uint32_t h) {
    uint32_t d3d = call("Direct3DCreate9", 1, 32u);
    uint32_t pp = gmem(56), out = gmem(4);
    MEM32(pp + 0) = w, MEM32(pp + 4) = h, MEM32(pp + 8) = 22, MEM32(pp + 12) = 1, MEM32(pp + 24) = 1;
    MEM32(pp + 28) = 0x10001u, MEM32(pp + 32) = 1, MEM32(pp + 36) = 1, MEM32(pp + 40) = 75;
    g_pp = pp;
    CHECKEQ(mcall(d3d, "IDirect3D9", "CreateDevice", 6, 0u, 1u, 0x10001u, 0x40u, pp, out), D3D_OK);
    return MEM32(out);
}
static uint32_t new_device(void) { return new_device_sized(64, 64); }
static uint32_t argb_target(uint32_t dev) {
    uint32_t out = gmem(4);
    CHECKEQ(DV(dev, "CreateRenderTarget", 8, 64u, 64u, 21u, 0u, 0u, 0u, out, 0u), D3D_OK);
    CHECKEQ(DV(dev, "SetRenderTarget", 2, 0u, MEM32(out)), D3D_OK);
    return MEM32(out);
}
static void clear(uint32_t dev, uint32_t c) { CHECKEQ(DV(dev, "Clear", 6, 0u, 0u, 7u, c, f2u(1.0f), 0u), D3D_OK); }
static uint32_t g_px[64 * 64];
/* a w x h render target's pixels into px */
static void read_into(uint32_t dev, uint32_t rt, uint32_t *px, uint32_t w, uint32_t h) {
    uint32_t desc = gmem(32), out = gmem(4);
    CHECKEQ(mcall(rt, "IDirect3DSurface9", "GetDesc", 1, desc), D3D_OK);
    CHECKEQ(DV(dev, "CreateOffscreenPlainSurface", 6, w, h, MEM32(desc), 2u, out, 0u), D3D_OK);
    uint32_t s = MEM32(out), lr = gmem(8);
    CHECKEQ(DV(dev, "GetRenderTargetData", 2, rt, s), D3D_OK);
    CHECKEQ(mcall(s, "IDirect3DSurface9", "LockRect", 3, lr, 0u, 0x10u), D3D_OK);
    for (uint32_t y = 0; y < h; y++) memcpy(px + y * w, hle_ptr(MEM32(lr + 4) + y * MEM32(lr)), w * 4);
    CHECKEQ(mcall(s, "IDirect3DSurface9", "UnlockRect", 0), D3D_OK);
    release(s);
}
static void read_rt(uint32_t dev, uint32_t rt) { read_into(dev, rt, g_px, 64, 64); }
/* an A8R8G8B8 managed texture of w x h texels */
static uint32_t texture(uint32_t dev, uint32_t w, uint32_t h, const uint32_t *texels) {
    uint32_t out = gmem(4), lr = gmem(8);
    CHECKEQ(DV(dev, "CreateTexture", 8, w, h, 1u, 0u, 21u, 1u, out, 0u), D3D_OK);
    uint32_t t = MEM32(out);
    CHECKEQ(TX(t, "LockRect", 4, 0u, lr, 0u, 0u), D3D_OK);
    for (uint32_t y = 0; y < h; y++) memcpy((uint8_t *)hle_ptr(MEM32(lr + 4)) + y * MEM32(lr), texels + y * w, w * 4);
    CHECKEQ(TX(t, "UnlockRect", 1, 0u), D3D_OK);
    return t;
}
static uint32_t solid(uint32_t dev, uint32_t c) {
    uint32_t t[16];
    for (int i = 0; i < 16; i++) t[i] = c;
    return texture(dev, 4, 4, t);
}
static int near_px(uint32_t got, uint32_t want, int tol, int alpha) {
    for (int s = alpha ? 0 : 8; s < 32; s += 8)
        if (abs((int)(got >> (24 - s) & 0xFF) - (int)(want >> (24 - s) & 0xFF)) > tol) return 0;
    return 1;
}
static void expect_px(int x, int y, uint32_t want, int tol, const char *what) {
    uint32_t got = g_px[y * 64 + x];
    if (!near_px(got, want, tol, 0)) {
        printf("FAIL %s: pixel (%d,%d) 0x%08X, want 0x%08X\n", what, x, y, got, want);
        fails++;
    }
}
static uint32_t vec3(float x, float y, float z) {
    uint32_t p = gmem(12);
    MEM32(p) = f2u(x), MEM32(p + 4) = f2u(y), MEM32(p + 8) = f2u(z);
    return p;
}
static uint32_t rect(int32_t l, int32_t t, int32_t r, int32_t b) {
    uint32_t p = gmem(16);
    MEM32(p) = (uint32_t)l, MEM32(p + 4) = (uint32_t)t, MEM32(p + 8) = (uint32_t)r, MEM32(p + 12) = (uint32_t)b;
    return p;
}
static uint32_t matrix(const float *m) { return gbytes(m, 64); }
static const float g_identity[16] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 };

static uint32_t new_sprite(uint32_t dev) {
    uint32_t out = gmem(4);
    CHECKEQ(call("D3DXCreateSprite", 2, dev, out), D3D_OK);
    CHECK(MEM32(out) != 0);
    return MEM32(out);
}
static uint32_t rs_get(uint32_t dev, uint32_t s) { uint32_t o = gmem(4); CHECKEQ(DV(dev, "GetRenderState", 2, s, o), D3D_OK); return MEM32(o); }
static uint32_t tss_get(uint32_t dev, uint32_t st, uint32_t s) {
    uint32_t o = gmem(4);
    CHECKEQ(DV(dev, "GetTextureStageState", 3, st, s, o), D3D_OK);
    return MEM32(o);
}
static uint32_t ss_get(uint32_t dev, uint32_t smp, uint32_t s) {
    uint32_t o = gmem(4);
    CHECKEQ(DV(dev, "GetSamplerState", 3, smp, s, o), D3D_OK);
    return MEM32(o);
}
static uint32_t fvf_get(uint32_t dev) { uint32_t o = gmem(4); CHECKEQ(DV(dev, "GetFVF", 1, o), D3D_OK); return MEM32(o); }
static uint32_t shader_get(uint32_t dev, const char *m) {      /* the bound shader, without keeping a reference */
    uint32_t o = gmem(4);
    CHECKEQ(DV(dev, m, 1, o), D3D_OK);
    if (MEM32(o)) release(MEM32(o));
    return MEM32(o);
}
static uint32_t shader(uint32_t dev, const char *src, int pixel) {
    uint32_t *t = NULL, nt = 0, out = gmem(4);
    char *errs = NULL;
    if (sm_assemble(src, strlen(src), NULL, &t, &nt, &errs)) { printf("FAIL asm: %s\n", errs); fails++; return 0; }
    CHECKEQ(DV(dev, pixel ? "CreatePixelShader" : "CreateVertexShader", 2, gbytes(t, 4 * nt), out), D3D_OK);
    free(t);
    return MEM32(out);
}

/* ---- d3d_call ---- */

static uint32_t g_dev;
static void c_unknown_method(void) { d3d_call(g_dev, "IDirect3DDevice9", "NoSuchMethod", 0, NULL); }
static void c_unknown_iface(void) { d3d_call(g_dev, "INoSuchInterface", "Release", 0, NULL); }

static void group_d3d_call(void) {
    uint32_t dev = g_dev = new_device(), out = gmem(4);
    CHECKEQ(rs_get(dev, 7), 1);                                       /* ZENABLE: on with the auto depth buffer */
    uint32_t a[2] = { 7, 0 };
    CHECKEQ(d3d_call(dev, "IDirect3DDevice9", "SetRenderState", 2, a), D3D_OK);
    uint32_t b[2] = { 7, out };
    MEM32(out) = 0xCDCDCDCDu;
    CHECKEQ(d3d_call(dev, "IDirect3DDevice9", "GetRenderState", 2, b), D3D_OK);
    CHECKEQ(MEM32(out), 0);
    CHECKEQ(rs_get(dev, 7), 0);
    char m[32];                                                       /* a name at another address: same slot */
    strcpy(m, "GetRenderState");
    MEM32(out) = 0xCDCDCDCDu;
    CHECKEQ(d3d_call(dev, "IDirect3DDevice9", m, 2, b), D3D_OK);
    CHECKEQ(MEM32(out), 0);
    uint32_t r0 = refs(dev);
    CHECKEQ(d3d_call(dev, "IDirect3DDevice9", "AddRef", 0, NULL), r0 + 1);
    CHECKEQ(d3d_call(dev, "IDirect3DDevice9", "Release", 0, NULL), r0);
    expect_stop(c_unknown_method, "IDirect3DDevice9 has no method NoSuchMethod", "unknown method");
    expect_stop(c_unknown_iface, "no interface INoSuchInterface", "unknown interface");
}

/* ---- sprite_basic: four solid colours, 1:1 ---- */

static const uint32_t g_quad4[16] = {
    0xFFFF0000u, 0xFFFF0000u, 0xFF00FF00u, 0xFF00FF00u, 0xFFFF0000u, 0xFFFF0000u, 0xFF00FF00u, 0xFF00FF00u,
    0xFF0000FFu, 0xFF0000FFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFF0000FFu, 0xFF0000FFu, 0xFFFFFFFFu, 0xFFFFFFFFu };

static void group_sprite_basic(void) {
    uint32_t dev = new_device(), rt = argb_target(dev), s = new_sprite(dev), t = texture(dev, 4, 4, g_quad4);
    const uint32_t bg = 0xFF203040u;
    clear(dev, bg);
    CHECKEQ(SP(s, "Begin", 1, D3DXSPRITE_ALPHABLEND), D3D_OK);
    CHECKEQ(SP(s, "Draw", 5, t, 0u, 0u, vec3(10, 10, 0), 0xFFFFFFFFu), D3D_OK);
    CHECKEQ(SP(s, "End", 0), D3D_OK);
    read_rt(dev, rt);
    for (int y = 9; y <= 14; y++)
        for (int x = 9; x <= 14; x++) {
            int in = x >= 10 && x < 14 && y >= 10 && y < 14;
            expect_px(x, y, in ? g_quad4[(y - 10) * 4 + (x - 10)] : bg, 1, in ? "texel" : "around");
        }
    release(s), release(t), release(rt);
}

/* ---- sprite_texel_exact: 16x16 distinct texels, LINEAR (the sprite's own filter) ---- */

static void group_sprite_texel_exact(void) {
    uint32_t dev = new_device(), rt = argb_target(dev), s = new_sprite(dev), tx[256];
    for (uint32_t y = 0; y < 16; y++)
        for (uint32_t x = 0; x < 16; x++)
            tx[y * 16 + x] = 0xFF000000u | (x * 17) << 16 | (y * 17) << 8 | ((x * 16 + y) * 7 & 0xFF);
    uint32_t t = texture(dev, 16, 16, tx);
    clear(dev, 0xFF000000u);
    CHECKEQ(SP(s, "Begin", 1, 0u), D3D_OK);
    CHECKEQ(SP(s, "Draw", 5, t, 0u, 0u, vec3(8, 8, 0), 0xFFFFFFFFu), D3D_OK);
    CHECKEQ(SP(s, "End", 0), D3D_OK);
    read_rt(dev, rt);
    int bad = 0;
    for (int y = 0; y < 16; y++)
        for (int x = 0; x < 16; x++)
            if (g_px[(8 + y) * 64 + 8 + x] != tx[y * 16 + x] && bad++ < 8)
                printf("FAIL texel (%d,%d): pixel 0x%08X, want 0x%08X\n", x, y, g_px[(8 + y) * 64 + 8 + x], tx[y * 16 + x]);
    fails += bad;
    release(s), release(t), release(rt);
}

/* ---- sprite_args: srcRect, center, the transform, the colour ---- */

static void group_sprite_args(void) {
    uint32_t dev = new_device(), rt = argb_target(dev), s = new_sprite(dev), tx[16];
    for (uint32_t i = 0; i < 16; i++) tx[i] = 0xFF000000u | (i * 16 + 8) << 16 | (255 - i * 16) << 8 | 0x40u;
    uint32_t t = texture(dev, 4, 4, tx), green = solid(dev, 0xFF00FF00u), white = solid(dev, 0xFFFFFFFFu);
    const uint32_t bg = 0xFF000000u;
    clear(dev, bg);
    CHECKEQ(SP(s, "Begin", 1, D3DXSPRITE_ALPHABLEND), D3D_OK);
    /* srcRect (2,2)-(4,4) at (10,10): only those 2x2 texels */
    CHECKEQ(SP(s, "Draw", 5, t, rect(2, 2, 4, 4), 0u, vec3(10, 10, 0), 0xFFFFFFFFu), D3D_OK);
    /* center (2,2,0) at (30,30): the quad from (28,28) */
    CHECKEQ(SP(s, "Draw", 5, t, 0u, vec3(2, 2, 0), vec3(30, 30, 0), 0xFFFFFFFFu), D3D_OK);
    /* 0x80FF0000 on white: half red over black */
    CHECKEQ(SP(s, "Draw", 5, white, 0u, 0u, vec3(50, 50, 0), 0x80FF0000u), D3D_OK);
    CHECKEQ(SP(s, "End", 0), D3D_OK);
    /* scale 2, translate (20,0): position (3,5) lands at (26,10), 8x8 */
    const float m[16] = { 2, 0, 0, 0, 0, 2, 0, 0, 0, 0, 1, 0, 20, 0, 0, 1 };
    CHECKEQ(SP(s, "SetTransform", 1, matrix(m)), D3D_OK);
    CHECKEQ(SP(s, "Begin", 1, D3DXSPRITE_ALPHABLEND), D3D_OK);
    CHECKEQ(SP(s, "Draw", 5, green, 0u, 0u, vec3(3, 5, 0), 0xFFFFFFFFu), D3D_OK);
    CHECKEQ(SP(s, "End", 0), D3D_OK);
    CHECKEQ(SP(s, "SetTransform", 1, matrix(g_identity)), D3D_OK);
    read_rt(dev, rt);
    expect_px(10, 10, tx[2 * 4 + 2], 1, "srcRect"), expect_px(11, 10, tx[2 * 4 + 3], 1, "srcRect");
    expect_px(10, 11, tx[3 * 4 + 2], 1, "srcRect"), expect_px(11, 11, tx[3 * 4 + 3], 1, "srcRect");
    expect_px(12, 10, bg, 1, "srcRect width"), expect_px(10, 12, bg, 1, "srcRect height");
    expect_px(9, 10, bg, 1, "srcRect left"), expect_px(10, 9, bg, 1, "srcRect top");
    expect_px(28, 28, tx[0], 1, "center"), expect_px(31, 31, tx[15], 1, "center");
    expect_px(27, 28, bg, 1, "center left"), expect_px(28, 27, bg, 1, "center top");
    expect_px(32, 31, bg, 1, "center right"), expect_px(31, 32, bg, 1, "center bottom");
    for (int y = 10; y < 18; y++)
        for (int x = 26; x < 34; x++) expect_px(x, y, 0xFF00FF00u, 1, "transform");
    expect_px(25, 10, bg, 1, "transform left"), expect_px(34, 10, bg, 1, "transform right");
    expect_px(26, 9, bg, 1, "transform top"), expect_px(26, 18, bg, 1, "transform bottom");
    expect_px(51, 51, 0xFF800000u, 1, "colour");
    release(s), release(t), release(green), release(white), release(rt);
}

/* ---- sprite_sort ---- */

static void group_sprite_sort(void) {
    uint32_t dev = new_device(), rt = argb_target(dev), s = new_sprite(dev);
    uint32_t a = solid(dev, 0xFFFF0000u), b = solid(dev, 0xFF00FF00u);
    const uint32_t bg = 0xFF000000u;
    static const uint32_t flags[] = { 0, D3DXSPRITE_SORT_TEXTURE };
    for (int k = 0; k < 2; k++) {
        uint32_t n0 = d3dx_sprite_draw_calls();
        CHECKEQ(SP(s, "Begin", 1, flags[k]), D3D_OK);
        CHECKEQ(SP(s, "Draw", 5, a, 0u, 0u, vec3(0, 0, 0), 0xFFFFFFFFu), D3D_OK);
        CHECKEQ(SP(s, "Draw", 5, b, 0u, 0u, vec3(8, 0, 0), 0xFFFFFFFFu), D3D_OK);
        CHECKEQ(SP(s, "Draw", 5, a, 0u, 0u, vec3(16, 0, 0), 0xFFFFFFFFu), D3D_OK);
        CHECKEQ(SP(s, "End", 0), D3D_OK);
        CHECKEQ(d3dx_sprite_draw_calls() - n0, k ? 2 : 3);
    }
    /* overlapping: red at z 0.2, then green at z 0.8 (depth test off) */
    CHECKEQ(DV(dev, "SetRenderState", 2, 7u, 0u), D3D_OK);
    static const struct { uint32_t flags; uint32_t want; const char *what; } c[] = {
        { 0, 0xFF00FF00u, "submission order" },
        { D3DXSPRITE_SORT_DEPTH_BACKTOFRONT, 0xFFFF0000u, "back to front" },
        { D3DXSPRITE_SORT_DEPTH_FRONTTOBACK, 0xFF00FF00u, "front to back" },
        { D3DXSPRITE_SORT_DEPTH_BACKTOFRONT | D3DXSPRITE_SORT_TEXTURE, 0xFFFF0000u, "depth, then texture" },
    };
    for (int k = 0; k < 4; k++) {
        clear(dev, bg);
        CHECKEQ(SP(s, "Begin", 1, c[k].flags), D3D_OK);
        CHECKEQ(SP(s, "Draw", 5, a, 0u, 0u, vec3(30, 30, 0.2f), 0xFFFFFFFFu), D3D_OK);
        CHECKEQ(SP(s, "Draw", 5, b, 0u, 0u, vec3(32, 32, 0.8f), 0xFFFFFFFFu), D3D_OK);
        CHECKEQ(SP(s, "End", 0), D3D_OK);
        read_rt(dev, rt);
        expect_px(33, 33, c[k].want, 1, c[k].what);
        expect_px(30, 30, 0xFFFF0000u, 1, c[k].what), expect_px(35, 35, 0xFF00FF00u, 1, c[k].what);
    }
    release(s), release(a), release(b), release(rt);
}

/* ---- sprite_state ---- */

/* the states a sprite draw sets: (kind, stage or sampler, state, the sprite's value, a test value) */
enum { K_RS, K_TSS, K_SS };
static const struct sstate { uint8_t kind, stage; uint16_t state; uint32_t sprite, test; } g_states[] = {
    { K_RS, 0, 27, 0, 1 },          /* ALPHABLENDENABLE (no ALPHABLEND flag) */
    { K_RS, 0, 25, 5, 7 },          /* ALPHAFUNC GREATER */
    { K_RS, 0, 24, 0, 0x40 },       /* ALPHAREF */
    { K_RS, 0, 15, 0, 1 },          /* ALPHATESTENABLE */
    { K_RS, 0, 171, 1, 2 },         /* BLENDOP ADD */
    { K_RS, 0, 19, 5, 2 },          /* SRCBLEND SRCALPHA */
    { K_RS, 0, 20, 6, 2 },          /* DESTBLEND INVSRCALPHA */
    { K_RS, 0, 206, 0, 1 },         /* SEPARATEALPHABLENDENABLE */
    { K_RS, 0, 136, 1, 0 },         /* CLIPPING */
    { K_RS, 0, 152, 0, 1 },         /* CLIPPLANEENABLE */
    { K_RS, 0, 168, 0xF, 7 },       /* COLORWRITEENABLE */
    { K_RS, 0, 22, 1, 2 },          /* CULLMODE NONE */
    { K_RS, 0, 145, 1, 0 },         /* DIFFUSEMATERIALSOURCE COLOR1 */
    { K_RS, 0, 8, 3, 2 },           /* FILLMODE SOLID */
    { K_RS, 0, 28, 0, 1 },          /* FOGENABLE */
    { K_RS, 0, 48, 0, 1 },          /* RANGEFOGENABLE */
    { K_RS, 0, 167, 0, 1 },         /* INDEXEDVERTEXBLENDENABLE */
    { K_RS, 0, 151, 0, 1 },         /* VERTEXBLEND */
    { K_RS, 0, 137, 0, 1 },         /* LIGHTING */
    { K_RS, 0, 9, 2, 1 },           /* SHADEMODE GOURAUD */
    { K_RS, 0, 29, 0, 1 },          /* SPECULARENABLE */
    { K_RS, 0, 194, 0, 1 },         /* SRGBWRITEENABLE */
    { K_RS, 0, 52, 0, 1 },          /* STENCILENABLE */
    { K_RS, 0, 128, 0, 1 },         /* WRAP0 */
    { K_TSS, 0, 1, 4, 3 },          /* COLOROP MODULATE */
    { K_TSS, 0, 2, 2, 1 },          /* COLORARG1 TEXTURE */
    { K_TSS, 0, 3, 0, 2 },          /* COLORARG2 DIFFUSE */
    { K_TSS, 0, 4, 4, 3 },          /* ALPHAOP MODULATE */
    { K_TSS, 0, 5, 2, 1 },          /* ALPHAARG1 TEXTURE */
    { K_TSS, 0, 6, 0, 2 },          /* ALPHAARG2 DIFFUSE */
    { K_TSS, 0, 11, 0, 1 },         /* TEXCOORDINDEX */
    { K_TSS, 0, 24, 0, 2 },         /* TEXTURETRANSFORMFLAGS DISABLE */
    { K_TSS, 1, 1, 1, 4 },          /* stage 1 COLOROP DISABLE */
    { K_TSS, 1, 4, 1, 2 },          /* stage 1 ALPHAOP DISABLE */
    { K_SS, 0, 1, 3, 1 },           /* ADDRESSU CLAMP */
    { K_SS, 0, 2, 3, 2 },           /* ADDRESSV CLAMP */
    { K_SS, 0, 5, 2, 1 },           /* MAGFILTER LINEAR */
    { K_SS, 0, 6, 2, 1 },           /* MINFILTER LINEAR */
    { K_SS, 0, 7, 2, 0 },           /* MIPFILTER LINEAR */
    { K_SS, 0, 9, 0, 2 },           /* MAXMIPLEVEL */
    { K_SS, 0, 10, 1, 4 },          /* MAXANISOTROPY */
    { K_SS, 0, 8, 0, 0x3F800000u }, /* MIPMAPLODBIAS */
    { K_SS, 0, 11, 0, 1 },          /* SRGBTEXTURE */
};
#define NSTATES (sizeof g_states / sizeof g_states[0])
static uint32_t state_get(uint32_t dev, const struct sstate *s) {
    return s->kind == K_RS ? rs_get(dev, s->state) : s->kind == K_TSS ? tss_get(dev, s->stage, s->state)
                                                                       : ss_get(dev, s->stage, s->state);
}
static void state_set(uint32_t dev, const struct sstate *s, uint32_t v) {
    if (s->kind == K_RS) CHECKEQ(DV(dev, "SetRenderState", 2, (uint32_t)s->state, v), D3D_OK);
    else if (s->kind == K_TSS) CHECKEQ(DV(dev, "SetTextureStageState", 3, (uint32_t)s->stage, (uint32_t)s->state, v), D3D_OK);
    else CHECKEQ(DV(dev, "SetSamplerState", 3, (uint32_t)s->stage, (uint32_t)s->state, v), D3D_OK);
}
static void expect_states(uint32_t dev, int sprite, uint32_t alphablend, const char *when) {
    for (size_t i = 0; i < NSTATES; i++) {
        const struct sstate *s = &g_states[i];
        uint32_t want = sprite ? s->sprite : s->test;
        if (sprite && alphablend && s->kind == K_RS && (s->state == 27 || s->state == 15)) want = 1;
        uint32_t got = state_get(dev, s);
        if (got != want) {
            printf("FAIL %s: %s %u.%u is 0x%X, want 0x%X\n", when, s->kind == K_RS ? "render state"
                   : s->kind == K_TSS ? "stage state" : "sampler state", s->stage, s->state, got, want);
            fails++;
        }
    }
}

static void group_sprite_state(void) {
    uint32_t dev = new_device(), rt = argb_target(dev), s = new_sprite(dev), t = solid(dev, 0xFFFF0000u);
    uint32_t vs = shader(dev, "vs_1_1\ndcl_position v0\nmov oPos, v0\n", 0);
    uint32_t ps = shader(dev, "ps_2_0\ndef c0, 1, 0, 0, 1\nmov oC0, c0\n", 1);
    static const uint32_t flags[] = { 0, D3DXSPRITE_ALPHABLEND };
    for (int k = 0; k < 2; k++) {
        for (size_t i = 0; i < NSTATES; i++) state_set(dev, &g_states[i], g_states[i].test);
        CHECKEQ(DV(dev, "SetFVF", 1, 0x112u), D3D_OK);
        CHECKEQ(DV(dev, "SetVertexShader", 1, vs), D3D_OK);
        CHECKEQ(DV(dev, "SetPixelShader", 1, ps), D3D_OK);
        CHECKEQ(SP(s, "Begin", 1, flags[k]), D3D_OK);
        CHECKEQ(SP(s, "Draw", 5, t, 0u, 0u, vec3(4, 4, 0), 0xFFFFFFFFu), D3D_OK);
        CHECKEQ(SP(s, "Flush", 0), D3D_OK);
        expect_states(dev, 1, flags[k], "after Flush");          /* the draw's own states */
        CHECKEQ(fvf_get(dev), 0x144);
        CHECKEQ(shader_get(dev, "GetVertexShader"), 0), CHECKEQ(shader_get(dev, "GetPixelShader"), 0);
        CHECKEQ(SP(s, "End", 0), D3D_OK);
        expect_states(dev, 0, 0, "after End");                    /* all back as set before Begin */
        CHECKEQ(fvf_get(dev), 0x112);
        CHECKEQ(shader_get(dev, "GetVertexShader"), vs), CHECKEQ(shader_get(dev, "GetPixelShader"), ps);
    }
    CHECKEQ(DV(dev, "SetVertexShader", 1, 0u), D3D_OK);
    CHECKEQ(DV(dev, "SetPixelShader", 1, 0u), D3D_OK);

    /* DONOTSAVESTATE: the sprite's values stay */
    CHECKEQ(DV(dev, "SetRenderState", 2, 27u, 1u), D3D_OK);
    CHECKEQ(SP(s, "Begin", 1, D3DXSPRITE_DONOTSAVESTATE), D3D_OK);
    CHECKEQ(SP(s, "Draw", 5, t, 0u, 0u, vec3(4, 4, 0), 0xFFFFFFFFu), D3D_OK);
    CHECKEQ(SP(s, "End", 0), D3D_OK);
    CHECKEQ(rs_get(dev, 27), 0);

    /* DONOTMODIFY_RENDERSTATE: the test's stage 0 (select the diffuse colour) is in force for the draw */
    for (size_t i = 0; i < NSTATES; i++) state_set(dev, &g_states[i], g_states[i].sprite);
    CHECKEQ(DV(dev, "SetTextureStageState", 3, 0u, 1u, 3u), D3D_OK);     /* COLOROP SELECTARG2 */
    CHECKEQ(DV(dev, "SetTextureStageState", 3, 0u, 4u, 3u), D3D_OK);     /* ALPHAOP SELECTARG2 */
    clear(dev, 0xFF000000u);
    CHECKEQ(SP(s, "Begin", 1, D3DXSPRITE_DONOTMODIFY_RENDERSTATE), D3D_OK);
    CHECKEQ(SP(s, "Draw", 5, t, 0u, 0u, vec3(4, 4, 0), 0xFF0000FFu), D3D_OK);
    CHECKEQ(SP(s, "Flush", 0), D3D_OK);
    CHECKEQ(tss_get(dev, 0, 1), 3);
    CHECKEQ(SP(s, "Draw", 5, t, 0u, 0u, vec3(20, 4, 0), 0xFFFFFFFFu), D3D_OK);
    CHECKEQ(SP(s, "End", 0), D3D_OK);
    read_rt(dev, rt);
    expect_px(5, 5, 0xFF0000FFu, 1, "DONOTMODIFY_RENDERSTATE: diffuse selected");   /* not red * blue */
    expect_px(21, 5, 0xFFFFFFFFu, 1, "DONOTMODIFY_RENDERSTATE: diffuse selected");
    /* the same draw without the flag modulates: red times blue is black */
    clear(dev, 0xFF808080u);
    CHECKEQ(SP(s, "Begin", 1, 0u), D3D_OK);
    CHECKEQ(SP(s, "Draw", 5, t, 0u, 0u, vec3(4, 4, 0), 0xFF0000FFu), D3D_OK);
    CHECKEQ(SP(s, "End", 0), D3D_OK);
    read_rt(dev, rt);
    expect_px(5, 5, 0xFF000000u, 1, "modulate");
    CHECKEQ(tss_get(dev, 0, 1), 3);                                   /* restored */
    release(s), release(t), release(vs), release(ps), release(rt);
}

/* ---- sprite_objectspace ---- */

static void group_sprite_objectspace(void) {
    uint32_t dev = new_device(), rt = argb_target(dev), s = new_sprite(dev), t = solid(dev, 0xFF00FF00u);
    /* view identity; projection: x 0..64 right, y 0..64 down, z 0..1 (an orthographic off-centre LH) */
    const float proj[16] = { 2.0f / 64, 0, 0, 0, 0, -2.0f / 64, 0, 0, 0, 0, 1, 0, -1, 1, 0, 1 };
    const float world[16] = { 3, 0, 0, 0, 0, 3, 0, 0, 0, 0, 3, 0, 1, 2, 3, 1 };
    CHECKEQ(DV(dev, "SetTransform", 2, 2u, matrix(g_identity)), D3D_OK);
    CHECKEQ(DV(dev, "SetTransform", 2, 3u, matrix(proj)), D3D_OK);
    CHECKEQ(DV(dev, "SetTransform", 2, 256u, matrix(world)), D3D_OK);
    /* the sprite at (4,4,0) under a sprite transform of translate (16.5, 8.5, 0.5): world x 20.5..24.5 */
    const float m[16] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 16.5f, 8.5f, 0.5f, 1 };
    CHECKEQ(SP(s, "SetTransform", 1, matrix(m)), D3D_OK);
    clear(dev, 0xFF000000u);
    CHECKEQ(SP(s, "Begin", 1, D3DXSPRITE_OBJECTSPACE), D3D_OK);
    CHECKEQ(SP(s, "Draw", 5, t, 0u, 0u, vec3(4, 4, 0), 0xFFFFFFFFu), D3D_OK);
    CHECKEQ(SP(s, "Flush", 0), D3D_OK);
    CHECKEQ(fvf_get(dev), 0x142);
    uint32_t got = gmem(64);
    CHECKEQ(DV(dev, "GetTransform", 2, 256u, got), D3D_OK);
    CHECK(!memcmp(hle_ptr(got), m, 64));                              /* world = the sprite transform */
    CHECKEQ(SP(s, "End", 0), D3D_OK);
    read_rt(dev, rt);
    for (int y = 13; y <= 16; y++)
        for (int x = 21; x <= 24; x++) expect_px(x, y, 0xFF00FF00u, 1, "object space");
    expect_px(20, 14, 0xFF000000u, 1, "object space left"), expect_px(25, 14, 0xFF000000u, 1, "object space right");
    expect_px(22, 12, 0xFF000000u, 1, "object space top"), expect_px(22, 17, 0xFF000000u, 1, "object space bottom");
    CHECKEQ(DV(dev, "GetTransform", 2, 256u, got), D3D_OK);
    CHECK(!memcmp(hle_ptr(got), world, 64));                          /* the world transform is back */
    CHECKEQ(DV(dev, "GetTransform", 2, 3u, got), D3D_OK);
    CHECK(!memcmp(hle_ptr(got), proj, 64));
    /* screen space leaves the device's transforms alone */
    CHECKEQ(SP(s, "Begin", 1, D3DXSPRITE_DONOTSAVESTATE), D3D_OK);
    CHECKEQ(SP(s, "Draw", 5, t, 0u, 0u, vec3(4, 4, 0), 0xFFFFFFFFu), D3D_OK);
    CHECKEQ(SP(s, "End", 0), D3D_OK);
    CHECKEQ(DV(dev, "GetTransform", 2, 256u, got), D3D_OK);
    CHECK(!memcmp(hle_ptr(got), world, 64));
    release(s), release(t), release(rt);
}

/* ---- sprite_rules ---- */

static uint32_t g_sprite;
static void c_billboard(void) { SP(g_sprite, "Begin", 1, D3DXSPRITE_BILLBOARD); }
static void c_unknown_flag(void) { SP(g_sprite, "Begin", 1, 0x100u); }
static void c_both_depths(void) {
    SP(g_sprite, "Begin", 1, D3DXSPRITE_SORT_DEPTH_FRONTTOBACK | D3DXSPRITE_SORT_DEPTH_BACKTOFRONT);
}
static void c_worldview_lh(void) { SP(g_sprite, "SetWorldViewLH", 2, 0u, 0u); }
static void c_worldview_rh(void) { SP(g_sprite, "SetWorldViewRH", 2, 0u, 0u); }

static void group_sprite_rules(void) {
    uint32_t dev = new_device(), out = gmem(4), t = solid(dev, 0xFFFFFFFFu), live0 = d3d_live_count(), dref0 = refs(dev);
    CHECKEQ(call("D3DXCreateSprite", 2, 0u, out), D3DERR_INVALIDCALL);
    CHECKEQ(call("D3DXCreateSprite", 2, dev, 0u), D3DERR_INVALIDCALL);
    uint32_t s = g_sprite = new_sprite(dev);
    CHECKEQ(refs(s), 1);
    CHECKEQ(refs(dev), dref0 + 1);
    /* QueryInterface: ID3DXSprite and IUnknown */
    static const uint8_t iid_sprite[16] = { 0x2D, 0x76, 0x0B, 0xBA, 0x28, 0x7D, 0xEC, 0x43,
                                            0xB9, 0xDC, 0x2F, 0x84, 0x44, 0x3B, 0x06, 0x14 };
    CHECKEQ(SP(s, "QueryInterface", 2, gbytes(iid_sprite, 16), out), D3D_OK);
    CHECKEQ(MEM32(out), s);
    release(s);
    /* GetDevice AddRefs */
    CHECKEQ(SP(s, "GetDevice", 1, 0u), D3DERR_INVALIDCALL);
    CHECKEQ(SP(s, "GetDevice", 1, out), D3D_OK);
    CHECKEQ(MEM32(out), dev);
    CHECKEQ(refs(dev), dref0 + 2);
    release(dev);
    /* GetTransform/SetTransform: identity at creation, then a round trip */
    uint32_t m = gmem(64);
    CHECKEQ(SP(s, "GetTransform", 1, m), D3D_OK);
    CHECK(!memcmp(hle_ptr(m), g_identity, 64));
    const float k[16] = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16 };
    CHECKEQ(SP(s, "SetTransform", 1, matrix(k)), D3D_OK);
    CHECKEQ(SP(s, "GetTransform", 1, m), D3D_OK);
    CHECK(!memcmp(hle_ptr(m), k, 64));
    CHECKEQ(SP(s, "GetTransform", 1, 0u), D3DERR_INVALIDCALL);
    CHECKEQ(SP(s, "SetTransform", 1, 0u), D3DERR_INVALIDCALL);
    CHECKEQ(SP(s, "SetTransform", 1, matrix(g_identity)), D3D_OK);
    /* outside Begin */
    CHECKEQ(SP(s, "Draw", 5, t, 0u, 0u, 0u, 0xFFFFFFFFu), D3DERR_INVALIDCALL);
    CHECKEQ(SP(s, "Flush", 0), D3DERR_INVALIDCALL);
    CHECKEQ(SP(s, "End", 0), D3DERR_INVALIDCALL);
    /* a second Begin, a NULL texture */
    uint32_t tref0 = refs(t);
    CHECKEQ(SP(s, "Begin", 1, 0u), D3D_OK);
    CHECKEQ(SP(s, "Begin", 1, 0u), D3DERR_INVALIDCALL);
    CHECKEQ(SP(s, "Draw", 5, 0u, 0u, 0u, 0u, 0xFFFFFFFFu), D3DERR_INVALIDCALL);
    CHECKEQ(SP(s, "Draw", 5, t, 0u, 0u, 0u, 0xFFFFFFFFu), D3D_OK);
    CHECKEQ(refs(t), tref0 + 1);                                     /* queued: held */
    CHECKEQ(SP(s, "Draw", 5, t, 0u, 0u, 0u, 0xFFFFFFFFu), D3D_OK);
    CHECKEQ(refs(t), tref0 + 2);
    CHECKEQ(SP(s, "Flush", 0), D3D_OK);
    CHECKEQ(refs(t), tref0 + 1);                                     /* drawn: only stage 0 holds it */
    CHECKEQ(SP(s, "Draw", 5, t, 0u, 0u, 0u, 0xFFFFFFFFu), D3D_OK);
    CHECKEQ(SP(s, "End", 0), D3D_OK);
    CHECKEQ(refs(t), tref0);                                         /* End's Apply unbound it */
    CHECKEQ(SP(s, "End", 0), D3DERR_INVALIDCALL);
    /* loud stops */
    expect_stop(c_billboard, "D3DXSPRITE_BILLBOARD (0x8)", "BILLBOARD");
    expect_stop(c_unknown_flag, "flag 0x100", "an unknown flag");
    expect_stop(c_both_depths, "FRONTTOBACK and", "both depth orders");
    expect_stop(c_worldview_lh, "ID3DXSprite::SetWorldViewLH not implemented", "SetWorldViewLH");
    expect_stop(c_worldview_rh, "ID3DXSprite::SetWorldViewRH not implemented", "SetWorldViewRH");
    /* the last Release lets go of the queue, the state block and the device */
    CHECKEQ(SP(s, "Begin", 1, 0u), D3D_OK);
    CHECKEQ(SP(s, "Draw", 5, t, 0u, 0u, 0u, 0xFFFFFFFFu), D3D_OK);
    CHECKEQ(refs(t), tref0 + 1);
    CHECKEQ(SP(s, "Release", 0), 0);
    CHECKEQ(refs(t), tref0);
    CHECKEQ(refs(dev), dref0);
    CHECKEQ(d3d_live_count(), live0);
    /* d3dx_sprite_create, as the font will use it */
    uint32_t s2 = d3dx_sprite_create(dev);
    CHECKEQ(refs(s2), 1);
    CHECKEQ(refs(dev), dref0 + 1);
    release(s2);
    CHECKEQ(refs(dev), dref0);
    release(t);
}

/* ---- sprite_lost ---- */

static void group_sprite_lost(void) {
    uint32_t dev = new_device(), rt = argb_target(dev), s = new_sprite(dev), t = solid(dev, 0xFF00FF00u);
    uint32_t tref0 = refs(t);
    clear(dev, 0xFF000000u);
    CHECKEQ(SP(s, "Begin", 1, 0u), D3D_OK);
    CHECKEQ(SP(s, "Draw", 5, t, 0u, 0u, vec3(10, 10, 0), 0xFFFFFFFFu), D3D_OK);
    uint32_t n0 = d3dx_sprite_draw_calls();
    CHECKEQ(SP(s, "OnLostDevice", 0), D3D_OK);
    CHECKEQ(d3dx_sprite_draw_calls(), n0);                           /* nothing drawn */
    CHECKEQ(refs(t), tref0);                                         /* End's Apply unbound it */
    CHECKEQ(SP(s, "End", 0), D3DERR_INVALIDCALL);                    /* the batch is over */
    read_rt(dev, rt);
    expect_px(11, 11, 0xFF000000u, 1, "lost: not drawn");
    /* Reset: the target (D3DPOOL_DEFAULT) goes first; the sprite holds nothing that blocks it */
    release(rt);
    CHECKEQ(DV(dev, "Reset", 1, g_pp), D3D_OK);
    CHECKEQ(SP(s, "OnResetDevice", 0), D3D_OK);
    rt = argb_target(dev);
    clear(dev, 0xFF000000u);
    CHECKEQ(SP(s, "Begin", 1, 0u), D3D_OK);
    CHECKEQ(SP(s, "Draw", 5, t, 0u, 0u, vec3(10, 10, 0), 0xFFFFFFFFu), D3D_OK);
    CHECKEQ(SP(s, "End", 0), D3D_OK);
    CHECKEQ(d3dx_sprite_draw_calls(), n0 + 1);
    read_rt(dev, rt);
    expect_px(11, 11, 0xFF00FF00u, 1, "after reset: drawn");
    expect_px(9, 9, 0xFF000000u, 1, "after reset: around");
    release(s), release(t), release(rt);
}

/* ---- fonts (font.h): the files and expectations come from tests/test_ui.py ---- */

static char **g_arg;                                   /* argv + 2: the group's arguments */
static void print_tm(const char *label, const struct bw2_tm *t) {
    printf("tm %s height=%d ascent=%d descent=%d internal_leading=%d external_leading=%d ave_char_width=%d "
           "max_char_width=%d weight=%d overhang=%d aspect_x=%d aspect_y=%d first=%u last=%u deflt=%u brk=%u "
           "italic=%u underlined=%u struck_out=%u pitch_family=%u charset=%u ppem=%u\n", label, t->height,
           t->ascent, t->descent, t->internal_leading, t->external_leading, t->ave_char_width, t->max_char_width,
           t->weight, t->overhang, t->aspect_x, t->aspect_y, t->first, t->last, t->deflt, t->brk, t->italic,
           t->underlined, t->struck_out, t->pitch_family, t->charset, t->ppem);
}
static struct bw2_font *open_at(const char *face, int32_t h, uint32_t w, int it, int line) {
    char err[256] = "";
    struct bw2_font *f = bw2_font_open(face, h, w, it, err, sizeof err);
    if (!f) printf("FAIL %s:%d open \"%s\" %d weight %u italic %d: %s\n", __FILE__, line, face, h, w, it, err), fails++;
    return f;
}
#define OPEN(face, h, w, it) open_at(face, h, w, it, __LINE__)
static void refused_at(const char *face, int32_t h, uint32_t w, int it, const char *want, int line) {
    char err[256] = "";
    struct bw2_font *f = bw2_font_open(face, h, w, it, err, sizeof err);
    if (f || !strstr(err, want)) {
        printf("FAIL %s:%d open \"%s\" %d weight %u italic %d: %s, message \"%s\", want \"%s\"\n", __FILE__, line,
               face, h, w, it, f ? "opened" : "refused", err, want);
        fails++;
    }
    if (f) bw2_font_close(f);
}
#define REFUSED(face, h, w, it, want) refused_at(face, h, w, it, want, __LINE__)
/* a host file into guest memory */
static uint32_t gfile(const char *path, uint32_t *len) {
    FILE *fp = fopen(path, "rb");
    if (!fp) { printf("FAIL cannot read %s\n", path); fails++; *len = 0; return 0; }
    fseek(fp, 0, SEEK_END);
    long n = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    uint32_t p = gmem((uint32_t)n + 1);
    CHECKEQ(fread(hle_ptr(p), 1, (size_t)n, fp), (size_t)n);
    fclose(fp);
    *len = (uint32_t)n;
    return p;
}
/* a gdi32 export as the game finds it: GetProcAddress(LoadLibraryA("gdi32.dll"), name) */
static uint32_t gdi_call(const char *name, int n, ...) {
    uint32_t mod = call("LoadLibraryA", 1, gbytes("gdi32.dll", 9));
    uint32_t va = call("GetProcAddress", 2, mod, gbytes(name, (uint32_t)strlen(name)));
    recomp_func_t f = va ? recomp_lookup_import(va) : NULL;
    if (!f) { printf("FAIL GetProcAddress(gdi32.dll, %s): not answered\n", name); fails++; return 0xDEADBEEFu; }
    uint32_t a[8];
    va_list ap;
    va_start(ap, n);
    for (int i = 0; i < n; i++) a[i] = va_arg(ap, uint32_t);
    va_end(ap);
    return run(f, name, n, a);
}
static uint32_t add_file(const char *path, uint32_t *count) {
    uint32_t len, p = gfile(path, &len), pc = gmem(4);
    MEM32(pc) = 0xCCCCCCCCu;
    uint32_t h = gdi_call("AddFontMemResourceEx", 4, p, len, 0u, pc);
    *count = MEM32(pc);
    return h;
}
static uint32_t add_ok(const char *path) {
    uint32_t n, h = add_file(path, &n);
    CHECK(h != 0);
    CHECKEQ(n, 1);
    return h;
}
static int is_memory(const struct bw2_font *f) { return f && !strcmp(bw2_font_source(f), "memory"); }
static uint32_t glyph_of(const struct bw2_font *f, char c) { return bw2_font_glyph(f, (uint8_t)c); }

/* argv: the Arial Narrow file, the Courier New file, Arial Narrow without VDMX renamed "BW2 No VDMX" */
static void group_font_metrics(void) {
    static const int32_t hs[] = { -8, -16, -46, 20 };
    for (int i = 0; i < 4; i++) {
        struct bw2_font *f = OPEN("Arial Narrow", hs[i], 400, 0);
        if (!f) continue;
        char label[32];
        snprintf(label, sizeof label, "arial%d", hs[i]);
        print_tm(label, bw2_font_metrics(f));
        if (i == 0) printf("source arial %s\n", bw2_font_source(f));
        if (hs[i] == -16) {                                     /* its VDMX entry, not OS/2's 18 */
            CHECKEQ(bw2_font_metrics(f)->height, 20);
            CHECKEQ(bw2_font_metrics(f)->ascent, 16);
            CHECKEQ(bw2_font_metrics(f)->descent, 4);
        }
        bw2_font_close(f);
    }
    struct bw2_font *f = OPEN("Courier New", -16, 400, 0);
    if (f) {
        print_tm("courier-16", bw2_font_metrics(f));
        printf("source courier %s\n", bw2_font_source(f));
        CHECKEQ(bw2_font_metrics(f)->pitch_family, 0x36);
        bw2_font_close(f);
    }
    f = OPEN("Arial Narrow", -16, 0, 0);                         /* weight 0 reads as 400 */
    if (f) CHECKEQ(bw2_font_metrics(f)->weight, 400), bw2_font_close(f);
    f = OPEN("Arial Narrow", -16, 700, 0);
    if (f) CHECKEQ(bw2_font_metrics(f)->weight, 700), bw2_font_close(f);
    add_ok(g_arg[2]);
    f = OPEN("BW2 No VDMX", -16, 400, 0);                        /* the OS/2 rule */
    if (f) {
        print_tm("novdmx-16", bw2_font_metrics(f));
        CHECK(is_memory(f));
        CHECKEQ(bw2_font_metrics(f)->height, 18);
        CHECKEQ(bw2_font_metrics(f)->ascent, 15);
        CHECKEQ(bw2_font_metrics(f)->descent, 3);
        bw2_font_close(f);
    }
}

/* argv: Arial Narrow without hdmx renamed "BW2 No HDMX". Prints glyphs and advances for Python. */
static void group_font_advance(void) {
    add_ok(g_arg[0]);
    static const char *const faces[2] = { "Arial Narrow", "BW2 No HDMX" };
    for (int k = 0; k < 2; k++) {
        struct bw2_font *f = OPEN(faces[k], -16, 400, 0);
        if (!f) continue;
        CHECKEQ(is_memory(f), k == 1);
        for (const char *c = "AWi "; *c; c++) {
            uint32_t g = glyph_of(f, *c);
            CHECK(g != 0);
            printf("adv %d %d glyph=%u advance=%d\n", k, *c, g, bw2_font_advance(f, g));
        }
        bw2_font_close(f);
    }
}

/* argv: Arial Narrow renamed "BW2 Test Face", Arial Narrow renamed "Courier New", Arial Narrow's maxp
 * count, Courier New's maxp count, a collection of "BW2 TTC One" (Arial Narrow) and "BW2 TTC Two"
 * (Courier New), Arial Narrow Bold renamed "BW2 Bold Face" */
static void group_font_memory(void) {
    uint32_t arial_n = (uint32_t)atoi(g_arg[2]), courier_n = (uint32_t)atoi(g_arg[3]);
    /* 1-2: added through gdi32, then opened by family, case-insensitively */
    uint32_t h = add_ok(g_arg[0]);
    struct bw2_font *f = OPEN("BW2 Test Face", -16, 400, 0), *f2 = OPEN("bw2 TEST face", -16, 400, 0);
    CHECK(is_memory(f) && is_memory(f2));
    if (f2) bw2_font_close(f2);
    struct bw2_glyph before = { 0 }, after = { 0 };
    if (f) CHECKEQ(bw2_font_raster(f, glyph_of(f, 'A'), &before), 1);
    CHECK(before.w > 0 && before.h > 0 && before.alpha);
    /* 3: removed, it is no longer found; an unknown handle is FALSE */
    CHECKEQ(gdi_call("RemoveFontMemResourceEx", 1, h), 1);
    REFUSED("BW2 Test Face", -16, 400, 0, "no font named \"BW2 Test Face\"");
    CHECKEQ(gdi_call("RemoveFontMemResourceEx", 1, h), 0);
    CHECKEQ(bw2_font_remove_memory(0x12345u), 0);
    /* 5: a face opened before still rasterizes, to the same coverage */
    if (f) {
        CHECKEQ(bw2_font_raster(f, glyph_of(f, 'A'), &after), 1);
        CHECK(after.w == before.w && after.h == before.h && after.left == before.left && after.top == before.top);
        CHECK(after.alpha && before.alpha && !memcmp(after.alpha, before.alpha, (size_t)before.w * before.h));
        bw2_font_close(f);
    }
    free(before.alpha), free(after.alpha);
    /* 4: a memory face wins over the system's of the same name */
    h = add_ok(g_arg[1]);
    f = OPEN("Courier New", -16, 400, 0);
    if (f) CHECKEQ(bw2_font_glyph_count(f), arial_n), CHECK(is_memory(f)), bw2_font_close(f);
    CHECKEQ(bw2_font_remove_memory(h), 1);
    f = OPEN("Courier New", -16, 400, 0);
    if (f) CHECKEQ(bw2_font_glyph_count(f), courier_n), CHECK(!is_memory(f)), bw2_font_close(f);
    /* a collection: one handle, two faces, each its own */
    uint32_t n;
    h = add_file(g_arg[4], &n);
    CHECK(h != 0);
    CHECKEQ(n, 2);
    f = OPEN("BW2 TTC One", -16, 400, 0), f2 = OPEN("BW2 TTC Two", -16, 400, 0);
    if (f) CHECKEQ(bw2_font_glyph_count(f), arial_n), CHECK(is_memory(f)), bw2_font_close(f);
    if (f2) CHECKEQ(bw2_font_glyph_count(f2), courier_n), CHECK(is_memory(f2)), bw2_font_close(f2);
    CHECKEQ(bw2_font_remove_memory(h), 1);
    /* by full name (name ID 4): the bold face's full name has its style in it */
    h = add_ok(g_arg[5]);
    f = OPEN("BW2 Bold Face Bold", -16, 700, 0), f2 = OPEN("BW2 Bold Face", -16, 700, 0);
    CHECK(is_memory(f) && is_memory(f2));
    if (f) bw2_font_close(f);
    if (f2) bw2_font_close(f2);
    REFUSED("BW2 Bold Face", -16, 400, 0, "regular");
    CHECKEQ(bw2_font_remove_memory(h), 1);
}

/* argv: Arial Narrow renamed "BW2 Test Face" */
static void group_font_errors(void) {
    REFUSED("No Such Face", -16, 400, 0, "no font named \"No Such Face\"");
    struct bw2_font *f = OPEN("Arial Narrow", -16, 700, 0);       /* macOS has the bold face */
    if (f) {
        printf("source arial-bold %s\n", bw2_font_source(f));
        CHECK(!is_memory(f));
        bw2_font_close(f);
    }
    add_ok(g_arg[0]);
    REFUSED("BW2 Test Face", -16, 700, 0, "bold");               /* memory only, Regular */
    REFUSED("BW2 Test Face", -16, 400, 1, "italic");
    REFUSED("BW2 Test Face", 0, 400, 0, "height 0");
    REFUSED("Arial Narrow", 0, 400, 0, "height 0");
    /* not a font: NULL, with a reason on stderr */
    uint32_t pc = gmem(4), zeros = gmem(16);
    MEM32(pc) = 0xCCCCCCCCu;
    CHECKEQ(gdi_call("AddFontMemResourceEx", 4, zeros, 16u, 0u, pc), 0);
    CHECKEQ(MEM32(pc), 0);
    /* reserved not NULL, no count pointer, no data: NULL with ERROR_INVALID_PARAMETER */
    uint32_t len, p = gfile(g_arg[0], &len);
    call("SetLastError", 1, 0u);
    CHECKEQ(gdi_call("AddFontMemResourceEx", 4, p, len, pc, pc), 0);
    CHECKEQ(call("GetLastError", 0), 87);
    call("SetLastError", 1, 0u);
    CHECKEQ(gdi_call("AddFontMemResourceEx", 4, p, len, 0u, 0u), 0);
    CHECKEQ(call("GetLastError", 0), 87);
    call("SetLastError", 1, 0u);
    CHECKEQ(gdi_call("AddFontMemResourceEx", 4, 0u, len, 0u, pc), 0);
    CHECKEQ(call("GetLastError", 0), 87);
}

/* argv: the Arial Narrow file, Courier New renamed "Arial Narrow" with Arial Narrow's PostScript name,
 * Courier New's maxp count */
static void group_font_fallthrough(void) {
    /* a memory face with a system face's PostScript name still draws from its own data */
    uint32_t h = add_ok(g_arg[1]);
    struct bw2_font *f = OPEN("Arial Narrow", -16, 400, 0);
    if (f) CHECKEQ(bw2_font_glyph_count(f), (uint32_t)atoi(g_arg[2])), CHECK(is_memory(f)), bw2_font_close(f);
    CHECKEQ(bw2_font_remove_memory(h), 1);
    /* the game's case: the Regular face from memory, the other styles from the system */
    add_ok(g_arg[0]);
    static const struct { uint32_t w; int it; const char *label; } st[] = {
        { 400, 0, "regular" }, { 700, 0, "bold" }, { 400, 1, "italic" }, { 700, 1, "bolditalic" },
    };
    for (int i = 0; i < 4; i++) {
        f = OPEN("Arial Narrow", -16, st[i].w, st[i].it);
        if (!f) continue;
        printf("source %s %s\n", st[i].label, bw2_font_source(f));
        CHECKEQ(is_memory(f), i == 0);
        CHECKEQ(bw2_font_metrics(f)->italic, st[i].it ? 255 : 0);
        bw2_font_close(f);
    }
}

/* argv: the cap height and the glyf box of 'I' (x0 y0 x1 y1), in pixels at ppem 46 */
static void group_font_raster(void) {
    double cap = atof(g_arg[0]), bx0 = atof(g_arg[1]), by0 = atof(g_arg[2]), bx1 = atof(g_arg[3]), by1 = atof(g_arg[4]);
    struct bw2_font *f = OPEN("Arial Narrow", -46, 400, 0);
    if (!f) return;
    CHECKEQ(bw2_font_metrics(f)->ppem, 46);
    struct bw2_glyph g = { 0 };
    CHECKEQ(bw2_font_raster(f, glyph_of(f, 'I'), &g), 1);
    printf("raster I left=%d top=%d w=%u h=%u\n", g.left, g.top, g.w, g.h);
    CHECK(g.h >= 0.6 * 46 && g.h <= 0.8 * 46);
    CHECK(abs(g.top - (int)lround(cap)) <= 2);
    CHECK(abs(g.left - (int)floor(bx0)) <= 1);                   /* one stem wide: the glyf box rounded out */
    CHECK(abs((int)g.w - ((int)ceil(bx1) - (int)floor(bx0))) <= 1);
    CHECK(abs(g.top - (int)ceil(by1)) <= 1 && abs(g.top - (int)g.h - (int)floor(by0)) <= 1);
    int full = 0;
    if (g.alpha)
        for (uint32_t x = 0; x < g.w; x++) full += g.alpha[(g.h / 2) * g.w + x] == 255;
    CHECK(full >= 2);                                            /* the stem is solid */
    free(g.alpha);
    struct bw2_glyph sp = { 1, 1, 1, 1, (uint8_t *)1 };
    CHECKEQ(bw2_font_raster(f, glyph_of(f, ' '), &sp), 1);
    CHECK(sp.w == 0 && sp.h == 0 && sp.alpha == NULL);
    struct bw2_glyph gg = { 0 };
    CHECKEQ(bw2_font_raster(f, glyph_of(f, 'g'), &gg), 1);
    printf("raster g left=%d top=%d w=%u h=%u\n", gg.left, gg.top, gg.w, gg.h);
    CHECK(gg.top - (int32_t)gg.h < 0);
    free(gg.alpha);
    bw2_font_close(f);
}

static void font_cp1252_draw(void);
static void group_font_cp1252(void) {
    CHECKEQ(bw2_cp1252(0x41), 0x41);
    CHECKEQ(bw2_cp1252(0x80), 0x20AC);
    CHECKEQ(bw2_cp1252(0x92), 0x2019);
    CHECKEQ(bw2_cp1252(0x9F), 0x0178);
    CHECKEQ(bw2_cp1252(0xE9), 0xE9);
    CHECKEQ(bw2_cp1252(0x81), 0x81);
    CHECKEQ(bw2_cp1252(0x8D), 0x8D), CHECKEQ(bw2_cp1252(0x8F), 0x8F), CHECKEQ(bw2_cp1252(0x90), 0x90);
    CHECKEQ(bw2_cp1252(0x9D), 0x9D);
    CHECKEQ(bw2_cp1252(0x7F), 0x7F), CHECKEQ(bw2_cp1252(0xA0), 0xA0), CHECKEQ(bw2_cp1252(0xFF), 0xFF);
    if (g_arg[0]) font_cp1252_draw();
}

/* ---- ID3DXFont (d3dx_font.c): a 256x64 A8R8G8B8 target. argv[0] is the Mac's Arial Narrow renamed
 * "BW2 Test Face", added through AddFontMemResourceEx, so the memory path is the one drawn from. The
 * expectations come from the same face opened through font.h. ---- */

#define FW 256u
#define FH 64u
#define TEST_FACE "BW2 Test Face"
#define DT_CENTER 0x1u
#define DT_RIGHT 0x2u
#define DT_VCENTER 0x4u
#define DT_BOTTOM 0x8u
#define DT_WORDBREAK 0x10u
#define DT_SINGLELINE 0x20u
#define DT_EXPANDTABS 0x40u
#define DT_NOCLIP 0x100u
#define DT_CALCRECT 0x400u
#define WHITE 0xFFFFFFFFu
#define BLACK 0xFF000000u
#define FN(f, m, n, ...) mcall(f, "ID3DXFont", m, n, ##__VA_ARGS__)

static uint32_t g_fpx[FW * FH];
static uint32_t wide_target(uint32_t dev) {
    uint32_t out = gmem(4);
    CHECKEQ(DV(dev, "CreateRenderTarget", 8, FW, FH, 21u, 0u, 0u, 0u, out, 0u), D3D_OK);
    CHECKEQ(DV(dev, "SetRenderTarget", 2, 0u, MEM32(out)), D3D_OK);
    return MEM32(out);
}
static void read_wide(uint32_t dev, uint32_t rt) { read_into(dev, rt, g_fpx, FW, FH); }
static uint32_t create_font(uint32_t dev, const char *face, int32_t h, uint32_t width, uint32_t mips, uint32_t charset) {
    uint32_t out = gmem(4);
    CHECKEQ(call("D3DXCreateFontA", 12, dev, (uint32_t)h, width, 400u, mips, 0u, charset, 7u, 4u, 0u,
                 gbytes(face, (uint32_t)strlen(face)), out), D3D_OK);
    return MEM32(out);
}
static uint32_t new_font(uint32_t dev, const char *face, int32_t h) {
    uint32_t f = create_font(dev, face, h, 0, 1, 1);
    CHECK(f != 0);
    return f;
}
static int32_t text_a(uint32_t font, uint32_t sprite, const char *s, int32_t count, uint32_t rc, uint32_t fmt,
                      uint32_t color) {
    return (int32_t)FN(font, "DrawTextA", 6, sprite, gbytes(s, (uint32_t)strlen(s)), (uint32_t)count, rc, fmt, color);
}
static int32_t text_w(uint32_t font, uint32_t sprite, const uint16_t *s, int32_t count, uint32_t rc, uint32_t fmt,
                      uint32_t color) {
    uint32_t n = 0;
    while (s[n]) n++;
    return (int32_t)FN(font, "DrawTextW", 6, sprite, gbytes(s, 2 * n + 2), (uint32_t)count, rc, fmt, color);
}
static int32_t rc_at(uint32_t r, int i) { return (int32_t)MEM32(r + 4u * (uint32_t)i); }

/* where a line of Windows-1252 text puts its glyphs: the black box's top-left, from the pen at x */
static uint32_t expect_line(const struct bw2_font *ref, const char *s, int32_t x, int32_t baseline,
                            struct d3dx_glyph_pos *out) {
    uint32_t n = 0;
    for (; *s; s++) {
        uint32_t g = bw2_font_glyph(ref, bw2_cp1252((uint8_t)*s));
        struct bw2_glyph b;
        bw2_font_raster(ref, g, &b);
        out[n++] = (struct d3dx_glyph_pos){ g, x + b.left, baseline - b.top };
        free(b.alpha);
        x += bw2_font_advance(ref, g);
    }
    return n;
}
static int32_t width_of(const struct bw2_font *ref, const char *s) {
    int32_t w = 0;
    for (; *s; s++) w += bw2_font_advance(ref, bw2_font_glyph(ref, bw2_cp1252((uint8_t)*s)));
    return w;
}
static struct d3dx_glyph_pos g_lay[1024];
static uint32_t layout(void) {
    uint32_t n = d3dx_font_last_layout(g_lay, 1024);
    CHECK(n <= 1024);
    return n;
}
static void expect_layout(const struct d3dx_glyph_pos *want, uint32_t n, const char *what) {
    uint32_t k = layout();
    if (k != n) { printf("FAIL %s: %u glyphs placed, want %u\n", what, k, n); fails++; return; }
    for (uint32_t i = 0; i < n; i++)
        if (g_lay[i].glyph != want[i].glyph || g_lay[i].x != want[i].x || g_lay[i].y != want[i].y) {
            printf("FAIL %s: glyph %u is %u at (%d,%d), want %u at (%d,%d)\n", what, i, g_lay[i].glyph, g_lay[i].x,
                   g_lay[i].y, want[i].glyph, want[i].x, want[i].y);
            fails++;
            return;
        }
}
/* The whole target against the glyphs' coverage alpha-blended in colour over black, in order (within
 * 2/255); a pixel outside every black box (or outside clip, l t r b) is black. */
static int expect_text(const struct bw2_font *ref, const struct d3dx_glyph_pos *p, uint32_t n, uint32_t color,
                       const int32_t *clip, const char *what) {
    static float want[FW * FH][3];
    memset(want, 0, sizeof want);
    float c[3] = { (float)(color >> 16 & 0xFF), (float)(color >> 8 & 0xFF), (float)(color & 0xFF) };
    float ca = (float)(color >> 24) / 255.0f;
    for (uint32_t k = 0; k < n; k++) {
        struct bw2_glyph g;
        bw2_font_raster(ref, p[k].glyph, &g);
        for (uint32_t yy = 0; yy < g.h; yy++)
            for (uint32_t xx = 0; xx < g.w; xx++) {
                int32_t X = p[k].x + (int32_t)xx, Y = p[k].y + (int32_t)yy;
                if (X < 0 || Y < 0 || X >= (int32_t)FW || Y >= (int32_t)FH) continue;
                if (clip && (X < clip[0] || Y < clip[1] || X >= clip[2] || Y >= clip[3])) continue;
                float a = (float)g.alpha[yy * g.w + xx] / 255.0f * ca;
                for (int i = 0; i < 3; i++) want[Y * FW + X][i] = c[i] * a + want[Y * FW + X][i] * (1 - a);
            }
        free(g.alpha);
    }
    int bad = 0;
    for (uint32_t i = 0; i < FW * FH; i++)
        for (int ch = 0; ch < 3; ch++) {
            int got = (int)(g_fpx[i] >> (16 - 8 * ch) & 0xFF), w = (int)lroundf(want[i][ch]);
            if (abs(got - w) > 2) {
                if (bad++ < 6)
                    printf("FAIL %s: pixel (%u,%u) 0x%08X, want %d in channel %d\n", what, i % FW, i / FW, g_fpx[i], w,
                           ch);
                break;
            }
        }
    if (bad > 6) printf("FAIL %s: %d pixels in all\n", what, bad);
    fails += bad;
    return bad == 0;
}
static void print_tma(const char *label, uint32_t tm) {
    const uint8_t *b = hle_ptr(tm + 44);
    printf("tm %s height=%d ascent=%d descent=%d internal_leading=%d external_leading=%d ave_char_width=%d "
           "max_char_width=%d weight=%d overhang=%d aspect_x=%d aspect_y=%d first=%u last=%u deflt=%u brk=%u "
           "italic=%u underlined=%u struck_out=%u pitch_family=%u charset=%u\n", label,
           (int32_t)MEM32(tm), (int32_t)MEM32(tm + 4), (int32_t)MEM32(tm + 8), (int32_t)MEM32(tm + 12),
           (int32_t)MEM32(tm + 16), (int32_t)MEM32(tm + 20), (int32_t)MEM32(tm + 24), (int32_t)MEM32(tm + 28),
           (int32_t)MEM32(tm + 32), (int32_t)MEM32(tm + 36), (int32_t)MEM32(tm + 40), b[0], b[1], b[2], b[3], b[4],
           b[5], b[6], b[7], b[8]);
}

/* ---- font_create ---- */

/* CoreText is not safe in a fork child of a process with Metal's threads, so the checks that open a face
 * after the stop run as processes of their own from tests/test_ui.py: font_log (with and without
 * BW2_FONT_LOG=1) and font_noface. */
static uint32_t g_fdev, g_font;
static void c_font_width(void) { create_font(g_fdev, TEST_FACE, -16, 5, 1, 1); }
static void c_font_mips(void) { create_font(g_fdev, TEST_FACE, -16, 0, 2, 1); }
static void c_font_charset(void) { create_font(g_fdev, TEST_FACE, -16, 0, 1, 128); }

static void group_font_log(void) {
    add_ok(g_arg[0]);
    uint32_t dev = new_device_sized(FW, FH);
    release(new_font(dev, TEST_FACE, -16));
    release(new_font(dev, "Courier New", -16));
}
static void group_font_noface(void) {                   /* stops */
    add_ok(g_arg[0]);
    create_font(new_device_sized(FW, FH), "No Such Face", -16, 0, 1, 1);
}

static void group_font_create(void) {
    add_ok(g_arg[0]);
    uint32_t dev = g_fdev = new_device_sized(FW, FH), out = gmem(4);
    CHECKEQ(call("D3DXCreateFontA", 12, 0u, (uint32_t)-16, 0u, 400u, 1u, 0u, 1u, 7u, 4u, 0u, gbytes("x", 1), out),
            D3DERR_INVALIDCALL);
    /* the game's call */
    uint32_t f = new_font(dev, TEST_FACE, -16);
    if (!f) return;
    uint32_t r0 = refs(dev);
    CHECKEQ(FN(f, "GetDevice", 1, out), D3D_OK);
    CHECKEQ(MEM32(out), dev);
    CHECKEQ(refs(dev), r0 + 1);
    release(dev);
    uint32_t d = gmem(96);
    memset(hle_ptr(d), 0xCC, 96);
    CHECKEQ(FN(f, "GetDescA", 1, d), D3D_OK);
    const uint8_t *b = hle_ptr(d);
    CHECKEQ(MEM32(d), (uint32_t)-16), CHECKEQ(MEM32(d + 4), 0), CHECKEQ(MEM32(d + 8), 400);
    CHECKEQ(MEM32(d + 12), 1), CHECKEQ(MEM32(d + 16), 0);
    CHECKEQ(b[20], 1), CHECKEQ(b[21], 7), CHECKEQ(b[22], 4), CHECKEQ(b[23], 0);
    CHECK(!memcmp(b + 24, TEST_FACE, sizeof TEST_FACE));
    for (int i = 24 + (int)sizeof TEST_FACE; i < 56; i++) CHECKEQ(b[i], 0);
    CHECKEQ(b[56], 0xCC);                                              /* 56 bytes, no more */
    memset(hle_ptr(d), 0xCC, 96);
    CHECKEQ(FN(f, "GetDescW", 1, d), D3D_OK);
    CHECKEQ(MEM32(d), (uint32_t)-16), CHECKEQ(MEM32(d + 8), 400), CHECKEQ(b[20], 1), CHECKEQ(b[22], 4);
    for (int i = 0; i < 32; i++)
        CHECKEQ(b[24 + 2 * i] | b[25 + 2 * i] << 8, i < (int)sizeof TEST_FACE - 1 ? (uint8_t)TEST_FACE[i] : 0);
    CHECKEQ(b[88], 0xCC);
    CHECKEQ(FN(f, "GetDescA", 1, 0u), D3DERR_INVALIDCALL);
    /* TEXTMETRICA (56 bytes), and TEXTMETRICW (60) with the same numbers */
    uint32_t ta = gmem(64), tw = gmem(64);
    memset(hle_ptr(ta), 0xCC, 64), memset(hle_ptr(tw), 0xCC, 64);
    CHECKEQ(FN(f, "GetTextMetricsA", 1, ta), 1);
    CHECKEQ(FN(f, "GetTextMetricsW", 1, tw), 1);
    CHECKEQ(FN(f, "GetTextMetricsA", 1, 0u), 0);
    print_tma("a", ta);
    const uint8_t *A = hle_ptr(ta), *W = hle_ptr(tw);
    CHECK(!memcmp(A, W, 44));
    for (int i = 0; i < 4; i++) CHECKEQ(W[44 + 2 * i] | W[45 + 2 * i] << 8, A[44 + i]);
    CHECK(!memcmp(W + 52, A + 48, 5));
    CHECKEQ(A[56], 0xCC), CHECKEQ(W[60], 0xCC);
    struct bw2_font *ref = OPEN(TEST_FACE, -16, 400, 0);
    if (ref) CHECKEQ(MEM32(ta), bw2_font_metrics(ref)->height), bw2_font_close(ref);
    /* the arguments this layer does not handle stop */
    expect_stop(c_font_width, "D3DXCreateFontA: Width 5", "Width 5");
    expect_stop(c_font_mips, "D3DXCreateFontA: MipLevels 2", "MipLevels 2");
    expect_stop(c_font_charset, "D3DXCreateFontA: CharSet 128", "CharSet 128");
    release(f);
}

/* ---- font_binding: a face binds at D3DXCreateFontA. argv: Arial Narrow renamed "Courier New" ---- */

static void group_font_binding(void) {
    uint32_t dev = new_device_sized(FW, FH), rt = wide_target(dev), ta = gmem(64), tb = gmem(64);
    uint32_t before = new_font(dev, "Courier New", -16);
    CHECKEQ(FN(before, "GetTextMetricsA", 1, ta), 1);
    print_tma("system", ta);
    uint32_t h = add_ok(g_arg[0]);
    CHECKEQ(FN(before, "GetTextMetricsA", 1, tb), 1);
    CHECK(!memcmp(hle_ptr(ta), hle_ptr(tb), 56));                      /* still the system face */
    uint32_t after = new_font(dev, "Courier New", -16);
    CHECKEQ(FN(after, "GetTextMetricsA", 1, tb), 1);
    print_tma("memory", tb);
    CHECK(memcmp(hle_ptr(ta), hle_ptr(tb), 56) != 0);
    struct bw2_font *ref = OPEN("Courier New", -16, 400, 0);           /* the memory face, while it is there */
    CHECK(is_memory(ref));
    CHECKEQ(gdi_call("RemoveFontMemResourceEx", 1, h), 1);
    if (!ref) return;
    /* removed: the font made from it still draws its glyphs (none cached before) */
    struct d3dx_glyph_pos want[8];
    uint32_t n = expect_line(ref, "Hi", 4, 4 + bw2_font_metrics(ref)->ascent, want);
    clear(dev, BLACK);
    CHECKEQ(text_a(after, 0, "Hi", -1, rect(4, 4, 200, 40), DT_NOCLIP, WHITE), bw2_font_metrics(ref)->height);
    read_wide(dev, rt);
    expect_text(ref, want, n, WHITE, NULL, "drawn after RemoveFontMemResourceEx");
    int lit = 0;
    for (uint32_t i = 0; i < FW * FH; i++) lit += (g_fpx[i] & 0xFFFFFF) != 0;
    CHECK(lit > 20);
    bw2_font_close(ref);
    release(before), release(after), release(rt);
}

/* ---- font_measure: the game's DT_CALCRECT call ---- */

static void group_font_measure(void) {
    add_ok(g_arg[0]);
    struct bw2_font *ref = OPEN(TEST_FACE, -16, 400, 0);
    if (!ref) return;
    const struct bw2_tm *tm = bw2_font_metrics(ref);
    int32_t h = tm->height, adv = bw2_font_advance(ref, bw2_font_glyph(ref, 'X'));
    uint32_t dev = new_device_sized(FW, FH), rt = wide_target(dev), f = new_font(dev, TEST_FACE, -16);
    clear(dev, BLACK);
    uint32_t live0 = d3d_live_count(), n0 = d3dx_sprite_draw_calls();
    static const uint32_t fmts[2] = { 0x524u, 0x528u };                /* DT_VCENTER, then DT_BOTTOM */
    struct d3dx_glyph_pos want[2];
    for (int k = 0; k < 2; k++) {
        uint32_t r = rect(0, 0, 0, 0);
        CHECKEQ(text_a(f, 0, "X", 1, r, fmts[k], 0u), h);
        CHECKEQ(rc_at(r, 0), 0), CHECKEQ(rc_at(r, 1), 0), CHECKEQ(rc_at(r, 2), adv), CHECKEQ(rc_at(r, 3), h);
        expect_layout(want, expect_line(ref, "X", 0, tm->ascent, want), "measured X");
        r = rect(10, 20, 10, 20);
        CHECKEQ(text_a(f, 0, "X", 1, r, fmts[k], 0u), h);
        CHECKEQ(rc_at(r, 0), 10), CHECKEQ(rc_at(r, 1), 20), CHECKEQ(rc_at(r, 2), 10 + adv), CHECKEQ(rc_at(r, 3), 20 + h);
    }
    /* two lines, the widest one's width */
    uint32_t r = rect(0, 0, 0, 0);
    CHECKEQ(text_a(f, 0, "ab\nabc", -1, r, DT_CALCRECT, 0u), 2 * h);
    CHECKEQ(rc_at(r, 2), width_of(ref, "abc")), CHECKEQ(rc_at(r, 3), 2 * h);
    /* count 0: 0, the rect as it was */
    r = rect(1, 2, 3, 4);
    CHECKEQ(text_a(f, 0, "X", 0, r, DT_CALCRECT, 0u), 0);
    CHECKEQ(rc_at(r, 2), 3), CHECKEQ(rc_at(r, 3), 4);
    CHECKEQ(layout(), 0);
    /* nothing drawn, no texture made */
    CHECKEQ(d3d_live_count(), live0);
    CHECKEQ(d3dx_sprite_draw_calls(), n0);
    read_wide(dev, rt);
    int changed = 0;
    for (uint32_t i = 0; i < FW * FH; i++) changed += g_fpx[i] != BLACK;
    CHECKEQ(changed, 0);
    bw2_font_close(ref);
    release(f), release(rt);
}

/* ---- font_draw ---- */

static void group_font_draw(void) {
    add_ok(g_arg[0]);
    struct bw2_font *ref = OPEN(TEST_FACE, -16, 400, 0);
    if (!ref) return;
    const struct bw2_tm *tm = bw2_font_metrics(ref);
    uint32_t dev = new_device_sized(FW, FH), rt = wide_target(dev), f = new_font(dev, TEST_FACE, -16);
    struct d3dx_glyph_pos want[8];
    uint32_t n = expect_line(ref, "Hi", 4, 4 + tm->ascent, want);
    static const uint32_t colors[2] = { WHITE, 0xFF00FF00u };
    for (int k = 0; k < 2; k++) {
        clear(dev, BLACK);
        CHECKEQ(text_a(f, 0, "Hi", -1, rect(4, 4, 200, 40), DT_NOCLIP, colors[k]), tm->height);
        expect_layout(want, n, "Hi");
        read_wide(dev, rt);
        expect_text(ref, want, n, colors[k], NULL, k ? "green Hi" : "white Hi");
    }
    int green = 0;                                                     /* the last one drew green */
    for (uint32_t i = 0; i < FW * FH; i++) green += (g_fpx[i] & 0xFFFFFF) == 0x00FF00u;
    CHECK(green > 4);
    bw2_font_close(ref);
    release(f), release(rt);
}

/* ---- font_align ---- */

static void group_font_align(void) {
    add_ok(g_arg[0]);
    struct bw2_font *ref = OPEN(TEST_FACE, -16, 400, 0);
    if (!ref) return;
    const struct bw2_tm *tm = bw2_font_metrics(ref);
    int32_t h = tm->height, W = width_of(ref, "Hi");
    uint32_t dev = new_device_sized(FW, FH), rt = wide_target(dev), f = new_font(dev, TEST_FACE, -16);
    struct d3dx_glyph_pos want[8];
    static const struct { uint32_t fmt; const char *what; } c[] = {
        { DT_CENTER, "DT_CENTER" }, { DT_RIGHT, "DT_RIGHT" }, { DT_VCENTER | DT_SINGLELINE, "DT_VCENTER" },
        { DT_BOTTOM | DT_SINGLELINE, "DT_BOTTOM" }, { DT_VCENTER, "DT_VCENTER without DT_SINGLELINE" },
        { DT_CENTER | DT_BOTTOM | DT_SINGLELINE, "DT_CENTER | DT_BOTTOM" },
    };
    for (size_t k = 0; k < sizeof c / sizeof *c; k++) {
        uint32_t fmt = c[k].fmt;
        int32_t x = 0, top = 0;
        if (fmt & DT_CENTER) x = ((int32_t)FW - W) / 2;                /* positive, so the division floors */
        if (fmt & DT_RIGHT) x = (int32_t)FW - W;
        if ((fmt & DT_SINGLELINE) && (fmt & DT_VCENTER)) top = ((int32_t)FH - h) / 2;
        if ((fmt & DT_SINGLELINE) && (fmt & DT_BOTTOM)) top = (int32_t)FH - h;
        int32_t ret = (fmt & DT_SINGLELINE) && (fmt & (DT_VCENTER | DT_BOTTOM)) ? top + h : h;
        uint32_t n = expect_line(ref, "Hi", x, top + tm->ascent, want);
        clear(dev, BLACK);
        CHECKEQ(text_a(f, 0, "Hi", -1, rect(0, 0, (int32_t)FW, (int32_t)FH), fmt, WHITE), ret);
        expect_layout(want, n, c[k].what);
        read_wide(dev, rt);
        expect_text(ref, want, n, WHITE, NULL, c[k].what);
    }
    CHECKEQ(text_a(f, 0, "Hi", -1, rect(0, 0, (int32_t)FW, (int32_t)FH), DT_BOTTOM | DT_SINGLELINE, WHITE), (int32_t)FH);
    /* a rect narrower than the line: floor((width - line width) / 2) rounds down, from the rect's left */
    uint32_t n = expect_line(ref, "Hi", 10 - 2, tm->ascent, want);
    text_a(f, 0, "Hi", -1, rect(10, 0, 10 + W - 3, (int32_t)FH), DT_CENTER | DT_NOCLIP, WHITE);
    expect_layout(want, n, "DT_CENTER, 3 pixels too narrow");
    n = expect_line(ref, "Hi", 10 + W - 3 - W, tm->ascent, want);
    text_a(f, 0, "Hi", -1, rect(10, 0, 10 + W - 3, (int32_t)FH), DT_RIGHT | DT_NOCLIP, WHITE);
    expect_layout(want, n, "DT_RIGHT, 3 pixels too narrow");
    bw2_font_close(ref);
    release(f), release(rt);
}

/* ---- font_wrap ---- */

/* the layout of lines of text, each at the left, one tm height apart from top */
static uint32_t expect_lines(const struct bw2_font *ref, const char *const *ls, int nl, int32_t x, int32_t top,
                             struct d3dx_glyph_pos *out) {
    uint32_t n = 0;
    for (int i = 0; i < nl; i++)
        n += expect_line(ref, ls[i], x, top + i * bw2_font_metrics(ref)->height + bw2_font_metrics(ref)->ascent, out + n);
    return n;
}
static void group_font_wrap(void) {
    add_ok(g_arg[0]);
    struct bw2_font *ref = OPEN(TEST_FACE, -16, 400, 0);
    if (!ref) return;
    const struct bw2_tm *tm = bw2_font_metrics(ref);
    int32_t h = tm->height, T = 8 * tm->ave_char_width;
    uint32_t dev = new_device_sized(FW, FH), rt = wide_target(dev), f = new_font(dev, TEST_FACE, -16);
    struct d3dx_glyph_pos want[64];
    (void)rt;
    /* as wide as "aaa bbb": two lines, the space at the break gone */
    static const char *const two[] = { "aaa bbb", "ccc" }, *const three[] = { "aaa", "bbb", "ccc" };
    int32_t w2 = width_of(ref, "aaa bbb"), w3 = width_of(ref, "aaa");
    CHECKEQ(text_a(f, 0, "aaa bbb ccc", -1, rect(0, 0, w2, 64), DT_WORDBREAK, WHITE), 2 * h);
    expect_layout(want, expect_lines(ref, two, 2, 0, 0, want), "two lines");
    CHECKEQ(text_a(f, 0, "aaa bbb ccc", -1, rect(0, 0, w2 + 1, 64), DT_WORDBREAK, WHITE), 2 * h);
    /* narrower than "aaa": three lines, no word split */
    CHECKEQ(text_a(f, 0, "aaa bbb ccc", -1, rect(0, 0, w3 - 1, 64), DT_WORDBREAK, WHITE), 3 * h);
    expect_layout(want, expect_lines(ref, three, 3, 0, 0, want), "three lines");
    CHECKEQ(text_a(f, 0, "aaa bbb ccc", -1, rect(0, 0, 0, 0), DT_WORDBREAK | DT_NOCLIP, WHITE), 3 * h);
    /* a run of spaces at a break takes no width */
    CHECKEQ(text_a(f, 0, "aaa    bbb ccc", -1, rect(0, 0, w3 + 2, 64), DT_WORDBREAK, WHITE), 3 * h);
    expect_layout(want, expect_lines(ref, three, 3, 0, 0, want), "a run of spaces");
    /* without DT_WORDBREAK: one line */
    CHECKEQ(text_a(f, 0, "aaa bbb ccc", -1, rect(0, 0, w3, 64), 0, WHITE), h);
    /* line breaks */
    static const char *const ab[] = { "a", "b" };
    static const char *const brk[] = { "a\nb", "a\r\nb", "a\rb" };
    for (int k = 0; k < 3; k++) {
        CHECKEQ(text_a(f, 0, brk[k], -1, rect(0, 0, 200, 64), 0, WHITE), 2 * h);
        expect_layout(want, expect_lines(ref, ab, 2, 0, 0, want), brk[k]);
        CHECKEQ(text_a(f, 0, brk[k], -1, rect(0, 0, 200, 64), DT_SINGLELINE, WHITE), h);
        expect_layout(want, expect_line(ref, "ab", 0, tm->ascent, want), "DT_SINGLELINE: CR and LF draw nothing");
    }
    CHECKEQ(text_a(f, 0, "a\n", -1, rect(0, 0, 200, 64), 0, WHITE), h);
    CHECKEQ(text_a(f, 0, "a\n\nb", -1, rect(0, 0, 200, 64), 0, WHITE), 3 * h);
    /* tabs: DT_EXPANDTABS stops every 8 * ave_char_width from the rect's left; else the tab's glyph */
    uint32_t n = expect_line(ref, "a", 5, tm->ascent, want);
    n += expect_line(ref, "b", 5 + T, tm->ascent, want + n);
    CHECKEQ(text_a(f, 0, "a\tb", -1, rect(5, 0, 200, 64), DT_EXPANDTABS, WHITE), h);
    expect_layout(want, n, "a tab stop");
    n = expect_line(ref, "b", 5 + 2 * T, tm->ascent, want);
    CHECKEQ(text_a(f, 0, "\t\tb", -1, rect(5, 0, 200, 64), DT_EXPANDTABS | DT_NOCLIP, WHITE), h);
    expect_layout(want, n, "two tab stops");
    int32_t wa = width_of(ref, "aaaaaaaaaa");
    n = expect_line(ref, "aaaaaaaaaa", 0, tm->ascent, want);
    n += expect_line(ref, "b", (wa / T + 1) * T, tm->ascent, want + n);
    CHECKEQ(text_a(f, 0, "aaaaaaaaaa\tb", -1, rect(0, 0, 200, 64), DT_EXPANDTABS | DT_NOCLIP, WHITE), h);
    expect_layout(want, n, "a tab past the first stop");
    n = expect_line(ref, "a\tb", 0, tm->ascent, want);
    CHECKEQ(text_a(f, 0, "a\tb", -1, rect(0, 0, 200, 64), 0, WHITE), h);
    expect_layout(want, n, "a tab without DT_EXPANDTABS");
    bw2_font_close(ref);
    release(f), release(rt);
}

/* ---- font_calcrect_roundtrip ---- */

static void group_font_calcrect_roundtrip(void) {
    add_ok(g_arg[0]);
    struct bw2_font *ref = OPEN(TEST_FACE, -16, 400, 0);
    if (!ref) return;
    int32_t h = bw2_font_metrics(ref)->height;
    uint32_t dev = new_device_sized(FW, FH), rt = wide_target(dev), f = new_font(dev, TEST_FACE, -16);
    static const char *const s[5] = { "The quick brown fox jumps over the lazy dog", "aaa bbb ccc ddd eee",
                                      "one  two   three four", "Hello, World! Wrap me please, at the edge.",
                                      "x yy zzz wwww vvvvv u" };
    static const int32_t extra[5] = { 0, 3, 9, 14, 21 };
    for (int k = 0; k < 5; k++) {
        int32_t word = 0;                                              /* the widest word: none is split */
        char buf[128];
        snprintf(buf, sizeof buf, "%s", s[k]);
        for (char *t = strtok(buf, " "); t; t = strtok(NULL, " ")) word = width_of(ref, t) > word ? width_of(ref, t) : word;
        int32_t W = word + extra[k];
        uint32_t r = rect(5, 3, 5 + W, 3);
        int32_t ret = text_a(f, 0, s[k], -1, r, DT_CALCRECT | DT_WORDBREAK, WHITE);
        CHECK(ret >= 2 * h && ret % h == 0);
        CHECKEQ(rc_at(r, 0), 5), CHECKEQ(rc_at(r, 1), 3), CHECKEQ(rc_at(r, 3), 3 + ret);
        CHECK(rc_at(r, 2) <= 5 + W && rc_at(r, 2) >= 5 + word);
        struct d3dx_glyph_pos calc[128], a[128];
        uint32_t nc = d3dx_font_last_layout(calc, 128);
        CHECKEQ(text_a(f, 0, s[k], -1, r, DT_WORDBREAK, WHITE), ret);  /* into the rect it gave */
        uint32_t na = d3dx_font_last_layout(a, 128);
        CHECKEQ(text_a(f, 0, s[k], -1, rect(5, 3, 5 + W, 3), DT_WORDBREAK | DT_NOCLIP, WHITE), ret);
        char what[64];
        snprintf(what, sizeof what, "roundtrip %d (%d lines)", k, ret / h);
        CHECKEQ(na, nc);
        expect_layout(a, na, what);
        expect_layout(calc, nc, what);
        /* the widest line is the rect's width: the pen after some glyph ends there */
        int32_t right = 0;
        for (uint32_t i = 0; i < na; i++) {
            struct bw2_glyph g;
            bw2_font_raster(ref, a[i].glyph, &g);
            int32_t end = a[i].x - g.left + bw2_font_advance(ref, a[i].glyph);
            free(g.alpha);
            if (end > right) right = end;
        }
        CHECKEQ(right, rc_at(r, 2));
    }
    bw2_font_close(ref);
    release(f), release(rt);
}

/* ---- font_clip ---- */

static void group_font_clip(void) {
    add_ok(g_arg[0]);
    struct bw2_font *ref = OPEN(TEST_FACE, -16, 400, 0);
    if (!ref) return;
    const struct bw2_tm *tm = bw2_font_metrics(ref);
    uint32_t dev = new_device_sized(FW, FH), rt = wide_target(dev), f = new_font(dev, TEST_FACE, -16);
    struct d3dx_glyph_pos want[16];
    const char *s = "WWWWWWWWWW";
    int32_t W = width_of(ref, s), h = tm->height;
    CHECK(W > 30);
    static const int32_t box[4] = { 10, 10, 40, 30 };
    uint32_t n = expect_line(ref, s, 10, 10 + tm->ascent, want);
    clear(dev, BLACK);
    text_a(f, 0, s, -1, rect(box[0], box[1], box[2], box[3]), 0, WHITE);
    read_wide(dev, rt);
    expect_text(ref, want, n, WHITE, box, "clipped right");
    clear(dev, BLACK);
    text_a(f, 0, s, -1, rect(box[0], box[1], box[2], box[3]), DT_NOCLIP, WHITE);
    read_wide(dev, rt);
    expect_text(ref, want, n, WHITE, NULL, "DT_NOCLIP");
    int past = 0;
    for (uint32_t y = 0; y < FH; y++)
        for (uint32_t x = 40; x < FW; x++) past += (g_fpx[y * FW + x] & 0xFFFFFF) != 0;
    CHECK(past > 0);
    /* cut on the left and at the top: DT_RIGHT and DT_BOTTOM in a short rect */
    static const int32_t small[4] = { 10, 10, 40, 18 };
    n = expect_line(ref, s, 40 - W, 18 - h + tm->ascent, want);
    clear(dev, BLACK);
    text_a(f, 0, s, -1, rect(small[0], small[1], small[2], small[3]), DT_RIGHT | DT_BOTTOM | DT_SINGLELINE, WHITE);
    read_wide(dev, rt);
    expect_text(ref, want, n, WHITE, small, "clipped left and top");
    bw2_font_close(ref);
    release(f), release(rt);
}

/* ---- font_sprite: the game's own sprite ---- */

static void group_font_sprite(void) {
    add_ok(g_arg[0]);
    struct bw2_font *ref = OPEN(TEST_FACE, -16, 400, 0);
    if (!ref) return;
    const struct bw2_tm *tm = bw2_font_metrics(ref);
    uint32_t dev = new_device_sized(FW, FH), rt = wide_target(dev), f = new_font(dev, TEST_FACE, -16);
    uint32_t s = new_sprite(dev);
    struct d3dx_glyph_pos want[8];
    uint32_t n = expect_line(ref, "Hi", 4, 4 + tm->ascent, want);
    /* 1:1 inside the game's Begin/End */
    clear(dev, BLACK);
    CHECKEQ(SP(s, "Begin", 1, D3DXSPRITE_ALPHABLEND), D3D_OK);
    uint32_t n0 = d3dx_sprite_draw_calls();
    CHECKEQ(text_a(f, s, "Hi", -1, rect(4, 4, 100, 30), DT_NOCLIP, WHITE), tm->height);
    CHECKEQ(d3dx_sprite_draw_calls(), n0);                            /* queued in the game's sprite */
    CHECKEQ(SP(s, "End", 0), D3D_OK);
    CHECK(d3dx_sprite_draw_calls() > n0);
    read_wide(dev, rt);
    expect_text(ref, want, n, WHITE, NULL, "in the game's sprite");
    /* scale 2: the text at twice the size */
    int32_t bx0 = 1 << 30, by0 = 1 << 30, bx1 = -(1 << 30), by1 = -(1 << 30);
    for (uint32_t k = 0; k < n; k++) {
        struct bw2_glyph g;
        bw2_font_raster(ref, want[k].glyph, &g);
        if (g.w) {
            bx0 = want[k].x < bx0 ? want[k].x : bx0, by0 = want[k].y < by0 ? want[k].y : by0;
            int32_t r = want[k].x + (int32_t)g.w, b = want[k].y + (int32_t)g.h;
            bx1 = r > bx1 ? r : bx1, by1 = b > by1 ? b : by1;
        }
        free(g.alpha);
    }
    const float m2[16] = { 2, 0, 0, 0, 0, 2, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 };
    CHECKEQ(SP(s, "SetTransform", 1, matrix(m2)), D3D_OK);
    clear(dev, BLACK);
    CHECKEQ(SP(s, "Begin", 1, D3DXSPRITE_ALPHABLEND), D3D_OK);
    n0 = d3dx_sprite_draw_calls();
    CHECKEQ(text_a(f, s, "Hi", -1, rect(4, 4, 100, 30), DT_NOCLIP, WHITE), tm->height);
    CHECKEQ(d3dx_sprite_draw_calls(), n0);
    CHECKEQ(SP(s, "End", 0), D3D_OK);
    CHECK(d3dx_sprite_draw_calls() > n0);
    read_wide(dev, rt);
    int32_t lx0 = 1 << 30, ly0 = 1 << 30, lx1 = -1, ly1 = -1;
    for (int32_t y = 0; y < (int32_t)FH; y++)
        for (int32_t x = 0; x < (int32_t)FW; x++)
            if ((g_fpx[y * (int32_t)FW + x] & 0xFF) > 0x40) {
                lx0 = x < lx0 ? x : lx0, ly0 = y < ly0 ? y : ly0, lx1 = x + 1 > lx1 ? x + 1 : lx1, ly1 = y + 1 > ly1 ? y + 1 : ly1;
            }
    printf("scaled box (%d,%d)-(%d,%d), 1:1 box (%d,%d)-(%d,%d)\n", lx0, ly0, lx1, ly1, bx0, by0, bx1, by1);
    CHECK(abs(lx0 - 2 * bx0) <= 2 && abs(ly0 - 2 * by0) <= 2 && abs(lx1 - 2 * bx1) <= 2 && abs(ly1 - 2 * by1) <= 2);
    bw2_font_close(ref);
    release(s), release(f), release(rt);
}

/* ---- font_cp1252 (the table), and with argv[0] what DrawTextA and DrawTextW place ---- */

static void font_cp1252_draw(void) {
    add_ok(g_arg[0]);
    struct bw2_font *ref = OPEN(TEST_FACE, -16, 400, 0);
    if (!ref) return;
    const struct bw2_tm *tm = bw2_font_metrics(ref);
    uint32_t dev = new_device_sized(FW, FH), rt = wide_target(dev), f = new_font(dev, TEST_FACE, -16);
    struct d3dx_glyph_pos want[8];
    uint32_t q = bw2_font_glyph(ref, 0x2019), e = bw2_font_glyph(ref, 0xE9);
    CHECK(q != 0 && e != 0 && q != e);
    uint32_t n = expect_line(ref, "\x92\xE9", 4, 4 + tm->ascent, want);
    CHECKEQ(want[0].glyph, q), CHECKEQ(want[1].glyph, e);
    clear(dev, BLACK);
    CHECKEQ(text_a(f, 0, "\x92\xE9", -1, rect(4, 4, 200, 40), DT_NOCLIP, WHITE), tm->height);
    expect_layout(want, n, "0x92 0xE9");
    read_wide(dev, rt);
    expect_text(ref, want, n, WHITE, NULL, "0x92 0xE9");
    /* the same through DrawTextW */
    static const uint16_t qe[] = { 0x2019, 0xE9, 0 };
    clear(dev, BLACK);
    CHECKEQ(text_w(f, 0, qe, -1, rect(4, 4, 200, 40), DT_NOCLIP, WHITE), tm->height);
    expect_layout(want, n, "U+2019 U+00E9");
    /* a code point the face lacks: glyph 0, with glyph 0's advance */
    CHECKEQ(bw2_font_glyph(ref, 0x4E00), 0);
    struct bw2_glyph g0;
    bw2_font_raster(ref, 0, &g0);
    CHECK(g0.w > 0 && g0.h > 0);
    want[0] = (struct d3dx_glyph_pos){ 0, 4 + g0.left, 4 + tm->ascent - g0.top };
    free(g0.alpha);
    n = 1 + expect_line(ref, "X", 4 + bw2_font_advance(ref, 0), 4 + tm->ascent, want + 1);
    static const uint16_t cjk[] = { 0x4E00, 'X', 0 };
    clear(dev, BLACK);
    CHECKEQ(text_w(f, 0, cjk, 2, rect(4, 4, 200, 40), DT_NOCLIP, WHITE), tm->height);
    expect_layout(want, n, "U+4E00 X");
    read_wide(dev, rt);
    expect_text(ref, want, n, WHITE, NULL, "U+4E00 X");
    bw2_font_close(ref);
    release(f), release(rt);
}

/* ---- font_cache_spill ---- */

static void group_font_cache_spill(void) {
    add_ok(g_arg[0]);
    struct bw2_font *ref = OPEN(TEST_FACE, -46, 400, 0);
    if (!ref) return;
    const struct bw2_tm *tm = bw2_font_metrics(ref);
    uint32_t dev = new_device_sized(FW, FH), rt = wide_target(dev), f = new_font(dev, TEST_FACE, -46);
    uint32_t live0 = d3d_live_count();
    CHECKEQ(FN(f, "PreloadCharacters", 2, 0x20u, 0xFFu), D3D_OK);
    uint32_t ntex = d3d_live_count() - live0;
    printf("cache textures %u\n", ntex);
    CHECK(ntex >= 2);
    int bad = 0;
    uint32_t live1 = 0;                       /* after the first draw: its sprite's state block is made then */
    for (uint16_t cp = 0x20; cp <= 0xFF; cp++) {
        uint32_t g = bw2_font_glyph(ref, cp);
        struct bw2_glyph b;
        bw2_font_raster(ref, g, &b);
        free(b.alpha);
        struct d3dx_glyph_pos want = { g, 8 + b.left, tm->ascent - b.top };
        uint16_t s[2] = { cp, 0 };
        clear(dev, BLACK);
        CHECKEQ(text_w(f, 0, s, 1, rect(8, 0, (int32_t)FW, (int32_t)FH), DT_NOCLIP, WHITE), tm->height);
        expect_layout(&want, 1, "spill layout");
        read_wide(dev, rt);
        char what[32];
        snprintf(what, sizeof what, "U+%04X", cp);
        bad += !expect_text(ref, &want, 1, WHITE, NULL, what);
        if (cp == 0x20) live1 = d3d_live_count(), CHECKEQ(live1, live0 + ntex + 1);
    }
    CHECKEQ(bad, 0);
    CHECKEQ(d3d_live_count(), live1);                                  /* every glyph was preloaded */
    /* PreloadTextA and PreloadTextW of glyphs already there make nothing */
    CHECKEQ(FN(f, "PreloadTextA", 2, gbytes("Hello", 5), (uint32_t)-1), D3D_OK);
    static const uint16_t hw[] = { 'W', 0xE9, 0 };
    CHECKEQ(FN(f, "PreloadTextW", 2, gbytes(hw, 6), 2u), D3D_OK);
    CHECKEQ(d3d_live_count(), live1);
    bw2_font_close(ref);
    release(f), release(rt);
}

/* ---- font_reset ---- */

static void group_font_reset(void) {
    add_ok(g_arg[0]);
    struct bw2_font *ref = OPEN(TEST_FACE, -16, 400, 0);
    if (!ref) return;
    const struct bw2_tm *tm = bw2_font_metrics(ref);
    uint32_t dev = new_device_sized(FW, FH), rt = wide_target(dev), f = new_font(dev, TEST_FACE, -16);
    uint32_t s = new_sprite(dev), t = solid(dev, 0xFF00FF00u);
    struct d3dx_glyph_pos want[8];
    uint32_t n = expect_line(ref, "Hi", 4, 4 + tm->ascent, want);
    for (int round = 0; round < 2; round++) {
        clear(dev, BLACK);
        CHECKEQ(text_a(f, 0, "Hi", -1, rect(4, 4, 200, 40), DT_NOCLIP, WHITE), tm->height);
        read_wide(dev, rt);
        expect_text(ref, want, n, WHITE, NULL, round ? "after Reset" : "before Reset");
        CHECKEQ(SP(s, "Begin", 1, 0u), D3D_OK);
        CHECKEQ(SP(s, "Draw", 5, t, 0u, 0u, vec3(100, 30, 0), 0xFFFFFFFFu), D3D_OK);
        CHECKEQ(SP(s, "End", 0), D3D_OK);
        read_wide(dev, rt);
        CHECKEQ(g_fpx[31 * FW + 101] & 0xFFFFFF, 0x00FF00u);
        if (round) break;
        CHECKEQ(FN(f, "OnLostDevice", 0), D3D_OK);
        CHECKEQ(SP(s, "OnLostDevice", 0), D3D_OK);
        release(rt);
        CHECKEQ(DV(dev, "Reset", 1, g_pp), D3D_OK);
        CHECKEQ(FN(f, "OnResetDevice", 0), D3D_OK);
        CHECKEQ(SP(s, "OnResetDevice", 0), D3D_OK);
        rt = wide_target(dev);
    }
    bw2_font_close(ref);
    release(s), release(t), release(f), release(rt);
}

/* ---- font_loud ---- */

static void c_getdc(void) { FN(g_font, "GetDC", 0); }
static void c_glyphdata(void) { FN(g_font, "GetGlyphData", 4, 0u, gmem(4), gmem(16), gmem(8)); }
static void c_preloadglyphs(void) { FN(g_font, "PreloadGlyphs", 2, 0u, 10u); }
static void c_rtl(void) { text_a(g_font, 0, "Hi", -1, rect(0, 0, 100, 40), 0x20000u, WHITE); }
static void c_tabstop(void) { text_a(g_font, 0, "Hi", -1, rect(0, 0, 100, 40), 0x80u | DT_EXPANDTABS, WHITE); }
static void c_high_bit(void) { text_a(g_font, 0, "Hi", -1, rect(0, 0, 100, 40), 0x80000000u, WHITE); }
static void c_center_right(void) { text_a(g_font, 0, "Hi", -1, rect(0, 0, 100, 40), DT_CENTER | DT_RIGHT, WHITE); }
static void c_count(void) { text_a(g_font, 0, "Hi", -2, rect(0, 0, 100, 40), 0, WHITE); }
static void c_no_text(void) { FN(g_font, "DrawTextA", 6, 0u, 0u, 2u, rect(0, 0, 100, 40), 0u, WHITE); }
static uint32_t g_idle_sprite;
static void c_sprite_not_begun(void) { text_a(g_font, g_idle_sprite, "Hi", -1, rect(0, 0, 100, 40), DT_NOCLIP, WHITE); }

static void group_font_loud(void) {
    add_ok(g_arg[0]);
    g_fdev = new_device_sized(FW, FH);
    wide_target(g_fdev);
    g_font = new_font(g_fdev, TEST_FACE, -16);
    expect_stop(c_getdc, "ID3DXFont::GetDC not implemented", "GetDC");
    expect_stop(c_glyphdata, "ID3DXFont::GetGlyphData not implemented", "GetGlyphData");
    expect_stop(c_preloadglyphs, "ID3DXFont::PreloadGlyphs not implemented", "PreloadGlyphs");
    expect_stop(c_rtl, "DT_RTLREADING (0x20000)", "DT_RTLREADING");
    expect_stop(c_tabstop, "DT_TABSTOP (0x80)", "DT_TABSTOP");
    expect_stop(c_high_bit, "0x80000000", "an unnamed format bit");
    expect_stop(c_center_right, "DT_CENTER and DT_RIGHT", "DT_CENTER | DT_RIGHT");
    expect_stop(c_count, "count -2", "count -2");
    expect_stop(c_no_text, "no text", "NULL text");
    g_idle_sprite = new_sprite(g_fdev);                                /* the glyphs cached here, not in the child */
    CHECKEQ(FN(g_font, "PreloadTextA", 2, gbytes("Hi", 2), 2u), D3D_OK);
    expect_stop(c_sprite_not_begun, "ID3DXSprite::Draw gave 0x8876086C", "a sprite outside Begin/End");
}

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: ui_selftest <group>\n"); return 2; }
    g_arg = argv + 2;
    hle_mem_init();
    bw2_register_shims();
    mach_enter();
    g_esp = hle_va(hle_alloc(1u << 20)) + (1u << 20) - 64;
    if (!strcmp(argv[1], "d3d_call")) group_d3d_call();
    else if (!strcmp(argv[1], "sprite_basic")) group_sprite_basic();
    else if (!strcmp(argv[1], "sprite_texel_exact")) group_sprite_texel_exact();
    else if (!strcmp(argv[1], "sprite_args")) group_sprite_args();
    else if (!strcmp(argv[1], "sprite_sort")) group_sprite_sort();
    else if (!strcmp(argv[1], "sprite_state")) group_sprite_state();
    else if (!strcmp(argv[1], "sprite_objectspace")) group_sprite_objectspace();
    else if (!strcmp(argv[1], "sprite_rules")) group_sprite_rules();
    else if (!strcmp(argv[1], "sprite_lost")) group_sprite_lost();
    else if (!strcmp(argv[1], "font_metrics")) group_font_metrics();
    else if (!strcmp(argv[1], "font_advance")) group_font_advance();
    else if (!strcmp(argv[1], "font_memory")) group_font_memory();
    else if (!strcmp(argv[1], "font_errors")) group_font_errors();
    else if (!strcmp(argv[1], "font_fallthrough")) group_font_fallthrough();
    else if (!strcmp(argv[1], "font_raster")) group_font_raster();
    else if (!strcmp(argv[1], "font_cp1252")) group_font_cp1252();
    else if (!strcmp(argv[1], "font_create")) group_font_create();
    else if (!strcmp(argv[1], "font_log")) group_font_log();
    else if (!strcmp(argv[1], "font_noface")) group_font_noface();
    else if (!strcmp(argv[1], "font_binding")) group_font_binding();
    else if (!strcmp(argv[1], "font_measure")) group_font_measure();
    else if (!strcmp(argv[1], "font_draw")) group_font_draw();
    else if (!strcmp(argv[1], "font_align")) group_font_align();
    else if (!strcmp(argv[1], "font_wrap")) group_font_wrap();
    else if (!strcmp(argv[1], "font_calcrect_roundtrip")) group_font_calcrect_roundtrip();
    else if (!strcmp(argv[1], "font_clip")) group_font_clip();
    else if (!strcmp(argv[1], "font_sprite")) group_font_sprite();
    else if (!strcmp(argv[1], "font_cache_spill")) group_font_cache_spill();
    else if (!strcmp(argv[1], "font_reset")) group_font_reset();
    else if (!strcmp(argv[1], "font_loud")) group_font_loud();
    else { fprintf(stderr, "ui_selftest: no group %s\n", argv[1]); return 2; }
    printf("%s: %d failed\n", argv[1], fails);
    fflush(stdout);
    return fails != 0;
}
