/*
 * input_selftest.c - the DirectInput mouse driven as lifted code drives it:
 * arguments pushed right to left, a return address, then the shim; COM
 * methods through the guest vtable. Each check also verifies the stdcall pop.
 * Built with the engine (minus main.c) by tests/test_input.py;
 * `input_selftest <group> [arg]` runs one group and prints "<group>: <n> failed".
 */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "win32hle.h"
#include "bw2.h"
#include "d3d_com.h"

const recomp_dispatch_entry_t recomp_dispatch_table[1] = { { 0, 0 } };
const uint32_t recomp_dispatch_count = 0;

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)
#define CHECKEQ(a, b) do { uint64_t _a = (uint64_t)(a), _b = (uint64_t)(b); if (_a != _b) { \
    printf("FAIL %s:%d %s == %s: 0x%llX != 0x%llX\n", __FILE__, __LINE__, #a, #b, \
           (unsigned long long)_a, (unsigned long long)_b); fails++; } } while (0)

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
static const char *method_name(uint32_t obj, int slot) {
    recomp_lookup_import(MEM32(MEM32(obj) + 4u * (uint32_t)slot));
    return g_cur_import ? g_cur_import : "";
}

static uint32_t gmem(uint32_t n) { uint32_t p = hle_va(hle_alloc(n)); memset(hle_ptr(p), 0, n); return p; }

#define G(a, b, c, d0, d1, d2, d3, d4, d5, d6, d7) \
    { (a) & 0xFF, ((a) >> 8) & 0xFF, ((a) >> 16) & 0xFF, ((a) >> 24) & 0xFF, (b) & 0xFF, ((b) >> 8) & 0xFF, \
      (c) & 0xFF, ((c) >> 8) & 0xFF, d0, d1, d2, d3, d4, d5, d6, d7 }
static const uint8_t IID_IDirectInput8A[16] = G(0xBF798030, 0x483A, 0x4DA2, 0xAA, 0x99, 0x5D, 0x64, 0xED, 0x36, 0x97, 0x00);
static const uint8_t IID_IDirectInput8W[16] = G(0xBF798031, 0x483A, 0x4DA2, 0xAA, 0x99, 0x5D, 0x64, 0xED, 0x36, 0x97, 0x00);
static const uint8_t GUID_SysMouse[16] = G(0x6F1D2B60, 0xD5A0, 0x11CF, 0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00);
static const uint8_t GUID_SysKeyboard[16] = G(0x6F1D2B61, 0xD5A0, 0x11CF, 0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00);
static const uint8_t GUID_XAxis[16] = G(0xA36D02E0, 0xC9F3, 0x11CF, 0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00);
static const uint8_t GUID_YAxis[16] = G(0xA36D02E1, 0xC9F3, 0x11CF, 0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00);
static const uint8_t GUID_ZAxis[16] = G(0xA36D02E2, 0xC9F3, 0x11CF, 0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00);

static uint32_t gguid(const uint8_t *g) { uint32_t p = gmem(16); memcpy(hle_ptr(p), g, 16); return p; }

#define DI_OK 0u
#define DI_NOEFFECT 1u
#define DIERR_INVALIDPARAM 0x80070057u
#define DIERR_NOINTERFACE 0x80004002u
#define DIERR_OLDDIRECTINPUTVERSION 0x8007047Eu
#define DIERR_BETADIRECTINPUTVERSION 0x80070481u
#define CLASS_E_NOAGGREGATION 0x80040110u
#define DIERR_ACQUIRED 0x800700AAu
#define DIERR_NOTACQUIRED 0x8007000Cu
#define DIPROP_BUFFERSIZE 1u
#define HINST 0x00400000u
#define HWND 0x00010010u

/* IDirectInput8A and IDirectInputDevice8A slots */
enum { DI_CreateDevice = 3 };
enum { DD_GetCapabilities = 3, DD_EnumObjects, DD_GetProperty, DD_SetProperty, DD_Acquire, DD_Unacquire,
       DD_GetDeviceState, DD_GetDeviceData, DD_SetDataFormat, DD_SetEventNotification, DD_SetCooperativeLevel };

/* DirectInput8Create(hinst, 0x800, IID_IDirectInput8A, &di, NULL) */
static uint32_t new_di(void) {
    uint32_t out = gmem(4);
    CHECKEQ(call("DirectInput8Create", 5, HINST, 0x800u, gguid(IID_IDirectInput8A), out, 0u), DI_OK);
    return MEM32(out);
}
static uint32_t new_mouse(uint32_t di) {
    uint32_t out = gmem(4);
    CHECKEQ(vcall(di, DI_CreateDevice, 3, gguid(GUID_SysMouse), out, 0u), DI_OK);
    return MEM32(out);
}

