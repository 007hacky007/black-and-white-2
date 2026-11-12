/*
 * ui_selftest.c - D3DX's 2D layer (plan 7): d3d_call and ID3DXSprite, called
 * as lifted code calls them, on a headless device. Built with the engine
 * (minus main.c) by tests/test_ui.py; `ui_selftest <group>` runs one group
 * and prints "<group>: <n> failed". Pixel groups draw into a 64x64 A8R8G8B8
 * target and read it back.
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
static uint32_t new_device(void) {
    uint32_t d3d = call("Direct3DCreate9", 1, 32u);
    uint32_t pp = gmem(56), out = gmem(4);
    MEM32(pp + 0) = 64, MEM32(pp + 4) = 64, MEM32(pp + 8) = 22, MEM32(pp + 12) = 1, MEM32(pp + 24) = 1;
    MEM32(pp + 28) = 0x10001u, MEM32(pp + 32) = 1, MEM32(pp + 36) = 1, MEM32(pp + 40) = 75;
    g_pp = pp;
    CHECKEQ(mcall(d3d, "IDirect3D9", "CreateDevice", 6, 0u, 1u, 0x10001u, 0x40u, pp, out), D3D_OK);
    return MEM32(out);
}
static uint32_t argb_target(uint32_t dev) {
    uint32_t out = gmem(4);
    CHECKEQ(DV(dev, "CreateRenderTarget", 8, 64u, 64u, 21u, 0u, 0u, 0u, out, 0u), D3D_OK);
    CHECKEQ(DV(dev, "SetRenderTarget", 2, 0u, MEM32(out)), D3D_OK);
    return MEM32(out);
}
static void clear(uint32_t dev, uint32_t c) { CHECKEQ(DV(dev, "Clear", 6, 0u, 0u, 7u, c, f2u(1.0f), 0u), D3D_OK); }
static uint32_t g_px[64 * 64];
static void read_rt(uint32_t dev, uint32_t rt) {
    uint32_t desc = gmem(32), out = gmem(4);
    CHECKEQ(mcall(rt, "IDirect3DSurface9", "GetDesc", 1, desc), D3D_OK);
    CHECKEQ(DV(dev, "CreateOffscreenPlainSurface", 6, 64u, 64u, MEM32(desc), 2u, out, 0u), D3D_OK);
    uint32_t s = MEM32(out), lr = gmem(8);
    CHECKEQ(DV(dev, "GetRenderTargetData", 2, rt, s), D3D_OK);
    CHECKEQ(mcall(s, "IDirect3DSurface9", "LockRect", 3, lr, 0u, 0x10u), D3D_OK);
    for (uint32_t y = 0; y < 64; y++) memcpy(g_px + y * 64, hle_ptr(MEM32(lr + 4) + y * MEM32(lr)), 64 * 4);
    CHECKEQ(mcall(s, "IDirect3DSurface9", "UnlockRect", 0), D3D_OK);
    release(s);
}
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

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: ui_selftest <group>\n"); return 2; }
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
    else { fprintf(stderr, "ui_selftest: no group %s\n", argv[1]); return 2; }
    printf("%s: %d failed\n", argv[1], fails);
    fflush(stdout);
    return fails != 0;
}