/* The game's copy of c_dfDIMouse, built as plan 6's "Measured" describes it: 24, 16, 2, 16, 7 objects. */
static uint32_t mouse_format(uint32_t data_size) {
    uint32_t x = gguid(GUID_XAxis), y = gguid(GUID_YAxis), z = gguid(GUID_ZAxis), objs = gmem(7 * 16), f = gmem(24);
    static const uint32_t types[7] = { 0x00FFFF03u, 0x00FFFF03u, 0x80FFFF03u, 0x00FFFF0Cu, 0x00FFFF0Cu,
                                       0x80FFFF0Cu, 0x80FFFF0Cu };
    for (uint32_t i = 0; i < 7; i++) {
        uint32_t o = objs + 16 * i;
        MEM32(o) = i == 0 ? x : i == 1 ? y : i == 2 ? z : 0;
        MEM32(o + 4) = i < 3 ? 4 * i : 12 + (i - 3);
        MEM32(o + 8) = types[i];
        MEM32(o + 12) = 0;
    }
    MEM32(f) = 24, MEM32(f + 4) = 16, MEM32(f + 8) = 2, MEM32(f + 12) = data_size, MEM32(f + 16) = 7;
    MEM32(f + 20) = objs;
    return f;
}
static uint32_t bufsize_prop(uint32_t n) {
    uint32_t p = gmem(20);
    MEM32(p) = 20, MEM32(p + 4) = 16, MEM32(p + 8) = 0, MEM32(p + 12) = 0, MEM32(p + 16) = n;
    return p;
}

static void group_di_create(void) {
    uint32_t out = gmem(4), riid = gguid(IID_IDirectInput8A);
    MEM32(out) = 0xCCCCCCCCu;
    CHECKEQ(call("DirectInput8Create", 5, HINST, 0x700u, riid, out, 0u), DIERR_OLDDIRECTINPUTVERSION);
    CHECKEQ(MEM32(out), 0);
    CHECKEQ(call("DirectInput8Create", 5, HINST, 0x801u, riid, out, 0u), DIERR_BETADIRECTINPUTVERSION);
    CHECKEQ(call("DirectInput8Create", 5, HINST, 0x800u, gguid(IID_IDirectInput8W), out, 0u), DIERR_NOINTERFACE);
    CHECKEQ(MEM32(out), 0);
    CHECKEQ(call("DirectInput8Create", 5, HINST, 0x800u, 0u, out, 0u), DIERR_INVALIDPARAM);
    CHECKEQ(call("DirectInput8Create", 5, HINST, 0x800u, riid, 0u, 0u), DIERR_INVALIDPARAM);
    CHECKEQ(call("DirectInput8Create", 5, HINST, 0x800u, riid, out, 0x1000u), CLASS_E_NOAGGREGATION);
    CHECKEQ(MEM32(out), 0);
    uint32_t live = d3d_live_count();
    CHECKEQ(call("DirectInput8Create", 5, HINST, 0x800u, riid, out, 0u), DI_OK);
    uint32_t di = MEM32(out);
    CHECK(di != 0);
    CHECK(!strcmp(method_name(di, 3), "IDirectInput8A::CreateDevice"));
    CHECK(!strcmp(method_name(di, 10), "IDirectInput8A::ConfigureDevices"));
    /* QueryInterface for its own IID answers the same object */
    uint32_t q = gmem(4);
    CHECKEQ(vcall(di, 0, 2, riid, q), DI_OK);
    CHECKEQ(MEM32(q), di);
    CHECKEQ(vcall(di, 2, 0), 1);
    CHECKEQ(vcall(di, 2, 0), 0);
    CHECKEQ(d3d_live_count(), live);
}

static void group_di_game_setup(void) {
    uint32_t live = d3d_live_count();
    uint32_t di = new_di(), dev = new_mouse(di);
    CHECK(dev != 0);
    CHECK(!strcmp(method_name(dev, 11), "IDirectInputDevice8A::SetDataFormat"));
    CHECK(!strcmp(method_name(dev, 31), "IDirectInputDevice8A::GetImageInfo"));
    /* the measured setup (0x9AF220) */
    CHECKEQ(vcall(dev, DD_SetDataFormat, 1, mouse_format(16)), DI_OK);
    CHECKEQ(vcall(dev, DD_SetCooperativeLevel, 2, HWND, 6u), DI_OK);
    CHECKEQ(vcall(dev, DD_SetProperty, 2, DIPROP_BUFFERSIZE, bufsize_prop(512)), DI_OK);
    CHECKEQ(vcall(dev, DD_Acquire, 0), DI_OK);
    /* the mode switch (0x9AF790) */
    CHECKEQ(vcall(dev, DD_Unacquire, 0), DI_OK);
    CHECKEQ(vcall(dev, DD_SetCooperativeLevel, 2, HWND, 5u), DI_OK);
    CHECKEQ(vcall(dev, DD_Acquire, 0), DI_OK);
    CHECKEQ(vcall(dev, DD_Acquire, 0), DI_NOEFFECT);
    /* the reads: no input */
    uint32_t buf = gmem(20 * 512), n = gmem(4);
    memset(hle_ptr(buf), 0xAB, 20 * 512);
    MEM32(n) = 512;
    CHECKEQ(vcall(dev, DD_GetDeviceData, 4, 20u, buf, n, 0u), DI_OK);
    CHECKEQ(MEM32(n), 0);
    MEM32(n) = 512;
    CHECKEQ(vcall(dev, DD_GetDeviceData, 4, 16u, buf, n, 1u), DI_OK);   /* DX3 size, DIGDD_PEEK */
    CHECKEQ(MEM32(n), 0);
    uint32_t st = gmem(16);
    memset(hle_ptr(st), 0xAB, 16);
    CHECKEQ(vcall(dev, DD_GetDeviceState, 2, 16u, st), DI_OK);
    for (int i = 0; i < 16; i++) CHECKEQ(((uint8_t *)hle_ptr(st))[i], 0);
    CHECKEQ(vcall(dev, DD_GetDeviceState, 2, 20u, st), DIERR_INVALIDPARAM);
    /* shutdown (0x9AF210) */
    CHECKEQ(vcall(dev, DD_Unacquire, 0), DI_OK);
    CHECKEQ(vcall(dev, DD_Unacquire, 0), DI_NOEFFECT);
    CHECKEQ(vcall(dev, 2, 0), 0);
    CHECKEQ(vcall(di, 2, 0), 0);
    CHECKEQ(d3d_live_count(), live);
}

static void group_di_rules(void) {
    uint32_t di = new_di(), dev = new_mouse(di);
    uint32_t fmt = mouse_format(16), prop = bufsize_prop(512), buf = gmem(20 * 16), n = gmem(4), st = gmem(20);
    /* reads before Acquire */
    MEM32(n) = 16;
    CHECKEQ(vcall(dev, DD_GetDeviceData, 4, 20u, buf, n, 0u), DIERR_NOTACQUIRED);
    CHECKEQ(vcall(dev, DD_GetDeviceState, 2, 16u, st), DIERR_NOTACQUIRED);
    CHECKEQ(vcall(dev, DD_Unacquire, 0), DI_NOEFFECT);
    /* format checks */
    uint32_t bad = mouse_format(16);
    MEM32(bad) = 20;
    CHECKEQ(vcall(dev, DD_SetDataFormat, 1, bad), DIERR_INVALIDPARAM);
    bad = mouse_format(16);
    MEM32(bad + 4) = 12;
    CHECKEQ(vcall(dev, DD_SetDataFormat, 1, bad), DIERR_INVALIDPARAM);
    CHECKEQ(vcall(dev, DD_SetDataFormat, 1, mouse_format(24)), DIERR_INVALIDPARAM);
    CHECKEQ(vcall(dev, DD_SetDataFormat, 1, 0u), DIERR_INVALIDPARAM);
    CHECKEQ(vcall(dev, DD_SetDataFormat, 1, mouse_format(20)), DI_OK);          /* c_dfDIMouse2's size */
    CHECKEQ(vcall(dev, DD_SetDataFormat, 1, fmt), DI_OK);
    /* background levels are accepted too */
    CHECKEQ(vcall(dev, DD_SetCooperativeLevel, 2, HWND, 9u), DI_OK);
    CHECKEQ(vcall(dev, DD_SetCooperativeLevel, 2, HWND, 10u), DI_OK);
    CHECKEQ(vcall(dev, DD_SetCooperativeLevel, 2, HWND, 6u), DI_OK);
    CHECKEQ(vcall(dev, DD_SetProperty, 2, DIPROP_BUFFERSIZE, prop), DI_OK);
    /* setup calls while acquired */
    CHECKEQ(vcall(dev, DD_Acquire, 0), DI_OK);
    CHECKEQ(vcall(dev, DD_SetDataFormat, 1, fmt), DIERR_ACQUIRED);
    CHECKEQ(vcall(dev, DD_SetCooperativeLevel, 2, HWND, 5u), DIERR_ACQUIRED);
    CHECKEQ(vcall(dev, DD_SetProperty, 2, DIPROP_BUFFERSIZE, prop), DIERR_ACQUIRED);
    /* bad reads while acquired */
    MEM32(n) = 16;
    CHECKEQ(vcall(dev, DD_GetDeviceData, 4, 24u, buf, n, 0u), DIERR_INVALIDPARAM);
    CHECKEQ(vcall(dev, DD_GetDeviceData, 4, 20u, buf, 0u, 0u), DIERR_INVALIDPARAM);
    CHECKEQ(vcall(dev, DD_GetDeviceData, 4, 20u, buf, n, 2u), DIERR_INVALIDPARAM);
    CHECKEQ(vcall(dev, DD_GetDeviceState, 2, 16u, 0u), DIERR_INVALIDPARAM);
    CHECKEQ(vcall(dev, DD_GetDeviceData, 4, 20u, 0u, n, 0u), DI_OK);          /* a flush */
    CHECKEQ(MEM32(n), 0);
    /* after Unacquire, setup is allowed again and reads are refused */
    CHECKEQ(vcall(dev, DD_Unacquire, 0), DI_OK);
    CHECKEQ(vcall(dev, DD_SetCooperativeLevel, 2, HWND, 5u), DI_OK);
    MEM32(n) = 16;
    CHECKEQ(vcall(dev, DD_GetDeviceData, 4, 20u, buf, n, 0u), DIERR_NOTACQUIRED);
    /* the property header must be a DIPROPDWORD */
    uint32_t badp = bufsize_prop(64);
    MEM32(badp) = 16;
    CHECKEQ(vcall(dev, DD_SetProperty, 2, DIPROP_BUFFERSIZE, badp), DIERR_INVALIDPARAM);
    CHECKEQ(vcall(dev, DD_SetProperty, 2, DIPROP_BUFFERSIZE, 0u), DIERR_INVALIDPARAM);
    /* CreateDevice argument checks */
    uint32_t out = gmem(4);
    CHECKEQ(vcall(di, DI_CreateDevice, 3, gguid(GUID_SysMouse), 0u, 0u), DIERR_INVALIDPARAM);
    CHECKEQ(vcall(di, DI_CreateDevice, 3, gguid(GUID_SysMouse), out, 0x1000u), CLASS_E_NOAGGREGATION);
    CHECKEQ(vcall(dev, 2, 0), 0);
    CHECKEQ(vcall(di, 2, 0), 0);
}

static void group_di_loud(const char *what) {
    uint32_t di = new_di();
    if (!strcmp(what, "caps")) {
        uint32_t dev = new_mouse(di), caps = gmem(44);
        MEM32(caps) = 44;
        vcall(dev, DD_GetCapabilities, 1, caps);
    } else if (!strcmp(what, "guid")) {
        uint32_t out = gmem(4);
        vcall(di, DI_CreateDevice, 3, gguid(GUID_SysKeyboard), out, 0u);
    } else if (!strcmp(what, "coop")) {
        uint32_t dev = new_mouse(di);
        vcall(dev, DD_SetCooperativeLevel, 2, HWND, 0x15u);               /* DISCL_NOWINKEY */
    } else if (!strcmp(what, "prop")) {
        uint32_t dev = new_mouse(di);
        vcall(dev, DD_SetProperty, 2, 2u, bufsize_prop(0));               /* DIPROP_AXISMODE */
    } else if (!strcmp(what, "enum")) {
        vcall(di, 4, 4, 0u, 0x12345678u, 0u, 0u);                         /* EnumDevices */
    } else {
        printf("FAIL no loud case %s\n", what);
        fails++;
        return;
    }
    printf("di_loud: returned\n");
}

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: input_selftest <group> [arg]\n"); return 2; }
    hle_mem_init();
    bw2_register_shims();
    mach_enter();
    g_esp = hle_va(hle_alloc(1u << 20)) + (1u << 20) - 64;
    if (!strcmp(argv[1], "di_create")) group_di_create();
    else if (!strcmp(argv[1], "di_game_setup")) group_di_game_setup();
    else if (!strcmp(argv[1], "di_rules")) group_di_rules();
    else if (!strcmp(argv[1], "di_loud") && argc > 2) group_di_loud(argv[2]);
    else { fprintf(stderr, "input_selftest: no group %s\n", argv[1]); return 2; }
    printf("%s: %d failed\n", argv[1], fails);
    fflush(stdout);
    return fails != 0;
}
