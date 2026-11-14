/*
 * input_selftest.c - the DirectInput mouse and the host input queue driven as
 * lifted code drives them: arguments pushed right to left, a return address,
 * then the shim; COM methods through the guest vtable. Each check also
 * verifies the stdcall pop. The queue groups (plan 6) push host events and
 * pump the messages as the game does (PeekMessageA, TranslateMessage,
 * DispatchMessageA), recording what the window procedures are given.
 * Built with the engine (minus main.c) by tests/test_input.py;
 * `input_selftest <group> [arg]` runs one group and prints "<group>: <n> failed".
 * The translator groups (plan 6 Task 3) call translate.c directly.
 */
#include <fcntl.h>
#include <math.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "win32hle.h"
#include "bw2.h"
#include "d3d_com.h"
#include "input.h"

const recomp_dispatch_entry_t recomp_dispatch_table[1] = { { 0, 0 } };
const uint32_t recomp_dispatch_count = 0;

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)
#define CHECKEQ(a, b) do { uint64_t _a = (uint64_t)(a), _b = (uint64_t)(b); if (_a != _b) { \
    printf("FAIL %s:%d %s == %s: 0x%llX != 0x%llX\n", __FILE__, __LINE__, #a, #b, \
           (unsigned long long)_a, (unsigned long long)_b); fails++; } } while (0)
#define CHECKSTR(a, b) do { const char *_a = (a), *_b = (b); if (strcmp(_a, _b)) { \
    printf("FAIL %s:%d %s: '%s' != '%s'\n", __FILE__, __LINE__, #a, _a, _b); fails++; } } while (0)

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
static const uint8_t GUID_Button[16] = G(0xA36D02F0, 0xC9F3, 0x11CF, 0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00);

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
#define DIPROP_AXISMODE 2u
#define DIPROP_RANGE 4u
#define DIERR_INPUTLOST 0x8007001Eu
#define DIERR_OTHERAPPHASPRIO 0x80070005u
#define DIERR_NOTBUFFERED 0x80040207u
#define E_NOTIMPL 0x80004001u
#define E_HANDLE 0x80070006u
#define DI_BUFFEROVERFLOW 1u
#define DIDFT_OPTIONAL 0x80000000u
#define DIGDD_PEEK 1u
#define INFINITE 0xFFFFFFFFu
#define HINST 0x00400000u

/* The window SetCooperativeLevel is given: a real top-level window, made once per process. */
static uint32_t make_window(uint32_t cls_style, int x, int y, int w, int h);
static uint32_t g_di_hwnd;
static uint32_t di_window(void) {
    if (!g_di_hwnd) g_di_hwnd = make_window(0, 50, 50, 1024, 768);
    return g_di_hwnd;
}
#define HWND di_window()

/* IDirectInput8A and IDirectInputDevice8A slots */
enum { DI_CreateDevice = 3 };
enum { DD_GetCapabilities = 3, DD_EnumObjects, DD_GetProperty, DD_SetProperty, DD_Acquire, DD_Unacquire,
       DD_GetDeviceState, DD_GetDeviceData, DD_SetDataFormat, DD_SetEventNotification, DD_SetCooperativeLevel };

/* DirectInput8Create(hinst, 0x800, IID_IDirectInput8A, &di, NULL) */
static uint32_t new_di(void) {
    di_window();
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
    CHECKEQ(vcall(dev, DD_SetDataFormat, 1, mouse_format(24)), DI_OK);    /* a multiple of 4, every offset inside */
    bad = mouse_format(16);
    MEM32(MEM32(bad + 20) + 6 * 16 + 4) = 16;                                /* the last button at 16 of 16 */
    CHECKEQ(vcall(dev, DD_SetDataFormat, 1, bad), DIERR_INVALIDPARAM);
    CHECKEQ(vcall(dev, DD_SetDataFormat, 1, 0u), DIERR_INVALIDPARAM);
    CHECKEQ(vcall(dev, DD_SetDataFormat, 1, mouse_format(20)), DI_OK);          /* c_dfDIMouse2's size */
    CHECKEQ(vcall(dev, DD_SetDataFormat, 1, fmt), DI_OK);
    /* non-exclusive background is accepted; an exclusive background mouse is not implemented by DirectInput */
    CHECKEQ(vcall(dev, DD_SetCooperativeLevel, 2, HWND, 9u), E_NOTIMPL);
    CHECKEQ(vcall(dev, DD_SetCooperativeLevel, 2, HWND, 10u), DI_OK);
    CHECKEQ(vcall(dev, DD_SetCooperativeLevel, 2, HWND, 3u), DIERR_INVALIDPARAM);   /* both exclusive and not */
    CHECKEQ(vcall(dev, DD_SetCooperativeLevel, 2, HWND, 12u), DIERR_INVALIDPARAM);  /* both fore- and background */
    CHECKEQ(vcall(dev, DD_SetCooperativeLevel, 2, HWND, 4u), DIERR_INVALIDPARAM);   /* neither exclusive nor not */
    CHECKEQ(vcall(dev, DD_SetCooperativeLevel, 2, HWND, 2u), DIERR_INVALIDPARAM);   /* neither fore- nor background */
    CHECKEQ(vcall(dev, DD_SetCooperativeLevel, 2, 0x12345u, 6u), E_HANDLE);         /* no such window */
    CHECKEQ(vcall(dev, DD_SetCooperativeLevel, 2, 0u, 6u), E_HANDLE);
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
        vcall(dev, DD_SetProperty, 2, DIPROP_RANGE, bufsize_prop(0));
    } else if (!strcmp(what, "enum")) {
        vcall(di, 4, 4, 0u, 0x12345678u, 0u, 0u);                         /* EnumDevices */
    } else {
        printf("FAIL no loud case %s\n", what);
        fails++;
        return;
    }
    printf("di_loud: returned\n");
}

/* BW2_QUIT_AFTER_READS (set by the test): the reads before the nth answer as
 * before, and the nth exits 0 before answering. "data" reads with
 * GetDeviceData only; "mixed" alternates GetDeviceData and GetDeviceState,
 * which count together. Each answered read prints a line, flushed, since the
 * quit never returns here. */
static void group_di_quit(const char *how) {
    int mixed = how && !strcmp(how, "mixed");
    uint32_t di = new_di(), dev = new_mouse(di);
    CHECKEQ(vcall(dev, DD_SetDataFormat, 1, mouse_format(16)), DI_OK);
    CHECKEQ(vcall(dev, DD_SetCooperativeLevel, 2, HWND, 5u), DI_OK);
    CHECKEQ(vcall(dev, DD_SetProperty, 2, DIPROP_BUFFERSIZE, bufsize_prop(512)), DI_OK);
    CHECKEQ(vcall(dev, DD_Acquire, 0), DI_OK);
    uint32_t buf = gmem(20 * 512), n = gmem(4), st = gmem(16);
    for (int i = 1; i <= 8; i++) {
        printf("di_quit: read %d asked (%d failed so far)\n", i, fails);
        fflush(stdout);
        if (mixed && i % 2 == 0) {
            memset(hle_ptr(st), 0xAB, 16);
            CHECKEQ(vcall(dev, DD_GetDeviceState, 2, 16u, st), DI_OK);
            for (int k = 0; k < 16; k++) CHECKEQ(((uint8_t *)hle_ptr(st))[k], 0);
        } else {
            MEM32(n) = 512;
            CHECKEQ(vcall(dev, DD_GetDeviceData, 4, 20u, buf, n, 0u), DI_OK);
            CHECKEQ(MEM32(n), 0);
        }
        printf("di_quit: read %d answered (%d failed so far)\n", i, fails);
        fflush(stdout);
    }
    printf("di_quit: returned\n");
}

/* ---- plan 6: the host input queue and the Windows messages it makes ---- */

#define WM_ACTIVATE 0x0006u
#define WM_SETFOCUS 0x0007u
#define WM_KILLFOCUS 0x0008u
#define WM_CLOSE 0x0010u
#define WM_ACTIVATEAPP 0x001Cu
#define WM_KEYDOWN 0x0100u
#define WM_KEYUP 0x0101u
#define WM_CHAR 0x0102u
#define WM_SYSKEYDOWN 0x0104u
#define WM_SYSKEYUP 0x0105u
#define WM_SYSCOMMAND 0x0112u
#define SC_CLOSE 0xF060u
#define WM_MOUSEMOVE 0x0200u
#define WM_LBUTTONDOWN 0x0201u
#define WM_LBUTTONUP 0x0202u
#define WM_LBUTTONDBLCLK 0x0203u
#define WM_MOUSEWHEEL 0x020Au
#define CS_DBLCLKS 8u
#define ANY 0xFFFFFFFFu

/* What the window procedures are given (hle_proc_hook), input and focus
 * messages only; a posted message also keeps the MSG's time. */
static struct rec { uint32_t h, m, w, l, t; } g_rec[4096];
static int g_nrec;
static uint32_t g_peek_time;
static int recorded(uint32_t m) {
    return m == WM_ACTIVATE || m == WM_SETFOCUS || m == WM_KILLFOCUS || m == WM_CLOSE || m == WM_ACTIVATEAPP ||
           m == WM_SYSCOMMAND ||
           (m >= 0x0100u && m <= 0x0109u) || (m >= 0x0200u && m <= 0x020Eu);
}
static void recorder(uint32_t h, uint32_t m, uint32_t w, uint32_t l) {
    if (recorded(m) && g_nrec < 4096) g_rec[g_nrec++] = (struct rec){ h, m, w, l, g_peek_time };
}
static void rec_reset(void) { g_nrec = 0; }
static int rec_has(uint32_t m) { for (int i = 0; i < g_nrec; i++) if (g_rec[i].m == m) return 1; return 0; }

/* The recorded messages, in order: n triples of (message, wParam, lParam), ANY matching anything. */
static void expect_msgs(int line, int n, ...) {
    va_list ap;
    va_start(ap, n);
    int ok = g_nrec == n;
    for (int i = 0; i < n; i++) {
        uint32_t m = va_arg(ap, uint32_t), w = va_arg(ap, uint32_t), l = va_arg(ap, uint32_t);
        if (i >= g_nrec || g_rec[i].m != m || (w != ANY && g_rec[i].w != w) || (l != ANY && g_rec[i].l != l)) ok = 0;
    }
    va_end(ap);
    if (ok) return;
    printf("FAIL %s:%d: %d messages, want %d:", __FILE__, line, g_nrec, n);
    for (int i = 0; i < g_nrec; i++) printf(" %04X(%X,%X)", g_rec[i].m, g_rec[i].w, g_rec[i].l);
    printf("\n");
    fails++;
}
#define EXPECT(...) expect_msgs(__LINE__, __VA_ARGS__)

/* The game's pump (0x9B10D0, 0x9B1100): peek, translate, dispatch, until empty. */
static void pump_all(void) {
    uint32_t msg = gmem(28);
    for (int i = 0; i < 10000; i++) {
        if (!call("PeekMessageA", 5, msg, 0u, 0u, 0u, 1u)) return;
        g_peek_time = MEM32(msg + 16);
        call("TranslateMessage", 1, msg);
        call("DispatchMessageA", 1, msg);
    }
    printf("FAIL pump_all: the queue never emptied\n");
    fails++;
}

static uint32_t gstr(const char *s) { uint32_t p = gmem((uint32_t)strlen(s) + 1); strcpy(hle_ptr(p), s); return p; }

/* A WS_POPUP | WS_VISIBLE window on its own class (DefWindowProcA), at screen (x, y). */
static uint32_t make_window(uint32_t cls_style, int x, int y, int w, int h) {
    static int n;
    char name[16];
    snprintf(name, sizeof name, "bw2in%d", n++);
    uint32_t nm = gstr(name), wc = gmem(40);
    MEM32(wc) = cls_style, MEM32(wc + 4) = hle_resolve("DefWindowProcA"), MEM32(wc + 16) = HINST, MEM32(wc + 36) = nm;
    CHECK(call("RegisterClassA", 1, wc) != 0);
    uint32_t hwnd = call("CreateWindowExA", 12, 0u, nm, gstr(""), 0x90000000u, (uint32_t)x, (uint32_t)y, (uint32_t)w,
                         (uint32_t)h, 0u, 0u, HINST, 0u);
    CHECK(hwnd != 0);
    return hwnd;
}

static void ev_move(int x, int y) {
    struct bw2_in_event e = { .kind = BW2_IN_MOVE, .x = x, .y = y };
    bw2_in_push(&e);
}
static void ev_button_at(int button, int down, int x, int y, uint32_t t) {
    struct bw2_in_event e = { .kind = BW2_IN_BUTTON, .time_ms = t, .x = x, .y = y, .button = button, .down = down };
    bw2_in_push_at(&e);
}
static void ev_button(int button, int down, int x, int y) {
    struct bw2_in_event e = { .kind = BW2_IN_BUTTON, .x = x, .y = y, .button = button, .down = down };
    bw2_in_push(&e);
}
static void ev_key(int down, uint32_t vk, uint32_t scan, int extended) {
    struct bw2_in_event e = { .kind = BW2_IN_KEY, .down = down, .vk = vk, .scan = scan, .extended = extended };
    bw2_in_push(&e);
}
static void ev_activate(int active) {
    struct bw2_in_event e = { .kind = BW2_IN_ACTIVATE, .down = active };
    bw2_in_push(&e);
}
static uint32_t key_state(uint32_t vk) { return call("GetKeyState", 1, vk); }
static int key_down(uint32_t vk) { return (key_state(vk) & 0x8000u) != 0; }

/* queue: a second thread's 10,000 events arrive in order, none lost, drained
 * after it finished (the ring grows) and while it pushes. */
static int g_sunk[10000], g_nsunk;
static void sink_record(const struct bw2_in_event *e) {
    if (e->kind == BW2_IN_MOVE && g_nsunk < 10000) g_sunk[g_nsunk++] = e->dx;
    else if (e->kind == BW2_IN_MOVE) g_nsunk++;
}
static volatile int g_pushed_all;
static void *pusher(void *arg) {
    (void)arg;
    for (int i = 0; i < 10000; i++) {
        struct bw2_in_event e = { .kind = BW2_IN_MOVE, .dx = i };
        bw2_in_push(&e);
    }
    __atomic_store_n(&g_pushed_all, 1, __ATOMIC_SEQ_CST);
    return NULL;
}
static void check_sunk(void) {
    CHECKEQ(g_nsunk, 10000);
    int order = 1;
    for (int i = 0; i < g_nsunk && i < 10000; i++) if (g_sunk[i] != i) order = 0;
    CHECK(order);
}
static void group_queue(void) {
    bw2_in_set_mouse_sink(sink_record);
    pthread_t t;
    /* all pushed first, then one drain */
    g_nsunk = 0, g_pushed_all = 0;
    CHECK(pthread_create(&t, NULL, pusher, NULL) == 0);
    pthread_join(t, NULL);
    bw2_in_drain();
    check_sunk();
    /* drained while it pushes */
    g_nsunk = 0, g_pushed_all = 0;
    CHECK(pthread_create(&t, NULL, pusher, NULL) == 0);
    while (!__atomic_load_n(&g_pushed_all, __ATOMIC_SEQ_CST)) bw2_in_drain_input();
    pthread_join(t, NULL);
    bw2_in_drain();
    check_sunk();
    /* an empty queue drains to nothing */
    bw2_in_drain();
    bw2_in_drain_input();
    CHECKEQ(g_nsunk, 10000);
    CHECK(!hle_msg_pop(NULL, NULL, NULL));
    CHECKEQ(bw2_in_active(), 1);
    CHECKEQ(bw2_in_losses(), 0);
    bw2_in_set_mouse_sink(bw2_dinput8_sink);
}

static void group_mouse_msgs(void) {
    uint32_t a = make_window(CS_DBLCLKS, 100, 50, 640, 480);
    uint32_t b = make_window(0, 800, 50, 200, 200);
    uint32_t pt = gmem(8);
    hle_proc_hook = recorder;
    bw2_in_set_window(a);
    pump_all();
    /* a move: the client point in lParam, the screen point for GetCursorPos */
    rec_reset();
    ev_move(10, 20);
    CHECKEQ(call("GetCursorPos", 1, pt), 1);
    CHECKEQ(MEM32(pt), 110);
    CHECKEQ(MEM32(pt + 4), 70);
    pump_all();
    EXPECT(1, WM_MOUSEMOVE, ANY, (20u << 16) | 10u);
    if (g_nrec) CHECKEQ(g_rec[0].h, a);
    /* down, up, down within 500 ms and 4 px: a double click; a third press starts over */
    uint32_t t0 = 1000000u;
    rec_reset();
    ev_button_at(0, 1, 10, 20, t0);
    ev_button_at(0, 0, 11, 21, t0 + 50);
    ev_button_at(0, 1, 12, 22, t0 + 100);
    ev_button_at(0, 0, 12, 22, t0 + 150);
    ev_button_at(0, 1, 12, 22, t0 + 200);
    ev_button_at(0, 0, 12, 22, t0 + 250);
    pump_all();
    EXPECT(6, WM_LBUTTONDOWN, 1u, (20u << 16) | 10u, WM_LBUTTONUP, 0u, (21u << 16) | 11u,
           WM_LBUTTONDBLCLK, 1u, (22u << 16) | 12u, WM_LBUTTONUP, 0u, ANY, WM_LBUTTONDOWN, 1u, ANY, WM_LBUTTONUP, 0u, ANY);
    if (g_nrec == 6) {
        CHECKEQ(g_rec[0].t, t0);                         /* the message's time is the event's */
        CHECKEQ(g_rec[2].t, t0 + 100);
        CHECKEQ(g_rec[0].h, a);
    }
    /* outside the 4 px wide rectangle centred on the first press: no double click */
    rec_reset();
    ev_button_at(0, 1, 100, 100, t0 + 1000);
    ev_button_at(0, 0, 100, 100, t0 + 1010);
    ev_button_at(0, 1, 103, 100, t0 + 1020);
    ev_button_at(0, 0, 103, 100, t0 + 1030);
    pump_all();
    EXPECT(4, WM_LBUTTONDOWN, ANY, ANY, WM_LBUTTONUP, ANY, ANY, WM_LBUTTONDOWN, ANY, ANY, WM_LBUTTONUP, ANY, ANY);
    /* a class without CS_DBLCLKS: a second down */
    bw2_in_set_window(b);
    rec_reset();
    ev_button_at(0, 1, 10, 20, t0 + 2000);
    ev_button_at(0, 0, 10, 20, t0 + 2050);
    ev_button_at(0, 1, 10, 20, t0 + 2100);
    ev_button_at(0, 0, 10, 20, t0 + 2150);
    pump_all();
    EXPECT(4, WM_LBUTTONDOWN, ANY, (20u << 16) | 10u, WM_LBUTTONUP, ANY, ANY, WM_LBUTTONDOWN, ANY, ANY,
           WM_LBUTTONUP, ANY, ANY);
    if (g_nrec) CHECKEQ(g_rec[0].h, b);
    CHECKEQ(call("GetCursorPos", 1, pt), 1);
    CHECKEQ(MEM32(pt), 810);
    /* 600 ms apart, pushed and drained together: the events' times decide */
    bw2_in_set_window(a);
    rec_reset();
    ev_button_at(0, 1, 10, 20, t0 + 3000);
    ev_button_at(0, 0, 10, 20, t0 + 3010);
    ev_button_at(0, 1, 10, 20, t0 + 3600);
    ev_button_at(0, 0, 10, 20, t0 + 3610);
    pump_all();
    EXPECT(4, WM_LBUTTONDOWN, ANY, ANY, WM_LBUTTONUP, ANY, ANY, WM_LBUTTONDOWN, ANY, ANY, WM_LBUTTONUP, ANY, ANY);
    /* the right button, and buttons 3 and 4 make no message */
    rec_reset();
    ev_button(1, 1, 10, 20);
    ev_button(1, 0, 10, 20);
    ev_button(3, 1, 10, 20);
    ev_button(4, 0, 10, 20);
    pump_all();
    EXPECT(2, 0x0204u, 2u, (20u << 16) | 10u, 0x0205u, 0u, ANY);
    /* the clip clamps the reported point while active; inverted clears it */
    int clip[4] = { 100, 50, 105, 55 };
    bw2_in_clip(clip);
    ev_move(10, 20);
    call("GetCursorPos", 1, pt);
    CHECKEQ(MEM32(pt), 104);
    CHECKEQ(MEM32(pt + 4), 54);
    ev_activate(0);
    pump_all();
    ev_move(10, 20);
    call("GetCursorPos", 1, pt);
    CHECKEQ(MEM32(pt), 110);                             /* inactive: nothing clamped */
    ev_activate(1);
    pump_all();
    int inverted[4] = { 300, 50, 100, 55 };
    bw2_in_clip(inverted);
    ev_move(10, 20);
    call("GetCursorPos", 1, pt);
    CHECKEQ(MEM32(pt), 110);
    CHECKEQ(MEM32(pt + 4), 70);
    hle_proc_hook = NULL;
}

static void group_wheel_msgs(void) {
    uint32_t a = make_window(CS_DBLCLKS, 100, 50, 640, 480);
    hle_proc_hook = recorder;
    bw2_in_set_window(a);
    pump_all();
    rec_reset();
    struct bw2_in_event e = { .kind = BW2_IN_WHEEL, .x = 5, .y = 6, .wheel = -240 };
    bw2_in_push(&e);
    pump_all();
    EXPECT(1, WM_MOUSEWHEEL, ANY, (56u << 16) | 105u);
    if (g_nrec == 1) {
        CHECKEQ(g_rec[0].w >> 16, (uint16_t)-240);
        CHECKEQ(g_rec[0].w & 0xFFFFu, 0);
        CHECKEQ(g_rec[0].h, a);
    }
    /* the key state in the low word: the left button held */
    rec_reset();
    ev_button(0, 1, 5, 6);
    e.wheel = 120;
    bw2_in_push(&e);
    ev_button(0, 0, 5, 6);
    pump_all();
    EXPECT(3, WM_LBUTTONDOWN, ANY, ANY, WM_MOUSEWHEEL, (120u << 16) | 1u, (56u << 16) | 105u, WM_LBUTTONUP, ANY, ANY);
    hle_proc_hook = NULL;
}

static void group_key_msgs(void) {
    uint32_t a = make_window(CS_DBLCLKS, 100, 50, 640, 480);
    hle_proc_hook = recorder;
    bw2_in_set_window(a);
    pump_all();
    /* an extended key: bit 24; a repeat: bit 30; the up: bits 30 and 31 */
    rec_reset();
    ev_key(1, 0x25u, 0x4Bu, 1);
    ev_key(1, 0x25u, 0x4Bu, 1);
    ev_key(0, 0x25u, 0x4Bu, 1);
    pump_all();
    EXPECT(3, WM_KEYDOWN, 0x25u, 0x014B0001u, WM_KEYDOWN, 0x25u, 0x414B0001u, WM_KEYUP, 0x25u, 0xC14B0001u);
    if (g_nrec) CHECKEQ(g_rec[0].h, a);
    /* left Shift: the generic VK in wParam, both in the key state; Shift+A gives 'A' */
    rec_reset();
    ev_key(1, 0xA0u, 0x2Au, 0);
    pump_all();
    EXPECT(1, WM_KEYDOWN, 0x10u, 0x002A0001u);
    CHECK(key_down(0xA0u));
    CHECK(key_down(0x10u));
    CHECK(!key_down(0xA1u));
    rec_reset();
    ev_key(1, 'A', 0x1Eu, 0);
    pump_all();
    EXPECT(2, WM_KEYDOWN, 'A', 0x001E0001u, WM_CHAR, 'A', 0x001E0001u);
    rec_reset();
    ev_key(0, 'A', 0x1Eu, 0);
    ev_key(0, 0xA0u, 0x2Au, 0);
    pump_all();
    EXPECT(2, WM_KEYUP, 'A', 0xC01E0001u, WM_KEYUP, 0x10u, 0xC02A0001u);
    CHECK(!key_down(0xA0u));
    CHECK(!key_down(0x10u));
    /* right Shift down while left goes up: the generic key stays down */
    ev_key(1, 0xA0u, 0x2Au, 0);
    ev_key(1, 0xA1u, 0x36u, 0);
    ev_key(0, 0xA0u, 0x2Au, 0);
    pump_all();
    CHECK(key_down(0x10u));
    CHECK(key_down(0xA1u));
    ev_key(0, 0xA1u, 0x36u, 0);
    pump_all();
    CHECK(!key_down(0x10u));
    /* left Alt reads down through GetKeyState(VK_LMENU); Alt+F4 is a system key */
    rec_reset();
    ev_key(1, 0xA4u, 0x38u, 0);
    pump_all();
    CHECK(key_down(0xA4u));
    CHECK(key_down(0x12u));
    ev_key(1, 0x73u, 0x3Eu, 0);
    ev_key(0, 0x73u, 0x3Eu, 0);
    ev_key(0, 0xA4u, 0x38u, 0);
    pump_all();
    EXPECT(4, WM_SYSKEYDOWN, 0x12u, 0x20380001u, WM_SYSKEYDOWN, 0x73u, 0x203E0001u, WM_SYSKEYUP, 0x73u, 0xE03E0001u,
           WM_SYSKEYUP, 0x12u, 0xC0380001u);
    CHECK(!key_down(0xA4u));
    CHECK(!key_down(0x12u));
    /* Backspace gives WM_CHAR 8 */
    rec_reset();
    ev_key(1, 0x08u, 0x0Eu, 0);
    pump_all();
    ev_key(0, 0x08u, 0x0Eu, 0);
    pump_all();
    EXPECT(3, WM_KEYDOWN, 8u, 0x000E0001u, WM_CHAR, 8u, 0x000E0001u, WM_KEYUP, 8u, 0xC00E0001u);
    hle_proc_hook = NULL;
}

static void group_activate(void) {
    uint32_t a = make_window(CS_DBLCLKS, 100, 50, 640, 480);
    hle_proc_hook = recorder;
    bw2_in_set_window(a);
    pump_all();
    CHECKEQ(bw2_in_active(), 1);
    /* L and A held */
    ev_button(0, 1, 5, 5);
    ev_key(1, 'A', 0x1Eu, 0);
    pump_all();
    CHECK(key_down('A'));
    CHECK(key_down(1));
    rec_reset();
    ev_activate(0);
    pump_all();
    EXPECT(5, WM_LBUTTONUP, 0u, (5u << 16) | 5u, WM_KEYUP, 'A', 0xC01E0001u, WM_ACTIVATE, 0u, 0u, WM_KILLFOCUS, 0u, 0u,
           WM_ACTIVATEAPP, 0u, ANY);
    for (int i = 0; i < g_nrec; i++) CHECKEQ(g_rec[i].h, a);
    CHECKEQ(bw2_in_active(), 0);
    CHECK(!key_down('A'));
    CHECK(!key_down(1));
    CHECKEQ(bw2_in_losses(), 1);
    /* back: the three in Windows' order */
    rec_reset();
    ev_activate(1);
    pump_all();
    EXPECT(3, WM_ACTIVATEAPP, 1u, ANY, WM_ACTIVATE, 1u, 0u, WM_SETFOCUS, 0u, 0u);
    CHECKEQ(bw2_in_active(), 1);
    rec_reset();
    ev_activate(0);
    pump_all();
    EXPECT(3, WM_ACTIVATE, 0u, 0u, WM_KILLFOCUS, 0u, 0u, WM_ACTIVATEAPP, 0u, ANY);
    CHECKEQ(bw2_in_losses(), 2);                         /* nothing held: only the three */
    ev_activate(1);
    pump_all();
    CHECKEQ(bw2_in_active(), 1);
    hle_proc_hook = NULL;
}

static void group_close(void) {
    uint32_t a = make_window(0, 0, 0, 640, 480);
    uint32_t b = make_window(0, 0, 0, 320, 240);
    hle_proc_hook = recorder;
    pump_all();
    /* until set, the first window */
    rec_reset();
    struct bw2_in_event e = { .kind = BW2_IN_CLOSE };
    bw2_in_push(&e);
    pump_all();
    /* as Windows gives the close button and Alt+F4; DefWindowProc makes the WM_CLOSE */
    EXPECT(2, WM_SYSCOMMAND, SC_CLOSE, 0u, WM_CLOSE, 0u, 0u);
    if (g_nrec) CHECKEQ(g_rec[0].h, a);
    bw2_in_set_window(b);
    rec_reset();
    bw2_in_push(&e);
    bw2_in_drain_input();                                /* a close is posted: a read may drain it */
    CHECKEQ(g_nrec, 0);
    pump_all();
    EXPECT(2, WM_SYSCOMMAND, SC_CLOSE, 0u, WM_CLOSE, 0u, 0u);
    if (g_nrec) CHECKEQ(g_rec[0].h, b);
    hle_proc_hook = NULL;
}

static void group_drain_points(void) {
    uint32_t a = make_window(CS_DBLCLKS, 100, 50, 640, 480);
    hle_proc_hook = recorder;
    bw2_in_set_window(a);
    pump_all();
    /* GetAsyncKeyState drains; GetKeyState does not */
    ev_key(1, 'A', 0x1Eu, 0);
    CHECK((call("GetAsyncKeyState", 1, 'A') & 0x8000u) != 0);
    pump_all();
    ev_key(0, 'A', 0x1Eu, 0);
    CHECK(key_down('A'));
    pump_all();
    CHECK(!key_down('A'));
    /* C held, then an inactive activation and a key before a read */
    ev_key(1, 'C', 0x2Eu, 0);
    pump_all();
    unsigned losses = bw2_in_losses();
    rec_reset();
    ev_activate(0);
    ev_key(1, 'B', 0x30u, 0);
    CHECKEQ(call("GetAsyncKeyState", 1, 'B') & 0x8000u, 0);          /* the key waits behind the activation */
    CHECK(!rec_has(WM_ACTIVATE) && !rec_has(WM_KILLFOCUS) && !rec_has(WM_ACTIVATEAPP));
    CHECKEQ(g_nrec, 0);
    CHECKEQ(bw2_in_losses(), losses + 1);
    CHECKEQ(bw2_in_active(), 0);
    CHECKEQ(call("GetAsyncKeyState", 1, 'B') & 0x8000u, 0);          /* a second read: still one loss */
    CHECKEQ(bw2_in_losses(), losses + 1);
    pump_all();
    EXPECT(6, WM_KEYUP, 'C', ANY, WM_ACTIVATE, 0u, 0u, WM_KILLFOCUS, 0u, 0u, WM_ACTIVATEAPP, 0u, ANY,
           WM_KEYDOWN, 'B', ANY, WM_CHAR, 'b', ANY);
    CHECKEQ(bw2_in_losses(), losses + 1);
    CHECKEQ(bw2_in_active(), 0);
    ev_key(0, 'B', 0x30u, 0);
    pump_all();
    /* an active activation before GetCursorPos waits for the pump */
    uint32_t pt = gmem(8);
    rec_reset();
    ev_activate(1);
    ev_move(1, 2);
    CHECKEQ(call("GetCursorPos", 1, pt), 1);
    CHECKEQ(g_nrec, 0);
    CHECKEQ(bw2_in_active(), 0);
    pump_all();
    EXPECT(4, WM_ACTIVATEAPP, 1u, ANY, WM_ACTIVATE, 1u, 0u, WM_SETFOCUS, 0u, 0u, WM_MOUSEMOVE, ANY, (2u << 16) | 1u);
    CHECKEQ(bw2_in_active(), 1);
    CHECKEQ(bw2_in_losses(), losses + 1);
    hle_proc_hook = NULL;
}

/* ---- plan 6 Task 2: the DirectInput mouse on the host input queue ---- */

/* A data format of n objects {guid or NULL, offset, type}, built in guest memory. */
struct fobj { const uint8_t *guid; uint32_t ofs, type; };
static uint32_t fmt_build(uint32_t flags, uint32_t data_size, int n, const struct fobj *o) {
    uint32_t objs = gmem(16u * (uint32_t)(n ? n : 1)), f = gmem(24);
    for (int i = 0; i < n; i++) {
        uint32_t a = objs + 16u * (uint32_t)i;
        MEM32(a) = o[i].guid ? gguid(o[i].guid) : 0, MEM32(a + 4) = o[i].ofs, MEM32(a + 8) = o[i].type;
        MEM32(a + 12) = 0;
    }
    MEM32(f) = 24, MEM32(f + 4) = 16, MEM32(f + 8) = flags, MEM32(f + 12) = data_size, MEM32(f + 16) = (uint32_t)n;
    MEM32(f + 20) = objs;
    return f;
}
/* the game's seven objects ("Measured" item 2), and room for an eighth */
static struct fobj g_game_objs[8] = {
    { GUID_XAxis, 0, 0x00FFFF03u }, { GUID_YAxis, 4, 0x00FFFF03u }, { GUID_ZAxis, 8, 0x80FFFF03u },
    { NULL, 12, 0x00FFFF0Cu }, { NULL, 13, 0x00FFFF0Cu }, { NULL, 14, 0x80FFFF0Cu }, { NULL, 15, 0x80FFFF0Cu } };

static uint32_t dword_prop(uint32_t v) { return bufsize_prop(v); }

/* The game's setup (0x9AF220) and mode switch (0x9AF790), with a buffer of `buffer` elements. */
static uint32_t game_mouse(uint32_t di, uint32_t buffer) {
    uint32_t dev = new_mouse(di);
    CHECKEQ(vcall(dev, DD_SetDataFormat, 1, mouse_format(16)), DI_OK);
    CHECKEQ(vcall(dev, DD_SetCooperativeLevel, 2, HWND, 6u), DI_OK);
    CHECKEQ(vcall(dev, DD_SetProperty, 2, DIPROP_BUFFERSIZE, bufsize_prop(buffer)), DI_OK);
    CHECKEQ(vcall(dev, DD_Acquire, 0), DI_OK);
    CHECKEQ(vcall(dev, DD_Unacquire, 0), DI_OK);
    CHECKEQ(vcall(dev, DD_SetCooperativeLevel, 2, HWND, 5u), DI_OK);
    CHECKEQ(vcall(dev, DD_Acquire, 0), DI_OK);
    return dev;
}

static void ev_delta(int dx, int dy, uint32_t t) {
    struct bw2_in_event e = { .kind = BW2_IN_MOVE, .time_ms = t, .dx = dx, .dy = dy };
    bw2_in_push_at(&e);
}
static void ev_wheel(int wheel, uint32_t t) {
    struct bw2_in_event e = { .kind = BW2_IN_WHEEL, .time_ms = t, .wheel = wheel };
    bw2_in_push_at(&e);
}

/* element i of a GetDeviceData buffer of cb-byte elements */
static uint32_t el(uint32_t buf, uint32_t cb, int i, int field) { return MEM32(buf + cb * (uint32_t)i + 4u * (uint32_t)field); }
enum { EL_OFS, EL_DATA, EL_TIME, EL_SEQ, EL_APP };

static uint32_t read_data(uint32_t dev, uint32_t cb, uint32_t buf, uint32_t *n, uint32_t flags) {
    uint32_t pn = gmem(4);
    MEM32(pn) = *n;
    uint32_t hr = vcall(dev, DD_GetDeviceData, 4, cb, buf, pn, flags);
    *n = MEM32(pn);
    return hr;
}

static void group_di_format(void) {
    uint32_t di = new_di(), dev = new_mouse(di);
    struct fobj o[8];
    memcpy(o, g_game_objs, sizeof o);
    /* the game's format */
    CHECKEQ(vcall(dev, DD_SetDataFormat, 1, fmt_build(2, 16, 7, o)), DI_OK);
    /* a fourth axis: no such object; optional, it is left out */
    o[7] = (struct fobj){ NULL, 16, 0x00FFFF03u };
    CHECKEQ(vcall(dev, DD_SetDataFormat, 1, fmt_build(2, 20, 8, o)), DIERR_INVALIDPARAM);
    o[7].type |= DIDFT_OPTIONAL;
    CHECKEQ(vcall(dev, DD_SetDataFormat, 1, fmt_build(2, 20, 8, o)), DI_OK);
    o[7] = (struct fobj){ NULL, 16, 0x00000303u };                          /* axis instance 3 */
    CHECKEQ(vcall(dev, DD_SetDataFormat, 1, fmt_build(2, 20, 8, o)), DIERR_INVALIDPARAM);
    /* a ninth button: there are eight */
    struct fobj b[9];
    for (int i = 0; i < 9; i++) b[i] = (struct fobj){ GUID_Button, (uint32_t)i, 0x00FFFF0Cu };
    CHECKEQ(vcall(dev, DD_SetDataFormat, 1, fmt_build(2, 12, 8, b)), DI_OK);
    CHECKEQ(vcall(dev, DD_SetDataFormat, 1, fmt_build(2, 12, 9, b)), DIERR_INVALIDPARAM);
    /* by instance: button 7, then an axis GUID on a button type matches nothing */
    struct fobj one[1] = { { NULL, 0, 0x0000070Cu } };
    CHECKEQ(vcall(dev, DD_SetDataFormat, 1, fmt_build(2, 4, 1, one)), DI_OK);
    one[0] = (struct fobj){ NULL, 0, 0x0000080Cu };
    CHECKEQ(vcall(dev, DD_SetDataFormat, 1, fmt_build(2, 4, 1, one)), DIERR_INVALIDPARAM);
    one[0] = (struct fobj){ GUID_XAxis, 0, 0x00FFFF0Cu };
    CHECKEQ(vcall(dev, DD_SetDataFormat, 1, fmt_build(2, 4, 1, one)), DIERR_INVALIDPARAM);
    one[0] = (struct fobj){ GUID_Button, 0, 0x00FFFF03u };
    CHECKEQ(vcall(dev, DD_SetDataFormat, 1, fmt_build(2, 4, 1, one)), DIERR_INVALIDPARAM);
    one[0] = (struct fobj){ GUID_ZAxis, 0, 0x00000203u };                   /* Z is axis instance 2 */
    CHECKEQ(vcall(dev, DD_SetDataFormat, 1, fmt_build(2, 4, 1, one)), DI_OK);
    /* offsets: past dwDataSize, an axis that does not fit, an unaligned axis; a size not a multiple of 4 */
    memcpy(o, g_game_objs, sizeof o);
    o[6].ofs = 16;
    CHECKEQ(vcall(dev, DD_SetDataFormat, 1, fmt_build(2, 16, 7, o)), DIERR_INVALIDPARAM);
    memcpy(o, g_game_objs, sizeof o);
    o[2].ofs = 14;
    CHECKEQ(vcall(dev, DD_SetDataFormat, 1, fmt_build(2, 20, 7, o)), DIERR_INVALIDPARAM);
    o[2].ofs = 16;
    CHECKEQ(vcall(dev, DD_SetDataFormat, 1, fmt_build(2, 16, 7, o)), DIERR_INVALIDPARAM);
    CHECKEQ(vcall(dev, DD_SetDataFormat, 1, fmt_build(2, 20, 7, o)), DI_OK);
    memcpy(o, g_game_objs, sizeof o);
    CHECKEQ(vcall(dev, DD_SetDataFormat, 1, fmt_build(2, 18, 7, o)), DIERR_INVALIDPARAM);
    CHECKEQ(vcall(dev, DD_SetDataFormat, 1, fmt_build(2, 0, 0, o)), DIERR_INVALIDPARAM);
    /* the axis-mode flags: absolute, relative, not both */
    CHECKEQ(vcall(dev, DD_SetDataFormat, 1, fmt_build(1, 16, 7, o)), DI_OK);
    CHECKEQ(vcall(dev, DD_SetDataFormat, 1, fmt_build(3, 16, 7, o)), DIERR_INVALIDPARAM);
    /* the same device object twice: the second is a fourth axis */
    o[1] = (struct fobj){ GUID_XAxis, 4, 0x00FFFF03u };
    CHECKEQ(vcall(dev, DD_SetDataFormat, 1, fmt_build(2, 16, 7, o)), DIERR_INVALIDPARAM);
    /* the axis mode property */
    uint32_t am = dword_prop(2);
    CHECKEQ(vcall(dev, DD_SetProperty, 2, DIPROP_AXISMODE, am), DIERR_INVALIDPARAM);
    CHECKEQ(vcall(dev, DD_SetProperty, 2, DIPROP_AXISMODE, dword_prop(0)), DI_OK);
    CHECKEQ(vcall(dev, DD_SetProperty, 2, DIPROP_AXISMODE, dword_prop(1)), DI_OK);
    /* the game's format again, acquired, then SetDataFormat and SetProperty are refused */
    CHECKEQ(vcall(dev, DD_SetDataFormat, 1, mouse_format(16)), DI_OK);
    CHECKEQ(vcall(dev, DD_SetCooperativeLevel, 2, HWND, 6u), DI_OK);
    CHECKEQ(vcall(dev, DD_Acquire, 0), DI_OK);
    CHECKEQ(vcall(dev, DD_SetProperty, 2, DIPROP_AXISMODE, dword_prop(0)), DIERR_ACQUIRED);
    CHECKEQ(vcall(dev, DD_SetDataFormat, 1, mouse_format(16)), DIERR_ACQUIRED);
    /* reads with no buffer size */
    uint32_t buf = gmem(20 * 4), n = 4;
    CHECKEQ(read_data(dev, 20, buf, &n, 0), DIERR_NOTBUFFERED);
    CHECKEQ(vcall(dev, DD_Unacquire, 0), DI_OK);
    /* a child window is not top-level */
    uint32_t child = call("CreateWindowExA", 12, 0u, gstr("Static"), gstr(""), 0x50000000u, 0u, 0u, 10u, 10u, HWND,
                          0u, HINST, 0u);
    CHECK(child != 0);
    CHECKEQ(vcall(dev, DD_SetCooperativeLevel, 2, child, 6u), E_HANDLE);
    CHECKEQ(vcall(dev, 2, 0), 0);
    CHECKEQ(vcall(di, 2, 0), 0);
}

static void group_di_data(void) {
    uint32_t di = new_di(), dev = game_mouse(di, 512);
    uint32_t buf = gmem(20 * 512), n;
    uint32_t t0 = 2000000u;
    memset(hle_ptr(buf), 0xAB, 20 * 512);
    ev_delta(3, -2, t0);
    ev_button_at(0, 1, 0, 0, t0 + 10);
    ev_wheel(-120, t0 + 20);
    n = 512;
    CHECKEQ(read_data(dev, 20, buf, &n, 0), DI_OK);
    CHECKEQ(n, 4);
    if (n == 4) {
        CHECKEQ(el(buf, 20, 0, EL_OFS), 0);
        CHECKEQ(el(buf, 20, 0, EL_DATA), 3);
        CHECKEQ(el(buf, 20, 1, EL_OFS), 4);
        CHECKEQ(el(buf, 20, 1, EL_DATA), (uint32_t)-2);
        CHECKEQ(el(buf, 20, 0, EL_SEQ), el(buf, 20, 1, EL_SEQ));
        CHECKEQ(el(buf, 20, 2, EL_OFS), 12);
        CHECKEQ(el(buf, 20, 2, EL_DATA), 0x80);
        CHECKEQ(el(buf, 20, 3, EL_OFS), 8);
        CHECKEQ(el(buf, 20, 3, EL_DATA), (uint32_t)-120);
        CHECK(el(buf, 20, 2, EL_SEQ) > el(buf, 20, 1, EL_SEQ));
        CHECK(el(buf, 20, 3, EL_SEQ) > el(buf, 20, 2, EL_SEQ));
        CHECKEQ(el(buf, 20, 0, EL_TIME), t0);
        CHECKEQ(el(buf, 20, 1, EL_TIME), t0);
        CHECKEQ(el(buf, 20, 2, EL_TIME), t0 + 10);
        CHECKEQ(el(buf, 20, 3, EL_TIME), t0 + 20);
        for (int i = 0; i < 4; i++) CHECKEQ(el(buf, 20, i, EL_APP), 0);
    }
    uint32_t last = el(buf, 20, 3, EL_SEQ);
    n = 512;
    CHECKEQ(read_data(dev, 20, buf, &n, 0), DI_OK);
    CHECKEQ(n, 0);
    /* bw2_in_push stamps the push time */
    uint32_t before = hle_ticks_ms();
    ev_button(0, 0, 0, 0);
    uint32_t after = hle_ticks_ms();
    /* DX3's 16-byte elements; a peek keeps the data; n caps the count */
    ev_delta(0, 5, t0 + 30);                                       /* only Y moved: one element */
    n = 512;
    CHECKEQ(read_data(dev, 16, buf, &n, DIGDD_PEEK), DI_OK);
    CHECKEQ(n, 2);
    memset(hle_ptr(buf), 0xAB, 20 * 512);
    n = 1;
    CHECKEQ(read_data(dev, 16, buf, &n, 0), DI_OK);
    CHECKEQ(n, 1);
    CHECKEQ(el(buf, 16, 0, EL_OFS), 12);
    CHECKEQ(el(buf, 16, 0, EL_DATA), 0);
    CHECK(el(buf, 16, 0, EL_TIME) >= before && el(buf, 16, 0, EL_TIME) <= after);
    CHECK(el(buf, 16, 0, EL_SEQ) > last);
    CHECKEQ(el(buf, 16, 1, EL_OFS), 0xABABABABu);                 /* nothing past n, and 16 bytes each */
    n = 512;
    CHECKEQ(read_data(dev, 16, buf, &n, 0), DI_OK);
    CHECKEQ(n, 1);
    CHECKEQ(el(buf, 16, 0, EL_OFS), 4);
    CHECKEQ(el(buf, 16, 0, EL_DATA), 5);
    /* a flush (NULL, INFINITE) gives the count; a NULL peek counts and keeps */
    ev_delta(1, 1, t0 + 40);
    ev_wheel(120, t0 + 50);
    n = INFINITE;
    CHECKEQ(read_data(dev, 20, 0, &n, DIGDD_PEEK), DI_OK);
    CHECKEQ(n, 3);
    n = INFINITE;
    CHECKEQ(read_data(dev, 20, 0, &n, 0), DI_OK);
    CHECKEQ(n, 3);
    n = 512;
    CHECKEQ(read_data(dev, 20, buf, &n, 0), DI_OK);
    CHECKEQ(n, 0);
    /* buttons 4 to 7 are not in the game's format; a second down of a held button is no change */
    ev_button(4, 1, 0, 0);
    ev_button(3, 1, 0, 0);
    ev_button(3, 1, 0, 0);
    n = 512;
    CHECKEQ(read_data(dev, 20, buf, &n, 0), DI_OK);
    CHECKEQ(n, 1);
    CHECKEQ(el(buf, 20, 0, EL_OFS), 15);
    /* Acquire empties the buffer */
    ev_delta(1, 0, t0 + 60);
    bw2_in_drain_input();
    CHECKEQ(vcall(dev, DD_Unacquire, 0), DI_OK);
    CHECKEQ(vcall(dev, DD_Acquire, 0), DI_OK);
    n = 512;
    CHECKEQ(read_data(dev, 20, buf, &n, 0), DI_OK);
    CHECKEQ(n, 0);
    /* unacquired, it records nothing */
    CHECKEQ(vcall(dev, DD_Unacquire, 0), DI_OK);
    ev_delta(1, 0, t0 + 70);
    bw2_in_drain_input();
    CHECKEQ(vcall(dev, DD_Acquire, 0), DI_OK);
    n = 512;
    CHECKEQ(read_data(dev, 20, buf, &n, 0), DI_OK);
    CHECKEQ(n, 0);
    CHECKEQ(vcall(dev, DD_Unacquire, 0), DI_OK);
    CHECKEQ(vcall(dev, 2, 0), 0);
    CHECKEQ(vcall(di, 2, 0), 0);
}

static int32_t st32(uint32_t st, int ofs) { return (int32_t)MEM32(st + (uint32_t)ofs); }
static uint8_t st8(uint32_t st, int ofs) { return ((uint8_t *)hle_ptr(st))[ofs]; }

static void group_di_state(void) {
    uint32_t di = new_di(), dev = game_mouse(di, 512), st = gmem(16);
    /* relative: the motion since the last GetDeviceState */
    ev_delta(3, 1, 1000u);
    ev_delta(4, 0, 1001u);
    CHECKEQ(vcall(dev, DD_GetDeviceState, 2, 16u, st), DI_OK);
    CHECKEQ(st32(st, 0), 7);
    CHECKEQ(st32(st, 4), 1);
    CHECKEQ(st32(st, 8), 0);
    memset(hle_ptr(st), 0xAB, 16);
    CHECKEQ(vcall(dev, DD_GetDeviceState, 2, 16u, st), DI_OK);
    CHECKEQ(st32(st, 0), 0);
    CHECKEQ(st32(st, 4), 0);
    for (int i = 12; i < 16; i++) CHECKEQ(st8(st, i), 0);
    /* a held button reads 0x80, read after read; the wheel is Z */
    ev_button(0, 1, 0, 0);
    ev_button(3, 1, 0, 0);
    ev_wheel(-240, 1002u);
    CHECKEQ(vcall(dev, DD_GetDeviceState, 2, 16u, st), DI_OK);
    CHECKEQ(st8(st, 12), 0x80);
    CHECKEQ(st8(st, 13), 0);
    CHECKEQ(st8(st, 15), 0x80);
    CHECKEQ(st32(st, 8), -240);
    CHECKEQ(vcall(dev, DD_GetDeviceState, 2, 16u, st), DI_OK);
    CHECKEQ(st8(st, 12), 0x80);
    CHECKEQ(st32(st, 8), 0);
    ev_button(0, 0, 0, 0);
    CHECKEQ(vcall(dev, DD_GetDeviceState, 2, 16u, st), DI_OK);
    CHECKEQ(st8(st, 12), 0);
    /* GetDeviceData does not take the motion GetDeviceState reports */
    uint32_t buf = gmem(20 * 16), n = INFINITE;
    CHECKEQ(read_data(dev, 20, 0, &n, 0), DI_OK);                  /* the flush: X, Y, X, 2 downs, Z, an up */
    CHECKEQ(n, 7);
    n = 16;
    ev_delta(2, 0, 1003u);
    CHECKEQ(read_data(dev, 20, buf, &n, 0), DI_OK);
    CHECKEQ(n, 1);
    CHECKEQ(vcall(dev, DD_GetDeviceState, 2, 16u, st), DI_OK);
    CHECKEQ(st32(st, 0), 2);
    /* cb must be the format's size */
    CHECKEQ(vcall(dev, DD_GetDeviceState, 2, 20u, st), DIERR_INVALIDPARAM);
    CHECKEQ(vcall(dev, DD_GetDeviceState, 2, 12u, st), DIERR_INVALIDPARAM);
    /* absolute (DIPROP_AXISMODE): the running sum, in the state and in the buffer */
    CHECKEQ(vcall(dev, DD_Unacquire, 0), DI_OK);
    CHECKEQ(vcall(dev, DD_SetProperty, 2, DIPROP_AXISMODE, dword_prop(0)), DI_OK);
    CHECKEQ(vcall(dev, DD_Acquire, 0), DI_OK);
    ev_delta(3, -1, 1004u);
    ev_delta(4, 0, 1005u);
    CHECKEQ(vcall(dev, DD_GetDeviceState, 2, 16u, st), DI_OK);
    CHECKEQ(st32(st, 0), 7);
    CHECKEQ(st32(st, 4), -1);
    CHECKEQ(vcall(dev, DD_GetDeviceState, 2, 16u, st), DI_OK);
    CHECKEQ(st32(st, 0), 7);
    n = 16;
    CHECKEQ(read_data(dev, 20, buf, &n, 0), DI_OK);
    CHECKEQ(n, 3);
    if (n == 3) {
        CHECKEQ(el(buf, 20, 0, EL_DATA), 3);
        CHECKEQ(el(buf, 20, 1, EL_DATA), (uint32_t)-1);
        CHECKEQ(el(buf, 20, 2, EL_OFS), 0);
        CHECKEQ(el(buf, 20, 2, EL_DATA), 7);
    }
    /* back to relative */
    CHECKEQ(vcall(dev, DD_Unacquire, 0), DI_OK);
    CHECKEQ(vcall(dev, DD_SetProperty, 2, DIPROP_AXISMODE, dword_prop(1)), DI_OK);
    CHECKEQ(vcall(dev, DD_Acquire, 0), DI_OK);
    ev_delta(5, 0, 1006u);
    CHECKEQ(vcall(dev, DD_GetDeviceState, 2, 16u, st), DI_OK);
    CHECKEQ(st32(st, 0), 5);
    /* DIDF_ABSAXIS in the format is absolute too */
    CHECKEQ(vcall(dev, DD_Unacquire, 0), DI_OK);
    struct fobj o[8];
    memcpy(o, g_game_objs, sizeof o);
    CHECKEQ(vcall(dev, DD_SetDataFormat, 1, fmt_build(1, 16, 7, o)), DI_OK);
    CHECKEQ(vcall(dev, DD_Acquire, 0), DI_OK);
    ev_delta(1, 0, 1007u);
    ev_delta(1, 0, 1008u);
    CHECKEQ(vcall(dev, DD_GetDeviceState, 2, 16u, st), DI_OK);
    CHECKEQ(st32(st, 0), 2);
    CHECKEQ(vcall(dev, DD_GetDeviceState, 2, 16u, st), DI_OK);
    CHECKEQ(st32(st, 0), 2);
    /* a 20-byte format (DIMOUSESTATE2): eight buttons */
    CHECKEQ(vcall(dev, DD_Unacquire, 0), DI_OK);
    struct fobj m2[11] = { { GUID_XAxis, 0, 0x00FFFF03u }, { GUID_YAxis, 4, 0x00FFFF03u }, { GUID_ZAxis, 8, 0x80FFFF03u } };
    for (int i = 0; i < 8; i++) m2[3 + i] = (struct fobj){ NULL, 12u + (uint32_t)i, 0x80FFFF0Cu };
    CHECKEQ(vcall(dev, DD_SetDataFormat, 1, fmt_build(2, 20, 11, m2)), DI_OK);
    CHECKEQ(vcall(dev, DD_Acquire, 0), DI_OK);
    uint32_t st2 = gmem(20);
    ev_button(4, 1, 0, 0);
    CHECKEQ(vcall(dev, DD_GetDeviceState, 2, 20u, st2), DI_OK);
    CHECKEQ(st8(st2, 16), 0x80);
    CHECKEQ(st8(st2, 15), 0);
    CHECKEQ(vcall(dev, DD_Unacquire, 0), DI_OK);
    CHECKEQ(vcall(dev, 2, 0), 0);
    CHECKEQ(vcall(di, 2, 0), 0);
}

static void group_di_overflow(void) {
    uint32_t di = new_di(), dev = game_mouse(di, 4);
    uint32_t buf = gmem(20 * 16), n;
    for (int i = 0; i < 6; i++) ev_button_at(0, i % 2 == 0, 0, 0, 3000u + (uint32_t)i);
    n = 16;
    CHECKEQ(read_data(dev, 20, buf, &n, 0), DI_BUFFEROVERFLOW);
    CHECKEQ(n, 4);
    uint32_t seq = 0;
    for (int i = 0; i < 4 && i < (int)n; i++) {
        CHECKEQ(el(buf, 20, i, EL_TIME), 3002u + (uint32_t)i);   /* the newest four */
        CHECKEQ(el(buf, 20, i, EL_DATA), i % 2 == 0 ? 0x80u : 0u);
        CHECK(el(buf, 20, i, EL_SEQ) > seq);
        seq = el(buf, 20, i, EL_SEQ);
    }
    n = 16;
    CHECKEQ(read_data(dev, 20, buf, &n, 0), DI_OK);
    CHECKEQ(n, 0);
    ev_button_at(0, 1, 0, 0, 3010u);
    n = 16;
    CHECKEQ(read_data(dev, 20, buf, &n, 0), DI_OK);
    CHECKEQ(n, 1);
    CHECK(el(buf, 20, 0, EL_SEQ) > seq);
    /* a peek after an overflow says so and keeps it for the read */
    for (int i = 0; i < 5; i++) ev_button_at(0, i % 2 != 0, 0, 0, 3020u + (uint32_t)i);
    n = 16;
    CHECKEQ(read_data(dev, 20, buf, &n, DIGDD_PEEK), DI_BUFFEROVERFLOW);
    CHECKEQ(n, 4);
    n = 16;
    CHECKEQ(read_data(dev, 20, buf, &n, 0), DI_BUFFEROVERFLOW);
    CHECKEQ(n, 4);
    n = 16;
    CHECKEQ(read_data(dev, 20, buf, &n, 0), DI_OK);
    /* a move is two elements; an overflow there drops the oldest element */
    ev_delta(1, 1, 3030u);
    ev_delta(2, 2, 3031u);
    ev_delta(3, 3, 3032u);
    n = 16;
    CHECKEQ(read_data(dev, 20, buf, &n, 0), DI_BUFFEROVERFLOW);
    CHECKEQ(n, 4);
    CHECKEQ(el(buf, 20, 0, EL_DATA), 2);
    CHECKEQ(el(buf, 20, 3, EL_DATA), 3);
    CHECKEQ(vcall(dev, DD_Unacquire, 0), DI_OK);
    CHECKEQ(vcall(dev, 2, 0), 0);
    CHECKEQ(vcall(di, 2, 0), 0);
}

static void group_focus_loss(void) {
    hle_proc_hook = recorder;
    bw2_in_set_window(HWND);
    pump_all();
    uint32_t di = new_di(), dev = game_mouse(di, 512);
    /* a non-exclusive background mouse is not lost */
    uint32_t bg = new_mouse(di);
    CHECKEQ(vcall(bg, DD_SetDataFormat, 1, mouse_format(16)), DI_OK);
    CHECKEQ(vcall(bg, DD_SetCooperativeLevel, 2, HWND, 10u), DI_OK);
    CHECKEQ(vcall(bg, DD_SetProperty, 2, DIPROP_BUFFERSIZE, bufsize_prop(64)), DI_OK);
    CHECKEQ(vcall(bg, DD_Acquire, 0), DI_OK);
    uint32_t buf = gmem(20 * 512), n, st = gmem(16);
    /* the left button held */
    ev_button(0, 1, 0, 0);
    n = 512;
    CHECKEQ(read_data(dev, 20, buf, &n, 0), DI_OK);
    CHECKEQ(n, 1);
    /* deactivated with no pump between: the read says lost, and no window procedure runs in it */
    rec_reset();
    ev_activate(0);
    n = 512;
    CHECKEQ(read_data(dev, 20, buf, &n, 0), DIERR_INPUTLOST);
    CHECK(!rec_has(WM_ACTIVATE) && !rec_has(WM_KILLFOCUS) && !rec_has(WM_ACTIVATEAPP));
    CHECKEQ(bw2_in_losses(), 1);
    n = 512;
    CHECKEQ(read_data(dev, 20, buf, &n, 0), DIERR_NOTACQUIRED);
    CHECKEQ(vcall(dev, DD_GetDeviceState, 2, 16u, st), DIERR_NOTACQUIRED);
    CHECKEQ(vcall(dev, DD_Acquire, 0), DIERR_OTHERAPPHASPRIO);
    CHECKEQ(vcall(dev, DD_Unacquire, 0), DI_NOEFFECT);
    n = 64;
    CHECKEQ(read_data(bg, 20, buf, &n, 0), DI_OK);
    CHECKEQ(n, 1);                                                /* the button, before the loss */
    /* the pump delivers the deactivation; still no Acquire */
    pump_all();
    CHECK(rec_has(WM_ACTIVATE) && rec_has(WM_KILLFOCUS) && rec_has(WM_ACTIVATEAPP));
    CHECKEQ(vcall(dev, DD_Acquire, 0), DIERR_OTHERAPPHASPRIO);
    CHECKEQ(bw2_in_mouse_exclusive(), 0);
    /* back: Acquire works, and the state is empty */
    ev_activate(1);
    pump_all();
    CHECKEQ(vcall(dev, DD_Acquire, 0), DI_OK);
    CHECKEQ(bw2_in_mouse_exclusive(), 1);
    CHECKEQ(vcall(dev, DD_GetDeviceState, 2, 16u, st), DI_OK);
    CHECKEQ(st8(st, 12), 0);
    /* the game's retry loop: read; on lost or not acquired, Acquire and read again */
    ev_button(0, 0, 0, 0);
    ev_activate(0);
    pump_all();
    ev_activate(1);
    pump_all();
    CHECKEQ(bw2_in_losses(), 2);
    uint32_t hr = 0, tries = 0;
    for (; tries < 4; tries++) {
        n = 512;
        hr = read_data(dev, 20, buf, &n, 0);
        if (hr != DIERR_INPUTLOST && hr != DIERR_NOTACQUIRED) break;
        CHECKEQ(vcall(dev, DD_Acquire, 0), DI_OK);
    }
    CHECKEQ(hr, DI_OK);
    CHECKEQ(tries, 1);
    CHECKEQ(n, 0);
    /* lost and then given back before the read: still lost once */
    ev_activate(0);
    ev_activate(1);
    n = 512;
    CHECKEQ(read_data(dev, 20, buf, &n, 0), DIERR_INPUTLOST);
    pump_all();
    CHECKEQ(vcall(dev, DD_Acquire, 0), DI_OK);
    n = 512;
    CHECKEQ(read_data(dev, 20, buf, &n, 0), DI_OK);
    CHECKEQ(vcall(dev, DD_Unacquire, 0), DI_OK);
    CHECKEQ(vcall(bg, DD_Unacquire, 0), DI_OK);
    CHECKEQ(vcall(bg, 2, 0), 0);
    CHECKEQ(vcall(dev, 2, 0), 0);
    CHECKEQ(vcall(di, 2, 0), 0);
    hle_proc_hook = NULL;
}

static void group_di_exclusive(void) {
    hle_proc_hook = recorder;
    bw2_in_set_window(HWND);
    pump_all();
    uint32_t di = new_di(), dev = game_mouse(di, 512), pt = gmem(8), buf = gmem(20 * 512), n;
    struct bw2_in_event e = { .kind = BW2_IN_MOVE, .x = 10, .y = 20, .dx = 1, .dy = 1 };
    /* the cursor somewhere first, through a non-exclusive phase */
    CHECKEQ(vcall(dev, DD_Unacquire, 0), DI_OK);
    CHECKEQ(bw2_in_mouse_exclusive(), 0);
    bw2_in_push(&e);
    pump_all();
    CHECKEQ(call("GetCursorPos", 1, pt), 1);
    CHECKEQ(MEM32(pt), 60);
    CHECKEQ(MEM32(pt + 4), 70);
    /* exclusive (5), acquired: DirectInput only */
    CHECKEQ(vcall(dev, DD_Acquire, 0), DI_OK);
    CHECKEQ(bw2_in_mouse_exclusive(), 1);
    rec_reset();
    e.x = 30, e.y = 40;
    bw2_in_push(&e);
    pump_all();
    CHECK(!rec_has(WM_MOUSEMOVE));
    CHECKEQ(call("GetCursorPos", 1, pt), 1);
    CHECKEQ(MEM32(pt), 60);
    CHECKEQ(MEM32(pt + 4), 70);
    n = 512;
    CHECKEQ(read_data(dev, 20, buf, &n, 0), DI_OK);
    CHECKEQ(n, 2);
    /* non-exclusive (6, the setup's): both */
    CHECKEQ(vcall(dev, DD_Unacquire, 0), DI_OK);
    CHECKEQ(vcall(dev, DD_SetCooperativeLevel, 2, HWND, 6u), DI_OK);
    CHECKEQ(vcall(dev, DD_Acquire, 0), DI_OK);
    CHECKEQ(bw2_in_mouse_exclusive(), 0);
    rec_reset();
    bw2_in_push(&e);
    pump_all();
    EXPECT(1, WM_MOUSEMOVE, ANY, (40u << 16) | 30u);
    CHECKEQ(call("GetCursorPos", 1, pt), 1);
    CHECKEQ(MEM32(pt), 80);
    n = 512;
    CHECKEQ(read_data(dev, 20, buf, &n, 0), DI_OK);
    CHECKEQ(n, 2);
    /* exclusive again, then Unacquire: a message again */
    CHECKEQ(vcall(dev, DD_Unacquire, 0), DI_OK);
    CHECKEQ(vcall(dev, DD_SetCooperativeLevel, 2, HWND, 5u), DI_OK);
    CHECKEQ(vcall(dev, DD_Acquire, 0), DI_OK);
    CHECKEQ(bw2_in_mouse_exclusive(), 1);
    CHECKEQ(vcall(dev, DD_Unacquire, 0), DI_OK);
    CHECKEQ(bw2_in_mouse_exclusive(), 0);
    rec_reset();
    e.x = 1, e.y = 2;
    bw2_in_push(&e);
    pump_all();
    EXPECT(1, WM_MOUSEMOVE, ANY, (2u << 16) | 1u);
    /* released with the device: no exclusive mouse left */
    CHECKEQ(vcall(dev, DD_Acquire, 0), DI_OK);
    CHECKEQ(bw2_in_mouse_exclusive(), 1);
    CHECKEQ(vcall(dev, 2, 0), 0);
    CHECKEQ(bw2_in_mouse_exclusive(), 0);
    CHECKEQ(vcall(di, 2, 0), 0);
    hle_proc_hook = NULL;
}

/* The game's IMM32 sequence (plan 6 "Measured" item 7), then the other four. */
static void group_imm(void) {
    uint32_t hwnd = make_window(0, 0, 0, 640, 480);
    call("ImmAssociateContext", 2, hwnd, 0u);
    call("ImmAssociateContext", 2, hwnd, 0u);
    call("ImmGetContext", 1, hwnd);
    call("ImmGetContext", 1, hwnd);
    uint32_t c1 = call("ImmCreateContext", 0);
    uint32_t c2 = call("ImmCreateContext", 0);
    CHECK(c1 != 0 && c1 != 0xDEADBEEFu);
    CHECK(c2 != 0 && c2 != 0xDEADBEEFu);
    CHECK(c1 != c2);
    CHECKEQ(call("ImmGetProperty", 2, 0x04090409u, 4u), 0);
    CHECKEQ(call("ImmGetProperty", 2, 0x04090409u, 8u), 0);
    CHECKEQ(call("ImmGetProperty", 2, 0x04090409u, 0x18u), 0);
    uint32_t buf = gmem(0x100);
    memset(hle_ptr(buf), 0x5A, 0x100);
    CHECKEQ(call("ImmGetDescriptionA", 3, 0x04090409u, buf, 0x100u), 0);
    int same = 1;
    for (int i = 0; i < 0x100; i++) if (((uint8_t *)hle_ptr(buf))[i] != 0x5A) same = 0;
    CHECK(same);
    CHECK(call("ImmAssociateContext", 2, hwnd, c1) != 0xDEADBEEFu);
    CHECKEQ(call("ImmGetOpenStatus", 1, c1), 0);
    CHECKEQ(call("ImmSetCompositionStringA", 6, c1, 9u, 0u, 0u, 0u, 0u), 0);
    uint32_t cf = gmem(28);
    CHECKEQ(call("ImmSetCompositionWindow", 2, c1, cf), 1);
    CHECKEQ(call("ImmDestroyContext", 1, c1), 1);
    CHECKEQ(call("ImmDestroyContext", 1, c1), 0);
    CHECKEQ(call("ImmDestroyContext", 1, 0x1234u), 0);
    CHECKEQ(call("ImmDestroyContext", 1, c2), 1);
}

/* ---- plan 6 Task 3: Mac events in the game's terms (translate.c, pure) ---- */
#define MAC(...) (&(struct bw2_mac_ev){ __VA_ARGS__ })
static struct bw2_in_event g_tr[8];
static int tr(struct bw2_translator *t, const struct bw2_geom *g, const struct bw2_mac_ev *m) {
    memset(g_tr, 0xA5, sizeof g_tr);
    int n = bw2_translate(t, g, m, g_tr, 4);
    CHECK(n >= 0 && n <= 4);
    return n;
}
#define EXPECT_KEY(e, dn, v, s, x) do { CHECKEQ((e).kind, BW2_IN_KEY); CHECKEQ((e).down, dn); \
    CHECKEQ((e).vk, v); CHECKEQ((e).scan, s); CHECKEQ((e).extended, x); } while (0)
#define EXPECT_BUTTON(e, b, dn, px, py) do { CHECKEQ((e).kind, BW2_IN_BUTTON); CHECKEQ((e).button, b); \
    CHECKEQ((e).down, dn); CHECKEQ((e).x, px); CHECKEQ((e).y, py); } while (0)
#define EXPECT_MOVE(e, px, py) do { CHECKEQ((e).kind, BW2_IN_MOVE); CHECKEQ((e).x, px); CHECKEQ((e).y, py); } while (0)
#define NEAR(a, b) (fabs((double)(a) - (double)(b)) < 1e-9)

static int g_mapped_seen[128];
static void expect_map(int line, unsigned mac, uint32_t vk, uint32_t scan, int ext) {
    uint32_t v = 0xDEAD, s = 0xDEAD;
    int x = -1;
    int ok = bw2_keymap(mac, &v, &s, &x);
    if (!ok || v != vk || s != scan || x != ext) {
        printf("FAIL keymap line %d: mac 0x%02X gave %d vk 0x%X scan 0x%X ext %d, want vk 0x%X scan 0x%X ext %d\n",
               line, mac, ok, v, s, x, vk, scan, ext);
        fails++;
    }
    if (mac < 128) {
        if (g_mapped_seen[mac]) { printf("FAIL keymap line %d: mac 0x%02X listed twice\n", line, mac); fails++; }
        g_mapped_seen[mac] = 1;
    }
}
#define MAP(mac, vk, scan, ext) expect_map(__LINE__, mac, vk, scan, ext)

/* "X1E Y2F ..." -> the hex value after letter c, or -1 */
static int find_code(const char *list, char c) {
    for (const char *p = list; *p; p++)
        if (*p == c && (p == list || p[-1] == ' ')) return (int)strtol(p + 1, NULL, 16);
    return -1;
}

static void group_keymap(void) {
    static const char scans[] = "Q10 W11 E12 R13 T14 Y15 U16 I17 O18 P19 A1E S1F D20 F21 G22 H23 J24 K25 L26 "
                                "Z2C X2D C2E V2F B30 N31 M32";
    static const char macs[] = "A00 S01 D02 F03 H04 G05 Z06 X07 C08 V09 B0B Q0C W0D E0E R0F Y10 T11 O1F U20 "
                               "I22 P23 L25 J26 K28 N2D M2E";
    for (char c = 'A'; c <= 'Z'; c++) {
        int mac = find_code(macs, c), scan = find_code(scans, c);
        CHECK(mac >= 0 && scan >= 0);
        if (mac >= 0 && scan >= 0) MAP((unsigned)mac, (uint32_t)c, (uint32_t)scan, 0);
    }
    static const unsigned digit_mac[10] = { 0x1D, 0x12, 0x13, 0x14, 0x15, 0x17, 0x16, 0x1A, 0x1C, 0x19 };
    for (int d = 0; d < 10; d++) MAP(digit_mac[d], '0' + (uint32_t)d, d ? 1u + (uint32_t)d : 0x0Bu, 0);
    /* punctuation */
    MAP(0x1B, 0xBD, 0x0C, 0); MAP(0x18, 0xBB, 0x0D, 0); MAP(0x21, 0xDB, 0x1A, 0); MAP(0x1E, 0xDD, 0x1B, 0);
    MAP(0x29, 0xBA, 0x27, 0); MAP(0x27, 0xDE, 0x28, 0); MAP(0x32, 0xC0, 0x29, 0); MAP(0x2A, 0xDC, 0x2B, 0);
    MAP(0x2B, 0xBC, 0x33, 0); MAP(0x2F, 0xBE, 0x34, 0); MAP(0x2C, 0xBF, 0x35, 0); MAP(0x0A, 0xE2, 0x56, 0);
    /* Return, Tab, Space, Delete, Escape */
    MAP(0x24, 0x0D, 0x1C, 0); MAP(0x30, 0x09, 0x0F, 0); MAP(0x31, 0x20, 0x39, 0); MAP(0x33, 0x08, 0x0E, 0);
    MAP(0x35, 0x1B, 0x01, 0);
    /* modifiers */
    MAP(0x38, 0xA0, 0x2A, 0); MAP(0x3C, 0xA1, 0x36, 0); MAP(0x3B, 0xA2, 0x1D, 0); MAP(0x3E, 0xA3, 0x1D, 1);
    MAP(0x3A, 0xA4, 0x38, 0); MAP(0x3D, 0xA5, 0x38, 1); MAP(0x39, 0x14, 0x3A, 0);
    /* function keys */
    static const unsigned f_mac[10] = { 0x7A, 0x78, 0x63, 0x76, 0x60, 0x61, 0x62, 0x64, 0x65, 0x6D };
    for (int i = 0; i < 10; i++) MAP(f_mac[i], 0x70u + (uint32_t)i, 0x3Bu + (uint32_t)i, 0);
    MAP(0x67, 0x7A, 0x57, 0); MAP(0x6F, 0x7B, 0x58, 0);
    MAP(0x69, 0x7C, 0x64, 0); MAP(0x6B, 0x7D, 0x65, 0); MAP(0x71, 0x7E, 0x66, 0);
    /* keypad */
    static const unsigned kp_mac[10] = { 0x52, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58, 0x59, 0x5B, 0x5C };
    static const uint32_t kp_scan[10] = { 0x52, 0x4F, 0x50, 0x51, 0x4B, 0x4C, 0x4D, 0x47, 0x48, 0x49 };
    for (int i = 0; i < 10; i++) MAP(kp_mac[i], 0x60u + (uint32_t)i, kp_scan[i], 0);
    MAP(0x41, 0x6E, 0x53, 0); MAP(0x43, 0x6A, 0x37, 0); MAP(0x45, 0x6B, 0x4E, 0); MAP(0x4E, 0x6D, 0x4A, 0);
    MAP(0x4B, 0x6F, 0x35, 1); MAP(0x4C, 0x0D, 0x1C, 1);
    /* navigation, all extended */
    MAP(0x73, 0x24, 0x47, 1); MAP(0x77, 0x23, 0x4F, 1); MAP(0x74, 0x21, 0x49, 1); MAP(0x79, 0x22, 0x51, 1);
    MAP(0x75, 0x2E, 0x53, 1); MAP(0x72, 0x2D, 0x52, 1);
    MAP(0x7B, 0x25, 0x4B, 1); MAP(0x7C, 0x27, 0x4D, 1); MAP(0x7D, 0x28, 0x50, 1); MAP(0x7E, 0x26, 0x48, 1);
    int listed = 0;
    for (int i = 0; i < 128; i++) listed += g_mapped_seen[i];
    CHECKEQ(listed, 101);
    /* not sent: Command (both), Function, keypad Clear, keypad =, F16-F20 */
    static const unsigned not_sent[] = { 0x37, 0x36, 0x3F, 0x47, 0x51, 0x6A, 0x40, 0x4F, 0x50, 0x5A };
    for (size_t i = 0; i < sizeof not_sent / sizeof not_sent[0]; i++) {
        uint32_t v, s;
        int x;
        CHECK(!g_mapped_seen[not_sent[i]]);
        CHECKEQ(bw2_keymap(not_sent[i], &v, &s, &x), 0);
    }
    /* and nothing else is mapped */
    int extra = 0;
    for (unsigned k = 0; k < 0x200; k++) {
        uint32_t v, s;
        int x;
        if ((k >= 128 || !g_mapped_seen[k]) && bw2_keymap(k, &v, &s, &x)) {
            printf("FAIL keymap: unlisted mac 0x%X is mapped\n", k);
            extra++;
        }
    }
    CHECKEQ(extra, 0);
    CHECKEQ(bw2_keymap(0xFFFFFFFFu, &(uint32_t){ 0 }, &(uint32_t){ 0 }, &(int){ 0 }), 0);
}

static void group_geom_letterbox(void) {
    struct bw2_geom g = { 1512, 982, 1024, 768 };
    double x0 = -1, y0 = -1, s = -1;
    bw2_letterbox(&g, &x0, &y0, &s);
    CHECK(NEAR(s, 982.0 / 768.0));
    CHECK(s > 1.27864 && s < 1.27865);
    CHECK(NEAR(x0, (1512.0 - 1024.0 * s) / 2));
    CHECK(x0 > 101.33 && x0 < 101.34);
    CHECK(NEAR(y0, 0));
    struct bw2_translator t = { 0 };
    int n;
    double right = x0 + 1024.0 * s;
    n = tr(&t, &g, MAC(.type = BW2_MAC_MOVE, .x = x0, .y = 982)); CHECKEQ(n, 1); EXPECT_MOVE(g_tr[0], 0, 0);
    n = tr(&t, &g, MAC(.type = BW2_MAC_MOVE, .x = right, .y = 0)); CHECKEQ(n, 1); EXPECT_MOVE(g_tr[0], 1023, 767);
    n = tr(&t, &g, MAC(.type = BW2_MAC_MOVE, .x = right - 0.01, .y = 0.01)); CHECKEQ(n, 1);
    EXPECT_MOVE(g_tr[0], 1023, 767);
    n = tr(&t, &g, MAC(.type = BW2_MAC_MOVE, .x = 0, .y = 490)); CHECKEQ(n, 1); EXPECT_MOVE(g_tr[0], 0, 384);
    n = tr(&t, &g, MAC(.type = BW2_MAC_MOVE, .x = 1512, .y = 982)); CHECKEQ(n, 1); EXPECT_MOVE(g_tr[0], 1023, 0);
    n = tr(&t, &g, MAC(.type = BW2_MAC_MOVE, .x = -50, .y = -50)); CHECKEQ(n, 1); EXPECT_MOVE(g_tr[0], 0, 767);
    n = tr(&t, &g, MAC(.type = BW2_MAC_MOVE, .x = x0 + 512 * s + 0.25, .y = 982 - 384 * s - 0.25)); CHECKEQ(n, 1);
    EXPECT_MOVE(g_tr[0], 512, 384);
    /* a view the back buffer's size: client = point, y flipped */
    struct bw2_geom g1 = { 1024, 768, 1024, 768 };
    bw2_letterbox(&g1, &x0, &y0, &s);
    CHECK(NEAR(s, 1) && NEAR(x0, 0) && NEAR(y0, 0));
    n = tr(&t, &g1, MAC(.type = BW2_MAC_MOVE, .x = 100.5, .y = 700.25)); CHECKEQ(n, 1); EXPECT_MOVE(g_tr[0], 100, 67);
    n = tr(&t, &g1, MAC(.type = BW2_MAC_MOVE, .x = 0, .y = 768)); CHECKEQ(n, 1); EXPECT_MOVE(g_tr[0], 0, 0);
    n = tr(&t, &g1, MAC(.type = BW2_MAC_MOVE, .x = 1023.9, .y = 0.1)); CHECKEQ(n, 1); EXPECT_MOVE(g_tr[0], 1023, 767);
    /* bars above and below */
    struct bw2_geom g2 = { 1024, 1000, 1024, 768 };
    bw2_letterbox(&g2, &x0, &y0, &s);
    CHECK(NEAR(s, 1) && NEAR(x0, 0) && NEAR(y0, 116));
    n = tr(&t, &g2, MAC(.type = BW2_MAC_MOVE, .x = 10, .y = 1000 - 116)); CHECKEQ(n, 1); EXPECT_MOVE(g_tr[0], 10, 0);
    n = tr(&t, &g2, MAC(.type = BW2_MAC_MOVE, .x = 10, .y = 990)); CHECKEQ(n, 1); EXPECT_MOVE(g_tr[0], 10, 0);
    n = tr(&t, &g2, MAC(.type = BW2_MAC_MOVE, .x = 10, .y = 116.5)); CHECKEQ(n, 1); EXPECT_MOVE(g_tr[0], 10, 767);
    n = tr(&t, &g2, MAC(.type = BW2_MAC_MOVE, .x = 10, .y = 5)); CHECKEQ(n, 1); EXPECT_MOVE(g_tr[0], 10, 767);
    /* a retina-sized view of a small buffer: 2 points a pixel */
    struct bw2_geom g3 = { 1280, 960, 640, 480 };
    bw2_letterbox(&g3, &x0, &y0, &s);
    CHECK(NEAR(s, 2) && NEAR(x0, 0) && NEAR(y0, 0));
    n = tr(&t, &g3, MAC(.type = BW2_MAC_MOVE, .x = 641, .y = 479)); CHECKEQ(n, 1); EXPECT_MOVE(g_tr[0], 320, 240);
}

static void group_deltas(void) {
    struct bw2_geom g = { 2048, 1536, 1024, 768 };   /* 2 points a pixel */
    struct bw2_translator t = { 0 };
    int n;
    n = tr(&t, &g, MAC(.type = BW2_MAC_MOVE, .x = 100, .y = 100, .dx = 1.0)); CHECKEQ(n, 1);
    CHECKEQ(g_tr[0].kind, BW2_IN_MOVE); CHECKEQ(g_tr[0].dx, 0); CHECKEQ(g_tr[0].dy, 0);
    n = tr(&t, &g, MAC(.type = BW2_MAC_MOVE, .x = 100, .y = 100, .dx = 1.0)); CHECKEQ(n, 1);
    CHECKEQ(g_tr[0].dx, 1); CHECKEQ(g_tr[0].dy, 0);
    /* down positive in AppKit and in DirectInput */
    n = tr(&t, &g, MAC(.type = BW2_MAC_MOVE, .x = 100, .y = 100, .dy = 4.0)); CHECKEQ(n, 1);
    CHECKEQ(g_tr[0].dx, 0); CHECKEQ(g_tr[0].dy, 2);
    n = tr(&t, &g, MAC(.type = BW2_MAC_MOVE, .x = 100, .y = 100, .dy = -6.0)); CHECKEQ(n, 1);
    CHECKEQ((int32_t)g_tr[0].dy, -3);
    /* negative fractions carry too */
    n = tr(&t, &g, MAC(.type = BW2_MAC_MOVE, .x = 100, .y = 100, .dx = -1.0)); CHECKEQ(n, 1);
    CHECKEQ(g_tr[0].dx, 0);
    n = tr(&t, &g, MAC(.type = BW2_MAC_MOVE, .x = 100, .y = 100, .dx = -1.0)); CHECKEQ(n, 1);
    CHECKEQ((int32_t)g_tr[0].dx, -1);
    /* a large one, and many small ones adding up */
    n = tr(&t, &g, MAC(.type = BW2_MAC_MOVE, .x = 100, .y = 100, .dx = 1024.0, .dy = 600.0)); CHECKEQ(n, 1);
    CHECKEQ(g_tr[0].dx, 512); CHECKEQ(g_tr[0].dy, 300);
    int sum = 0;
    for (int i = 0; i < 100; i++) {
        n = tr(&t, &g, MAC(.type = BW2_MAC_MOVE, .x = 100, .y = 100, .dx = 0.25));
        CHECKEQ(n, 1);
        sum += g_tr[0].dx;
    }
    CHECKEQ(sum, 12);                    /* 25 points, half a pixel left over */
    /* the point and the delta are independent (a detached pointer keeps its point) */
    n = tr(&t, &g, MAC(.type = BW2_MAC_MOVE, .x = 1000, .y = 1000, .dx = 3.0)); CHECKEQ(n, 1);
    EXPECT_MOVE(g_tr[0], 500, 268); CHECKEQ(g_tr[0].dx, 2);
    /* at the 1512x982 letterbox, 1.27864 points a pixel */
    struct bw2_geom gl = { 1512, 982, 1024, 768 };
    struct bw2_translator t2 = { 0 };
    n = tr(&t2, &gl, MAC(.type = BW2_MAC_MOVE, .x = 700, .y = 500, .dx = 982.0, .dy = -982.0)); CHECKEQ(n, 1);
    CHECKEQ(g_tr[0].dx, 768); CHECKEQ((int32_t)g_tr[0].dy, -768);
}

static void group_scroll_precise(void) {
    struct bw2_geom g = { 1024, 768, 1024, 768 };
    struct bw2_translator t = { 0 };
    int n;
    for (int i = 0; i < 3; i++) {
        n = tr(&t, &g, MAC(.type = BW2_MAC_SCROLL, .x = 10, .y = 758, .precise = 1, .scroll_dy = 3));
        CHECKEQ(n, 0);
    }
    n = tr(&t, &g, MAC(.type = BW2_MAC_SCROLL, .x = 10, .y = 758, .precise = 1, .scroll_dy = 3));
    CHECKEQ(n, 1);
    CHECKEQ(g_tr[0].kind, BW2_IN_WHEEL); CHECKEQ(g_tr[0].wheel, 120); CHECKEQ(g_tr[0].x, 10); CHECKEQ(g_tr[0].y, 10);
    CHECK(NEAR(t.scroll_rem, 2));
    /* momentum: nothing, remainder kept */
    n = tr(&t, &g, MAC(.type = BW2_MAC_SCROLL, .precise = 1, .momentum = 1, .scroll_dy = 50)); CHECKEQ(n, 0);
    n = tr(&t, &g, MAC(.type = BW2_MAC_SCROLL, .precise = 1, .momentum = 1, .scroll_dy = -50)); CHECKEQ(n, 0);
    CHECK(NEAR(t.scroll_rem, 2));
    n = tr(&t, &g, MAC(.type = BW2_MAC_SCROLL, .x = 10, .y = 758, .precise = 1, .scroll_dy = -25)); CHECKEQ(n, 1);
    CHECKEQ(g_tr[0].kind, BW2_IN_WHEEL); CHECKEQ((int32_t)g_tr[0].wheel, -240);
    CHECK(NEAR(t.scroll_rem, -3));
    /* a wheel mouse: 120 a line; the precise remainder is left alone */
    n = tr(&t, &g, MAC(.type = BW2_MAC_SCROLL, .x = 10, .y = 758, .scroll_dy = 1.0)); CHECKEQ(n, 1);
    CHECKEQ(g_tr[0].kind, BW2_IN_WHEEL); CHECKEQ(g_tr[0].wheel, 120);
    CHECK(NEAR(t.scroll_rem, -3));
    n = tr(&t, &g, MAC(.type = BW2_MAC_SCROLL, .x = 10, .y = 758, .scroll_dy = -3.0)); CHECKEQ(n, 1);
    CHECKEQ((int32_t)g_tr[0].wheel, -360);
    /* a fractional line is still one notch (each wheel event is a detent) */
    n = tr(&t, &g, MAC(.type = BW2_MAC_SCROLL, .x = 10, .y = 758, .scroll_dy = 0.3)); CHECKEQ(n, 1);
    CHECKEQ(g_tr[0].wheel, 120);
    n = tr(&t, &g, MAC(.type = BW2_MAC_SCROLL, .x = 10, .y = 758, .scroll_dy = -0.1)); CHECKEQ(n, 1);
    CHECKEQ((int32_t)g_tr[0].wheel, -120);
    n = tr(&t, &g, MAC(.type = BW2_MAC_SCROLL, .x = 10, .y = 758, .scroll_dy = 2.6)); CHECKEQ(n, 1);
    CHECKEQ(g_tr[0].wheel, 360);
    /* no vertical scroll: nothing */
    n = tr(&t, &g, MAC(.type = BW2_MAC_SCROLL, .x = 10, .y = 758, .scroll_dy = 0)); CHECKEQ(n, 0);
    n = tr(&t, &g, MAC(.type = BW2_MAC_SCROLL, .x = 10, .y = 758, .precise = 1, .scroll_dy = 0)); CHECKEQ(n, 0);
    /* the remainder reaches a notch from -3: -7 more */
    n = tr(&t, &g, MAC(.type = BW2_MAC_SCROLL, .x = 10, .y = 758, .precise = 1, .scroll_dy = -6.5)); CHECKEQ(n, 0);
    n = tr(&t, &g, MAC(.type = BW2_MAC_SCROLL, .x = 10, .y = 758, .precise = 1, .scroll_dy = -0.5)); CHECKEQ(n, 1);
    CHECKEQ((int32_t)g_tr[0].wheel, -120);
    CHECK(NEAR(t.scroll_rem, 0));
}

static void group_option_middle(void) {
    struct bw2_geom g = { 1024, 768, 1024, 768 };
    struct bw2_translator t = { 0 };
    const uint64_t OPT = BW2_MOD_OPTION;
    int n;
    n = tr(&t, &g, MAC(.type = BW2_MAC_MOVE, .x = 100, .y = 668)); CHECKEQ(n, 1); EXPECT_MOVE(g_tr[0], 100, 100);
    /* Option down: the game sees Alt down */
    n = tr(&t, &g, MAC(.type = BW2_MAC_FLAGS, .keycode = 0x3A, .mods = OPT)); CHECKEQ(n, 1);
    EXPECT_KEY(g_tr[0], 1, 0xA4, 0x38, 0);
    /* Option + left: Alt up, then the middle button */
    n = tr(&t, &g, MAC(.type = BW2_MAC_DOWN, .x = 100, .y = 668, .button = 0, .mods = OPT)); CHECKEQ(n, 2);
    EXPECT_KEY(g_tr[0], 0, 0xA4, 0x38, 0);
    EXPECT_BUTTON(g_tr[1], 2, 1, 100, 100);
    /* Option released first: dropped */
    n = tr(&t, &g, MAC(.type = BW2_MAC_FLAGS, .keycode = 0x3A, .mods = 0)); CHECKEQ(n, 0);
    /* the left up ends the middle, whatever the modifiers */
    n = tr(&t, &g, MAC(.type = BW2_MAC_UP, .x = 100, .y = 668, .button = 0)); CHECKEQ(n, 1);
    EXPECT_BUTTON(g_tr[0], 2, 0, 100, 100);
    /* a later plain left is the left */
    n = tr(&t, &g, MAC(.type = BW2_MAC_DOWN, .x = 100, .y = 668, .button = 0)); CHECKEQ(n, 1);
    EXPECT_BUTTON(g_tr[0], 0, 1, 100, 100);
    n = tr(&t, &g, MAC(.type = BW2_MAC_UP, .x = 100, .y = 668, .button = 0)); CHECKEQ(n, 1);
    EXPECT_BUTTON(g_tr[0], 0, 0, 100, 100);
    /* and Option is no longer held back */
    n = tr(&t, &g, MAC(.type = BW2_MAC_FLAGS, .keycode = 0x3A, .mods = OPT)); CHECKEQ(n, 1);
    EXPECT_KEY(g_tr[0], 1, 0xA4, 0x38, 0);

    /* Option released after the button: the middle goes up at the left up, Option's release is dropped */
    n = tr(&t, &g, MAC(.type = BW2_MAC_DOWN, .x = 200, .y = 568, .button = 0, .mods = OPT)); CHECKEQ(n, 3);
    EXPECT_MOVE(g_tr[0], 200, 200);
    EXPECT_KEY(g_tr[1], 0, 0xA4, 0x38, 0);
    EXPECT_BUTTON(g_tr[2], 2, 1, 200, 200);
    n = tr(&t, &g, MAC(.type = BW2_MAC_MOVE, .x = 210, .y = 568, .mods = OPT)); CHECKEQ(n, 1);
    EXPECT_MOVE(g_tr[0], 210, 200);
    n = tr(&t, &g, MAC(.type = BW2_MAC_UP, .x = 210, .y = 568, .button = 0, .mods = OPT)); CHECKEQ(n, 1);
    EXPECT_BUTTON(g_tr[0], 2, 0, 210, 200);
    n = tr(&t, &g, MAC(.type = BW2_MAC_FLAGS, .keycode = 0x3A, .mods = 0)); CHECKEQ(n, 0);
    CHECKEQ(t.keys[0x3A], 0); CHECKEQ(t.option_held, 0);

    /* the right Option, with the device bits macOS adds (NX_DEVICERALTKEYMASK 0x40, left 0x20) */
    n = tr(&t, &g, MAC(.type = BW2_MAC_FLAGS, .keycode = 0x3D, .mods = OPT | 0x40)); CHECKEQ(n, 1);
    EXPECT_KEY(g_tr[0], 1, 0xA5, 0x38, 1);
    n = tr(&t, &g, MAC(.type = BW2_MAC_DOWN, .x = 210, .y = 568, .button = 0, .mods = OPT | 0x40)); CHECKEQ(n, 2);
    EXPECT_KEY(g_tr[0], 0, 0xA5, 0x38, 1);
    EXPECT_BUTTON(g_tr[1], 2, 1, 210, 200);
    /* held back: the other Option down, this one up, that one up */
    n = tr(&t, &g, MAC(.type = BW2_MAC_FLAGS, .keycode = 0x3A, .mods = OPT | 0x60)); CHECKEQ(n, 0);
    n = tr(&t, &g, MAC(.type = BW2_MAC_FLAGS, .keycode = 0x3D, .mods = OPT | 0x20)); CHECKEQ(n, 0);
    n = tr(&t, &g, MAC(.type = BW2_MAC_UP, .x = 210, .y = 568, .button = 0, .mods = OPT | 0x20)); CHECKEQ(n, 1);
    EXPECT_BUTTON(g_tr[0], 2, 0, 210, 200);
    n = tr(&t, &g, MAC(.type = BW2_MAC_FLAGS, .keycode = 0x3A, .mods = 0)); CHECKEQ(n, 0);
    /* both Options: an Alt down only once released and pressed again */
    n = tr(&t, &g, MAC(.type = BW2_MAC_FLAGS, .keycode = 0x3A, .mods = OPT | 0x20)); CHECKEQ(n, 1);
    EXPECT_KEY(g_tr[0], 1, 0xA4, 0x38, 0);
    n = tr(&t, &g, MAC(.type = BW2_MAC_FLAGS, .keycode = 0x3D, .mods = OPT | 0x60)); CHECKEQ(n, 1);
    EXPECT_KEY(g_tr[0], 1, 0xA5, 0x38, 1);
    /* Option + left with both Alts down: both go up first */
    n = tr(&t, &g, MAC(.type = BW2_MAC_DOWN, .x = 300, .y = 468, .button = 0, .mods = OPT | 0x60)); CHECKEQ(n, 4);
    EXPECT_MOVE(g_tr[0], 300, 300);
    EXPECT_KEY(g_tr[1], 0, 0xA4, 0x38, 0);
    EXPECT_KEY(g_tr[2], 0, 0xA5, 0x38, 1);
    EXPECT_BUTTON(g_tr[3], 2, 1, 300, 300);
    n = tr(&t, &g, MAC(.type = BW2_MAC_UP, .x = 300, .y = 468, .button = 0, .mods = OPT | 0x60)); CHECKEQ(n, 1);
    EXPECT_BUTTON(g_tr[0], 2, 0, 300, 300);
    /* still held: Option + left again is the middle again, with no Alt to release */
    n = tr(&t, &g, MAC(.type = BW2_MAC_DOWN, .x = 300, .y = 468, .button = 0, .mods = OPT | 0x60)); CHECKEQ(n, 1);
    EXPECT_BUTTON(g_tr[0], 2, 1, 300, 300);
    n = tr(&t, &g, MAC(.type = BW2_MAC_UP, .x = 300, .y = 468, .button = 0)); CHECKEQ(n, 1);
    EXPECT_BUTTON(g_tr[0], 2, 0, 300, 300);
    n = tr(&t, &g, MAC(.type = BW2_MAC_FLAGS, .keycode = 0x3D, .mods = OPT | 0x20)); CHECKEQ(n, 0);
    n = tr(&t, &g, MAC(.type = BW2_MAC_FLAGS, .keycode = 0x3A, .mods = 0)); CHECKEQ(n, 0);

    /* Option + right is the right button, and Alt stays down */
    n = tr(&t, &g, MAC(.type = BW2_MAC_FLAGS, .keycode = 0x3A, .mods = OPT)); CHECKEQ(n, 1);
    n = tr(&t, &g, MAC(.type = BW2_MAC_DOWN, .x = 300, .y = 468, .button = 1, .mods = OPT)); CHECKEQ(n, 1);
    EXPECT_BUTTON(g_tr[0], 1, 1, 300, 300);
    n = tr(&t, &g, MAC(.type = BW2_MAC_UP, .x = 300, .y = 468, .button = 1, .mods = OPT)); CHECKEQ(n, 1);
    EXPECT_BUTTON(g_tr[0], 1, 0, 300, 300);
    n = tr(&t, &g, MAC(.type = BW2_MAC_FLAGS, .keycode = 0x3A, .mods = 0)); CHECKEQ(n, 1);
    EXPECT_KEY(g_tr[0], 0, 0xA4, 0x38, 0);
}

static void group_cmd_keys(void) {
    struct bw2_geom g = { 1024, 768, 1024, 768 };
    struct bw2_translator t = { 0 };
    const uint64_t CMD = BW2_MOD_COMMAND;
    int n;
    /* Command down (left, right): nothing; Cmd+Q: nothing, its up: nothing */
    n = tr(&t, &g, MAC(.type = BW2_MAC_FLAGS, .keycode = 0x37, .mods = CMD | 0x08)); CHECKEQ(n, 0);
    n = tr(&t, &g, MAC(.type = BW2_MAC_KEYDOWN, .keycode = 0x0C, .mods = CMD | 0x08)); CHECKEQ(n, 0);
    n = tr(&t, &g, MAC(.type = BW2_MAC_KEYUP, .keycode = 0x0C, .mods = CMD | 0x08)); CHECKEQ(n, 0);
    n = tr(&t, &g, MAC(.type = BW2_MAC_FLAGS, .keycode = 0x36, .mods = CMD | 0x18)); CHECKEQ(n, 0);
    n = tr(&t, &g, MAC(.type = BW2_MAC_FLAGS, .keycode = 0x36, .mods = CMD | 0x08)); CHECKEQ(n, 0);
    n = tr(&t, &g, MAC(.type = BW2_MAC_FLAGS, .keycode = 0x37, .mods = 0)); CHECKEQ(n, 0);
    /* Q after Command: the key */
    n = tr(&t, &g, MAC(.type = BW2_MAC_KEYUP, .keycode = 0x0C)); CHECKEQ(n, 0);   /* never given down */
    n = tr(&t, &g, MAC(.type = BW2_MAC_KEYDOWN, .keycode = 0x0C)); CHECKEQ(n, 1);
    EXPECT_KEY(g_tr[0], 1, 'Q', 0x10, 0); CHECKEQ(g_tr[0].repeat, 0);
    n = tr(&t, &g, MAC(.type = BW2_MAC_KEYDOWN, .keycode = 0x0C, .repeat = 1)); CHECKEQ(n, 1);
    EXPECT_KEY(g_tr[0], 1, 'Q', 0x10, 0); CHECKEQ(g_tr[0].repeat, 1);
    /* Command pressed while Q is held: Q's up still reaches the game */
    n = tr(&t, &g, MAC(.type = BW2_MAC_FLAGS, .keycode = 0x37, .mods = CMD)); CHECKEQ(n, 0);
    n = tr(&t, &g, MAC(.type = BW2_MAC_KEYDOWN, .keycode = 0x0C, .mods = CMD, .repeat = 1)); CHECKEQ(n, 0);
    n = tr(&t, &g, MAC(.type = BW2_MAC_KEYUP, .keycode = 0x0C, .mods = CMD)); CHECKEQ(n, 1);
    EXPECT_KEY(g_tr[0], 0, 'Q', 0x10, 0);
    n = tr(&t, &g, MAC(.type = BW2_MAC_FLAGS, .keycode = 0x37, .mods = 0)); CHECKEQ(n, 0);
    /* Control+A and Shift+A are keys; Control is a key too */
    n = tr(&t, &g, MAC(.type = BW2_MAC_FLAGS, .keycode = 0x3B, .mods = BW2_MOD_CONTROL)); CHECKEQ(n, 1);
    EXPECT_KEY(g_tr[0], 1, 0xA2, 0x1D, 0);
    n = tr(&t, &g, MAC(.type = BW2_MAC_KEYDOWN, .keycode = 0x00, .mods = BW2_MOD_CONTROL)); CHECKEQ(n, 1);
    EXPECT_KEY(g_tr[0], 1, 'A', 0x1E, 0);
    n = tr(&t, &g, MAC(.type = BW2_MAC_KEYUP, .keycode = 0x00, .mods = BW2_MOD_CONTROL)); CHECKEQ(n, 1);
    EXPECT_KEY(g_tr[0], 0, 'A', 0x1E, 0);
    n = tr(&t, &g, MAC(.type = BW2_MAC_FLAGS, .keycode = 0x3B, .mods = 0)); CHECKEQ(n, 1);
    EXPECT_KEY(g_tr[0], 0, 0xA2, 0x1D, 0);
    /* right Control and right Shift */
    n = tr(&t, &g, MAC(.type = BW2_MAC_FLAGS, .keycode = 0x3E, .mods = BW2_MOD_CONTROL | 0x2000)); CHECKEQ(n, 1);
    EXPECT_KEY(g_tr[0], 1, 0xA3, 0x1D, 1);
    n = tr(&t, &g, MAC(.type = BW2_MAC_FLAGS, .keycode = 0x3E, .mods = 0)); CHECKEQ(n, 1);
    EXPECT_KEY(g_tr[0], 0, 0xA3, 0x1D, 1);
    n = tr(&t, &g, MAC(.type = BW2_MAC_FLAGS, .keycode = 0x3C, .mods = BW2_MOD_SHIFT | 0x04)); CHECKEQ(n, 1);
    EXPECT_KEY(g_tr[0], 1, 0xA1, 0x36, 0);
    /* left Shift too, then the right one up: the left stays */
    n = tr(&t, &g, MAC(.type = BW2_MAC_FLAGS, .keycode = 0x38, .mods = BW2_MOD_SHIFT | 0x06)); CHECKEQ(n, 1);
    EXPECT_KEY(g_tr[0], 1, 0xA0, 0x2A, 0);
    n = tr(&t, &g, MAC(.type = BW2_MAC_FLAGS, .keycode = 0x3C, .mods = BW2_MOD_SHIFT | 0x02)); CHECKEQ(n, 1);
    EXPECT_KEY(g_tr[0], 0, 0xA1, 0x36, 0);
    /* a flags event with no change: nothing */
    n = tr(&t, &g, MAC(.type = BW2_MAC_FLAGS, .keycode = 0x38, .mods = BW2_MOD_SHIFT | 0x02)); CHECKEQ(n, 0);
    n = tr(&t, &g, MAC(.type = BW2_MAC_FLAGS, .keycode = 0x38, .mods = 0)); CHECKEQ(n, 1);
    EXPECT_KEY(g_tr[0], 0, 0xA0, 0x2A, 0);
    /* with no device bits: both sides of a family go up when its bit clears */
    n = tr(&t, &g, MAC(.type = BW2_MAC_FLAGS, .keycode = 0x38, .mods = BW2_MOD_SHIFT)); CHECKEQ(n, 1);
    n = tr(&t, &g, MAC(.type = BW2_MAC_FLAGS, .keycode = 0x3C, .mods = BW2_MOD_SHIFT)); CHECKEQ(n, 1);
    EXPECT_KEY(g_tr[0], 1, 0xA1, 0x36, 0);
    n = tr(&t, &g, MAC(.type = BW2_MAC_FLAGS, .keycode = 0x3C, .mods = 0)); CHECKEQ(n, 2);
    EXPECT_KEY(g_tr[0], 0, 0xA0, 0x2A, 0);
    EXPECT_KEY(g_tr[1], 0, 0xA1, 0x36, 0);
    /* Fn, keypad Clear, keypad =, F16: nothing */
    n = tr(&t, &g, MAC(.type = BW2_MAC_FLAGS, .keycode = 0x3F, .mods = 1ull << 23)); CHECKEQ(n, 0);
    n = tr(&t, &g, MAC(.type = BW2_MAC_FLAGS, .keycode = 0x3F, .mods = 0)); CHECKEQ(n, 0);
    n = tr(&t, &g, MAC(.type = BW2_MAC_KEYDOWN, .keycode = 0x47)); CHECKEQ(n, 0);
    n = tr(&t, &g, MAC(.type = BW2_MAC_KEYDOWN, .keycode = 0x51)); CHECKEQ(n, 0);
    n = tr(&t, &g, MAC(.type = BW2_MAC_KEYDOWN, .keycode = 0x6A)); CHECKEQ(n, 0);
    n = tr(&t, &g, MAC(.type = BW2_MAC_KEYUP, .keycode = 0x6A)); CHECKEQ(n, 0);
    /* an extended key: the arrows */
    n = tr(&t, &g, MAC(.type = BW2_MAC_KEYDOWN, .keycode = 0x7B, .mods = 1ull << 23)); CHECKEQ(n, 1);
    EXPECT_KEY(g_tr[0], 1, 0x25, 0x4B, 1);
    n = tr(&t, &g, MAC(.type = BW2_MAC_KEYUP, .keycode = 0x7B, .mods = 1ull << 23)); CHECKEQ(n, 1);
    EXPECT_KEY(g_tr[0], 0, 0x25, 0x4B, 1);
}

static void group_caps(void) {
    struct bw2_geom g = { 1024, 768, 1024, 768 };
    struct bw2_translator t = { 0 };
    int n;
    n = tr(&t, &g, MAC(.type = BW2_MAC_FLAGS, .keycode = 0x39, .mods = BW2_MOD_CAPS)); CHECKEQ(n, 2);
    EXPECT_KEY(g_tr[0], 1, 0x14, 0x3A, 0);
    EXPECT_KEY(g_tr[1], 0, 0x14, 0x3A, 0);
    n = tr(&t, &g, MAC(.type = BW2_MAC_FLAGS, .keycode = 0x39, .mods = 0)); CHECKEQ(n, 2);
    EXPECT_KEY(g_tr[0], 1, 0x14, 0x3A, 0);
    EXPECT_KEY(g_tr[1], 0, 0x14, 0x3A, 0);
    /* no change: nothing */
    n = tr(&t, &g, MAC(.type = BW2_MAC_FLAGS, .keycode = 0x39, .mods = 0)); CHECKEQ(n, 0);
    /* toggled while away, seen on the next flags change: the toggle first, then the modifier */
    n = tr(&t, &g, MAC(.type = BW2_MAC_FLAGS, .keycode = 0x38, .mods = BW2_MOD_CAPS | BW2_MOD_SHIFT)); CHECKEQ(n, 3);
    EXPECT_KEY(g_tr[0], 1, 0x14, 0x3A, 0);
    EXPECT_KEY(g_tr[1], 0, 0x14, 0x3A, 0);
    EXPECT_KEY(g_tr[2], 1, 0xA0, 0x2A, 0);
    /* a key with the lock on is only the key */
    n = tr(&t, &g, MAC(.type = BW2_MAC_KEYDOWN, .keycode = 0x00, .mods = BW2_MOD_CAPS | BW2_MOD_SHIFT)); CHECKEQ(n, 1);
    EXPECT_KEY(g_tr[0], 1, 'A', 0x1E, 0);
}

/* Additions: buttons, the MOVE before them, and what is not sent. */
static void group_mouse_events(void) {
    struct bw2_geom g = { 1024, 768, 1024, 768 };
    struct bw2_translator t = { 0 };
    int n;
    /* the first event has no point before it: a MOVE first */
    n = tr(&t, &g, MAC(.type = BW2_MAC_DOWN, .x = 5, .y = 763, .button = 0)); CHECKEQ(n, 2);
    EXPECT_MOVE(g_tr[0], 5, 5); CHECKEQ(g_tr[0].dx, 0); CHECKEQ(g_tr[0].dy, 0);
    EXPECT_BUTTON(g_tr[1], 0, 1, 5, 5);
    /* same point: the button only */
    n = tr(&t, &g, MAC(.type = BW2_MAC_UP, .x = 5.5, .y = 762.5, .button = 0)); CHECKEQ(n, 1);
    EXPECT_BUTTON(g_tr[0], 0, 0, 5, 5);
    /* buttons 1, 2, 3, 4 */
    for (int b = 1; b <= 4; b++) {
        n = tr(&t, &g, MAC(.type = BW2_MAC_DOWN, .x = 5, .y = 763, .button = b)); CHECKEQ(n, 1);
        EXPECT_BUTTON(g_tr[0], b, 1, 5, 5);
        n = tr(&t, &g, MAC(.type = BW2_MAC_UP, .x = 5, .y = 763, .button = b)); CHECKEQ(n, 1);
        EXPECT_BUTTON(g_tr[0], b, 0, 5, 5);
    }
    /* button 5 and up: nothing; an up of a button never given down: nothing */
    n = tr(&t, &g, MAC(.type = BW2_MAC_DOWN, .x = 5, .y = 763, .button = 5)); CHECKEQ(n, 0);
    n = tr(&t, &g, MAC(.type = BW2_MAC_UP, .x = 5, .y = 763, .button = 5)); CHECKEQ(n, 0);
    n = tr(&t, &g, MAC(.type = BW2_MAC_UP, .x = 5, .y = 763, .button = 1)); CHECKEQ(n, 0);
    /* moved: the MOVE first, even with nothing else to send */
    n = tr(&t, &g, MAC(.type = BW2_MAC_UP, .x = 9, .y = 763, .button = 1)); CHECKEQ(n, 1);
    EXPECT_MOVE(g_tr[0], 9, 5);
    n = tr(&t, &g, MAC(.type = BW2_MAC_DOWN, .x = 20, .y = 748, .button = 1, .dx = 4, .dy = 6)); CHECKEQ(n, 2);
    EXPECT_MOVE(g_tr[0], 20, 20); CHECKEQ(g_tr[0].dx, 4); CHECKEQ(g_tr[0].dy, 6);
    EXPECT_BUTTON(g_tr[1], 1, 1, 20, 20);
    /* a delta with no new point still moves first (a detached pointer) */
    n = tr(&t, &g, MAC(.type = BW2_MAC_UP, .x = 20, .y = 748, .button = 1, .dx = 2)); CHECKEQ(n, 2);
    EXPECT_MOVE(g_tr[0], 20, 20); CHECKEQ(g_tr[0].dx, 2);
    EXPECT_BUTTON(g_tr[1], 1, 0, 20, 20);
    /* Control + left is the left button, not the right */
    n = tr(&t, &g, MAC(.type = BW2_MAC_DOWN, .x = 20, .y = 748, .button = 0, .mods = BW2_MOD_CONTROL)); CHECKEQ(n, 1);
    EXPECT_BUTTON(g_tr[0], 0, 1, 20, 20);
    n = tr(&t, &g, MAC(.type = BW2_MAC_UP, .x = 20, .y = 748, .button = 0, .mods = BW2_MOD_CONTROL)); CHECKEQ(n, 1);
    EXPECT_BUTTON(g_tr[0], 0, 0, 20, 20);
    /* Command + click still clicks (only keys go to the menus) */
    n = tr(&t, &g, MAC(.type = BW2_MAC_DOWN, .x = 20, .y = 748, .button = 0, .mods = BW2_MOD_COMMAND)); CHECKEQ(n, 1);
    EXPECT_BUTTON(g_tr[0], 0, 1, 20, 20);
    n = tr(&t, &g, MAC(.type = BW2_MAC_UP, .x = 20, .y = 748, .button = 0)); CHECKEQ(n, 1);
    /* a view with no size has no picture to point at: no mouse events, keys still go */
    struct bw2_geom g0 = { 0, 0, 1024, 768 };
    n = tr(&t, &g0, MAC(.type = BW2_MAC_MOVE, .x = 1, .y = 1, .dx = 5)); CHECKEQ(n, 0);
    n = tr(&t, &g0, MAC(.type = BW2_MAC_DOWN, .x = 1, .y = 1, .button = 0)); CHECKEQ(n, 0);
    n = tr(&t, &g0, MAC(.type = BW2_MAC_SCROLL, .x = 1, .y = 1, .scroll_dy = 1)); CHECKEQ(n, 0);
    n = tr(&t, &g0, MAC(.type = BW2_MAC_KEYDOWN, .keycode = 0x31)); CHECKEQ(n, 1);
    EXPECT_KEY(g_tr[0], 1, 0x20, 0x39, 0);
    n = tr(&t, &g0, MAC(.type = BW2_MAC_KEYUP, .keycode = 0x31)); CHECKEQ(n, 1);
}

/* Addition: on deactivation the queue gives the game its ups; the translator forgets its downs. */
static void group_translate_reset(void) {
    struct bw2_geom g = { 1024, 768, 1024, 768 };
    struct bw2_translator t = { 0 };
    int n;
    n = tr(&t, &g, MAC(.type = BW2_MAC_FLAGS, .keycode = 0x39, .mods = BW2_MOD_CAPS)); CHECKEQ(n, 2);
    n = tr(&t, &g, MAC(.type = BW2_MAC_FLAGS, .keycode = 0x38, .mods = BW2_MOD_CAPS | BW2_MOD_SHIFT)); CHECKEQ(n, 1);
    n = tr(&t, &g, MAC(.type = BW2_MAC_KEYDOWN, .keycode = 0x00, .mods = BW2_MOD_CAPS | BW2_MOD_SHIFT)); CHECKEQ(n, 1);
    n = tr(&t, &g, MAC(.type = BW2_MAC_DOWN, .x = 5, .y = 763, .button = 0)); CHECKEQ(n, 2);
    n = tr(&t, &g, MAC(.type = BW2_MAC_FLAGS, .keycode = 0x3A, .mods = BW2_MOD_CAPS | BW2_MOD_SHIFT | BW2_MOD_OPTION));
    CHECKEQ(n, 1);
    n = tr(&t, &g, MAC(.type = BW2_MAC_DOWN, .x = 5, .y = 763, .button = 1, .mods = BW2_MOD_OPTION)); CHECKEQ(n, 1);
    n = tr(&t, &g, MAC(.type = BW2_MAC_SCROLL, .x = 5, .y = 763, .precise = 1, .scroll_dy = 7)); CHECKEQ(n, 0);
    bw2_translate_reset(&t);
    /* the ups after it are not sent again */
    n = tr(&t, &g, MAC(.type = BW2_MAC_UP, .x = 5, .y = 763, .button = 0)); CHECKEQ(n, 0);
    n = tr(&t, &g, MAC(.type = BW2_MAC_UP, .x = 5, .y = 763, .button = 1)); CHECKEQ(n, 0);
    n = tr(&t, &g, MAC(.type = BW2_MAC_KEYUP, .keycode = 0x00, .mods = BW2_MOD_CAPS | BW2_MOD_SHIFT)); CHECKEQ(n, 0);
    n = tr(&t, &g, MAC(.type = BW2_MAC_FLAGS, .keycode = 0x3A, .mods = BW2_MOD_CAPS | BW2_MOD_SHIFT)); CHECKEQ(n, 0);
    /* a key still held when the window comes back is pressed afresh */
    n = tr(&t, &g, MAC(.type = BW2_MAC_FLAGS, .keycode = 0x38, .mods = BW2_MOD_CAPS)); CHECKEQ(n, 0);
    n = tr(&t, &g, MAC(.type = BW2_MAC_FLAGS, .keycode = 0x38, .mods = BW2_MOD_CAPS | BW2_MOD_SHIFT)); CHECKEQ(n, 1);
    EXPECT_KEY(g_tr[0], 1, 0xA0, 0x2A, 0);
    /* the lock state and the point are kept; the scroll remainder is not */
    n = tr(&t, &g, MAC(.type = BW2_MAC_FLAGS, .keycode = 0x39, .mods = BW2_MOD_CAPS | BW2_MOD_SHIFT)); CHECKEQ(n, 0);
    n = tr(&t, &g, MAC(.type = BW2_MAC_DOWN, .x = 5, .y = 763, .button = 0)); CHECKEQ(n, 1);
    EXPECT_BUTTON(g_tr[0], 0, 1, 5, 5);
    n = tr(&t, &g, MAC(.type = BW2_MAC_SCROLL, .x = 5, .y = 763, .precise = 1, .scroll_dy = 3)); CHECKEQ(n, 0);
    CHECK(NEAR(t.scroll_rem, 3));
}

/* window_size (plan 6 Task 4): the first windowed size is the back buffer in points, scaled down
 * (never up) to fit 90% of the screen's visible frame, the aspect kept. */
static int near(double a, double b) { return fabs(a - b) < 1e-9; }
static void group_window_size(void) {
    double w = 0, h = 0;
    bw2_window_fit(1512, 945, 1024, 768, &w, &h);
    CHECK(near(w, 1024) && near(h, 768));
    bw2_window_fit(1512, 945, 1920, 1080, &w, &h);       /* 90% of the width is the tighter bound */
    if (!near(w, 1360.8) || !near(h, 765.45)) { printf("FAIL window_size: %.6f x %.6f\n", w, h); fails++; }
    bw2_window_fit(1512, 945, 1000, 1000, &w, &h);       /* 90% of the height */
    CHECK(near(w, 850.5) && near(h, 850.5));
    bw2_window_fit(1512, 945, 640, 480, &w, &h);
    CHECK(near(w, 640) && near(h, 480));
    bw2_window_fit(1360.8, 850.5, 1512, 945, &w, &h);    /* the same aspect: both bounds at once */
    CHECK(near(w, 1224.72) && near(h, 765.45));
    bw2_window_fit(0, 0, 800, 600, &w, &h);              /* no visible frame known: the back buffer */
    CHECK(near(w, 800) && near(h, 600));
}

/* ---- plan 6 Task 5: the cursor ---- */

#define IDC_ARROW 32512u
static struct bw2_cursor_mode last_mode(void) { struct bw2_cursor_mode m; bw2_cursor_last(&m); return m; }
#define EXPECT_MODE(hid, det, img) do { struct bw2_cursor_mode _m = last_mode(); CHECKEQ(_m.hidden, hid); \
    CHECKEQ(_m.detached, det); CHECKEQ(_m.image, img); } while (0)
#define EXPECT_WARP(px, py) do { struct bw2_cursor_mode _m = last_mode(); CHECKEQ(_m.warp, 1); \
    CHECKEQ(_m.warp_x, px); CHECKEQ(_m.warp_y, py); } while (0)
#define EXPECT_NO_WARP() CHECKEQ(last_mode().warp, 0)

/* cursor_policy: the game's ShowCursor, focus, the exclusive mouse and SetCursor. */
static void group_cursor_policy(void) {
    uint32_t a = make_window(0, 0, 0, 1024, 768);
    bw2_in_set_window(a);
    pump_all();
    struct bw2_cursor_mode m;
    bw2_cursor_policy(&m);
    CHECKEQ(m.hidden, 0);
    CHECKEQ(m.detached, 0);
    CHECKEQ(m.image, 0);
    CHECKEQ(m.warp, 0);
    /* the game's one ShowCursor(FALSE) ("Measured" item 5) */
    CHECKEQ(call("ShowCursor", 1, 0u), (uint32_t)-1);
    EXPECT_MODE(1, 0, 0);
    EXPECT_NO_WARP();
    /* inactive: shown whatever the count; back: hidden again */
    ev_activate(0);
    pump_all();
    EXPECT_MODE(0, 0, 0);
    ev_activate(1);
    pump_all();
    EXPECT_MODE(1, 0, 0);
    CHECKEQ(call("ShowCursor", 1, 1u), 0u);
    EXPECT_MODE(0, 0, 0);
    /* the exclusive mouse (coop 5), acquired and active: detached and hidden */
    uint32_t di = new_di(), dev = game_mouse(di, 512);
    EXPECT_MODE(1, 1, 0);
    CHECKEQ(vcall(dev, DD_Unacquire, 0), DI_OK);
    EXPECT_MODE(0, 0, 0);
    /* SetCursor(NULL) hides; the system arrow with the count at 0 shows the arrow */
    call("SetCursor", 1, 0u);
    EXPECT_MODE(1, 0, 0);
    uint32_t arrow = call("LoadCursorA", 2, 0u, IDC_ARROW);
    CHECK(arrow != 0);
    call("SetCursor", 1, arrow);
    EXPECT_MODE(0, 0, 0);
    CHECKEQ(vcall(dev, 2, 0), 0);
    CHECKEQ(vcall(di, 2, 0), 0);
}

static uint32_t g_ms_dev;
static int g_ms_seen;
static struct bw2_cursor_mode g_ms_mode;
static void acquire_on_activate(uint32_t h, uint32_t m, uint32_t w, uint32_t l) {
    (void)h, (void)l;
    if (m != WM_ACTIVATE || w != 1u) return;
    CHECKEQ(vcall(g_ms_dev, DD_Acquire, 0), DI_OK);
    bw2_cursor_last(&g_ms_mode);
    g_ms_seen++;
}
/* mode_switch: the game's setup and mode switch on a 640x480 game window, with no cursor shim called
 * after the DirectInput calls: DirectInput alone detaches the pointer, parked at the window's centre. */
static void group_mode_switch(void) {
    uint32_t a = make_window(0, 50, 50, 640, 480);
    bw2_in_set_window(a);
    pump_all();
    uint32_t di = new_di(), dev = new_mouse(di);
    CHECKEQ(vcall(dev, DD_SetDataFormat, 1, mouse_format(16)), DI_OK);
    CHECKEQ(vcall(dev, DD_SetCooperativeLevel, 2, a, 6u), DI_OK);
    CHECKEQ(vcall(dev, DD_SetProperty, 2, DIPROP_BUFFERSIZE, bufsize_prop(512)), DI_OK);
    CHECKEQ(vcall(dev, DD_Acquire, 0), DI_OK);
    EXPECT_MODE(0, 0, 0);                                /* non-exclusive: the pointer stays */
    EXPECT_NO_WARP();
    CHECKEQ(vcall(dev, DD_Unacquire, 0), DI_OK);
    CHECKEQ(vcall(dev, DD_SetCooperativeLevel, 2, a, 5u), DI_OK);
    EXPECT_MODE(0, 0, 0);
    CHECKEQ(vcall(dev, DD_Acquire, 0), DI_OK);
    EXPECT_MODE(1, 1, 0);
    EXPECT_WARP(320, 240);
    CHECKEQ(vcall(dev, DD_Unacquire, 0), DI_OK);
    EXPECT_MODE(0, 0, 0);
    EXPECT_NO_WARP();
    CHECKEQ(vcall(dev, DD_Acquire, 0), DI_OK);
    EXPECT_MODE(1, 1, 0);
    EXPECT_WARP(320, 240);
    /* Cmd-Tab away: reattached and shown */
    ev_activate(0);
    pump_all();
    EXPECT_MODE(0, 0, 0);
    EXPECT_NO_WARP();
    /* back: the foreground device was lost, so the pointer stays until the game's retry acquires it */
    ev_activate(1);
    pump_all();
    EXPECT_MODE(0, 0, 0);
    CHECKEQ(vcall(dev, DD_Acquire, 0), DI_OK);
    EXPECT_MODE(1, 1, 0);
    EXPECT_WARP(320, 240);
    /* nothing changed: no warp */
    bw2_cursor_update();
    EXPECT_MODE(1, 1, 0);
    EXPECT_NO_WARP();
    /* a partial drain that meets an inactive activation (the loss) reattaches at once */
    ev_activate(0);
    uint32_t buf = gmem(20 * 512), n = 512;
    CHECKEQ(read_data(dev, 20, buf, &n, 0), DIERR_INPUTLOST);
    EXPECT_MODE(0, 0, 0);
    pump_all();
    ev_activate(1);
    pump_all();
    CHECKEQ(vcall(dev, DD_Acquire, 0), DI_OK);
    EXPECT_MODE(1, 1, 0);
    EXPECT_WARP(320, 240);
    /* a window procedure that acquires in WM_ACTIVATE: the detach is published from its Acquire, with
     * the warp, and the drain's own update after the activation adds no second warp */
    ev_activate(0);
    pump_all();
    EXPECT_MODE(0, 0, 0);
    g_ms_dev = dev, g_ms_seen = 0;
    hle_proc_hook = acquire_on_activate;
    ev_activate(1);
    pump_all();
    hle_proc_hook = NULL;
    CHECKEQ(g_ms_seen, 1);
    CHECKEQ(g_ms_mode.detached, 1);
    CHECKEQ(g_ms_mode.hidden, 1);
    CHECKEQ(g_ms_mode.warp, 1);
    CHECKEQ(g_ms_mode.warp_x, 320);
    CHECKEQ(g_ms_mode.warp_y, 240);
    EXPECT_MODE(1, 1, 0);
    EXPECT_NO_WARP();
    /* released while acquired: no exclusive mouse left */
    CHECKEQ(vcall(dev, 2, 0), 0);
    EXPECT_MODE(0, 0, 0);
    CHECKEQ(vcall(di, 2, 0), 0);
}

/* cursor_warp: SetCursorPos warps the pointer to the client point while active and not detached. */
static void group_cursor_warp(void) {
    uint32_t a = make_window(0, 100, 50, 640, 480), pt = gmem(8);
    bw2_in_set_window(a);
    pump_all();
    CHECKEQ(call("SetCursorPos", 2, 150u, 90u), 1u);
    EXPECT_MODE(0, 0, 0);
    EXPECT_WARP(50, 40);
    CHECKEQ(call("GetCursorPos", 1, pt), 1u);
    CHECKEQ(MEM32(pt), 150);
    CHECKEQ(MEM32(pt + 4), 90);
    /* outside the client area: still the client point */
    CHECKEQ(call("SetCursorPos", 2, 20u, 10u), 1u);
    EXPECT_WARP(-80, -40);
    /* another cursor call: no warp of its own */
    call("ShowCursor", 1, 1u);
    EXPECT_NO_WARP();
    call("ShowCursor", 1, 0u);
    /* inactive: the position is kept, the pointer is not moved */
    ev_activate(0);
    pump_all();
    CHECKEQ(call("SetCursorPos", 2, 200u, 100u), 1u);
    EXPECT_NO_WARP();
    CHECKEQ(call("GetCursorPos", 1, pt), 1u);
    CHECKEQ(MEM32(pt), 200);
    ev_activate(1);
    pump_all();
    EXPECT_NO_WARP();                                    /* a SetCursorPos made while inactive is not kept */
    /* detached: the detach's own centre warp, then SetCursorPos warps nothing */
    uint32_t di = new_di(), dev = new_mouse(di);
    CHECKEQ(vcall(dev, DD_SetDataFormat, 1, mouse_format(16)), DI_OK);
    CHECKEQ(vcall(dev, DD_SetCooperativeLevel, 2, a, 5u), DI_OK);
    CHECKEQ(vcall(dev, DD_Acquire, 0), DI_OK);
    EXPECT_MODE(1, 1, 0);
    EXPECT_WARP(320, 240);
    CHECKEQ(call("SetCursorPos", 2, 150u, 90u), 1u);
    EXPECT_MODE(1, 1, 0);
    EXPECT_NO_WARP();
    CHECKEQ(call("GetCursorPos", 1, pt), 1u);
    CHECKEQ(MEM32(pt), 150);
    CHECKEQ(vcall(dev, 2, 0), 0);
    CHECKEQ(vcall(di, 2, 0), 0);
    /* with a clip: the position, and so the warp, kept inside it */
    int clip[4] = { 100, 50, 300, 200 };
    bw2_in_clip(clip);
    CHECKEQ(call("SetCursorPos", 2, 500u, 10u), 1u);
    EXPECT_WARP(199, 0);
    CHECKEQ(call("GetCursorPos", 1, pt), 1u);
    CHECKEQ(MEM32(pt), 299);
    CHECKEQ(MEM32(pt + 4), 50);
    bw2_in_clip(NULL);
    /* the Mac side's warp target: a client pixel's centre in view points (bottom-left origin), through
     * the letterbox; a move there translates back to the same pixel */
    struct bw2_geom g = { 1512, 982, 1024, 768 };
    double vx, vy, x0, y0, sc;
    bw2_letterbox(&g, &x0, &y0, &sc);
    bw2_client_to_view(&g, 512, 384, &vx, &vy);
    CHECK(fabs(vx - (x0 + 512.5 * sc)) < 1e-9 && fabs(vy - (982 - y0 - 384.5 * sc)) < 1e-9);
    CHECK(fabs(x0 - (1512 - 1024 * 982.0 / 768) / 2) < 1e-9 && fabs(y0) < 1e-9);
    static const int pts[5][2] = { { 0, 0 }, { 1023, 767 }, { 512, 384 }, { 1, 766 }, { 700, 3 } };
    for (int i = 0; i < 5; i++) {
        struct bw2_translator t;
        memset(&t, 0, sizeof t);
        bw2_client_to_view(&g, pts[i][0], pts[i][1], &vx, &vy);
        int n = tr(&t, &g, MAC(.type = BW2_MAC_MOVE, .x = vx, .y = vy));
        CHECKEQ(n, 1);
        if (n == 1) { CHECKEQ(g_tr[0].x, pts[i][0]); CHECKEQ(g_tr[0].y, pts[i][1]); }
    }
    /* a point off the picture maps off it, linearly */
    bw2_client_to_view(&g, -80, -40, &vx, &vy);
    CHECK(fabs(vx - (x0 - 79.5 * sc)) < 1e-9 && fabs(vy - (982 - y0 + 39.5 * sc)) < 1e-9);
}

static uint32_t grect(int l, int t, int r, int b) {
    uint32_t p = gmem(16);
    MEM32(p) = (uint32_t)l, MEM32(p + 4) = (uint32_t)t, MEM32(p + 8) = (uint32_t)r, MEM32(p + 12) = (uint32_t)b;
    return p;
}
/* clip: ClipCursor clamps the reported point while active; NULL, empty and inverted clear it. */
static void group_clip(void) {
    uint32_t a = make_window(0, 0, 0, 640, 480), pt = gmem(8);
    hle_proc_hook = recorder;
    bw2_in_set_window(a);
    pump_all();
    CHECKEQ(call("ClipCursor", 1, grect(0, 0, 640, 480)), 1u);
    rec_reset();
    ev_move(700, 10);
    pump_all();
    EXPECT(1, WM_MOUSEMOVE, ANY, (10u << 16) | 639u);
    CHECKEQ(call("GetCursorPos", 1, pt), 1u);
    CHECKEQ(MEM32(pt), 639);
    CHECKEQ(MEM32(pt + 4), 10);
    /* inactive: not clamped */
    ev_activate(0);
    pump_all();
    ev_move(700, 10);
    CHECKEQ(call("GetCursorPos", 1, pt), 1u);
    CHECKEQ(MEM32(pt), 700);
    ev_activate(1);
    pump_all();
    ev_move(700, 10);
    CHECKEQ(call("GetCursorPos", 1, pt), 1u);
    CHECKEQ(MEM32(pt), 639);
    /* NULL unclamps */
    CHECKEQ(call("ClipCursor", 1, 0u), 1u);
    ev_move(700, 10);
    CHECKEQ(call("GetCursorPos", 1, pt), 1u);
    CHECKEQ(MEM32(pt), 700);
    /* the game's garbage rect ("Measured" item 10): TRUE, and no clip */
    CHECKEQ(call("ClipCursor", 1, grect(0, 0, 640, 480)), 1u);
    CHECKEQ(call("ClipCursor", 1, grect(11883128, -1, -559087616, 13098988)), 1u);
    ev_move(700, 10);
    CHECKEQ(call("GetCursorPos", 1, pt), 1u);
    CHECKEQ(MEM32(pt), 700);
    /* an empty rect clears it too */
    CHECKEQ(call("ClipCursor", 1, grect(0, 0, 640, 480)), 1u);
    CHECKEQ(call("ClipCursor", 1, grect(10, 10, 10, 400)), 1u);
    ev_move(700, 10);
    CHECKEQ(call("GetCursorPos", 1, pt), 1u);
    CHECKEQ(MEM32(pt), 700);
    /* then the game's window rect: a MOVE to screen (2000, 2000) stops at (1073, 817), outside this
     * window (no message) and at the bottom-right pixel of the game's own 1024x768 window at (50, 50) */
    CHECKEQ(call("ClipCursor", 1, grect(50, 50, 1074, 818)), 1u);
    pump_all();
    rec_reset();
    ev_move(2000, 2000);
    pump_all();
    CHECKEQ(g_nrec, 0);
    CHECKEQ(call("GetCursorPos", 1, pt), 1u);
    CHECKEQ(MEM32(pt), 1073);
    CHECKEQ(MEM32(pt + 4), 817);
    uint32_t game = make_window(0, 50, 50, 1024, 768);
    bw2_in_set_window(game);
    rec_reset();
    ev_move(1950, 1950);
    pump_all();
    EXPECT(1, WM_MOUSEMOVE, ANY, (767u << 16) | 1023u);
    if (g_nrec) CHECKEQ(g_rec[0].h, game);
    ev_move(-10, -10);
    CHECKEQ(call("GetCursorPos", 1, pt), 1u);
    CHECKEQ(MEM32(pt), 50);
    CHECKEQ(MEM32(pt + 4), 50);
    hle_proc_hook = NULL;
}

/* cursor_image: a 32x32 monochrome cursor's AND/XOR masks as RGBA. */
static void group_cursor_image(void) {
    hle_cursor_image_t img;
    memset(&img, 0, sizeof img);
    img.handle = 0x1C100000u;
    /* row r, pixel x: the bit (7 - x % 8) of byte r * 4 + x / 8 (rows top-down, MSB first) */
    for (int r = 0; r < 32; r++)
        for (int x = 0; x < 32; x++) {
            int kind = (r + x) % 4;                      /* 0 transparent, 1 black, 2 white, 3 inverted */
            int and_bit = kind == 0 || kind == 3, xor_bit = kind == 2 || kind == 3;
            if (and_bit) img.and_mask[r * 4 + x / 8] |= (uint8_t)(0x80u >> (x % 8));
            if (xor_bit) img.xor_mask[r * 4 + x / 8] |= (uint8_t)(0x80u >> (x % 8));
        }
    static uint8_t rgba[32 * 32 * 4];
    memset(rgba, 0xAB, sizeof rgba);
    bw2_cursor_rgba(&img, rgba);
    static const uint8_t want[4][4] = { { 0, 0, 0, 0 }, { 0, 0, 0, 255 }, { 255, 255, 255, 255 }, { 0, 0, 0, 255 } };
    int bad = 0;
    for (int r = 0; r < 32; r++)
        for (int x = 0; x < 32; x++) {
            const uint8_t *p = rgba + (r * 32 + x) * 4, *w = want[(r + x) % 4];
            if (memcmp(p, w, 4)) {
                if (bad++ < 8)
                    printf("FAIL cursor_image: pixel (%d,%d) %u,%u,%u,%u want %u,%u,%u,%u\n", x, r, p[0], p[1], p[2],
                           p[3], w[0], w[1], w[2], w[3]);
            }
        }
    fails += bad;
    /* the corners, with a hand-made pattern: only the top-left pixel is white */
    memset(img.and_mask, 0xFF, sizeof img.and_mask);
    memset(img.xor_mask, 0, sizeof img.xor_mask);
    img.and_mask[0] = 0x7F, img.xor_mask[0] = 0x80;
    bw2_cursor_rgba(&img, rgba);
    CHECKEQ(rgba[0], 255);
    CHECKEQ(rgba[3], 255);
    CHECKEQ(rgba[4 + 3], 0);
    CHECKEQ(rgba[(31 * 32 + 31) * 4 + 3], 0);
}

/* ---- plan 6 Task 6: BW2_INPUT_SCRIPT (script.c) ---- */

#define WM_RBUTTONDOWN 0x0204u
#define WM_RBUTTONUP 0x0205u

/* What the mouse sink is given, then on to the DirectInput mouse. */
static struct bw2_in_event g_ss[64];
static int g_nss;
static void script_sink(const struct bw2_in_event *e) {
    if (g_nss < 64) g_ss[g_nss] = *e;
    g_nss++;
    bw2_dinput8_sink(e);
}
static void expect_sunk(int line, int i, enum bw2_in_kind kind, int x, int y, int a, int b) {
    /* a, b: dx, dy for a MOVE; button, down for a BUTTON; the wheel (a) for a WHEEL */
    if (i >= g_nss || i >= 64) { printf("FAIL %s:%d: no sunk event %d (%d)\n", __FILE__, line, i, g_nss); fails++; return; }
    const struct bw2_in_event *e = &g_ss[i];
    int ok = e->kind == kind && e->x == x && e->y == y;
    if (kind == BW2_IN_MOVE) ok = ok && e->dx == a && e->dy == b;
    if (kind == BW2_IN_BUTTON) ok = ok && e->button == a && e->down == b;
    if (kind == BW2_IN_WHEEL) ok = ok && e->wheel == a;
    if (ok) return;
    printf("FAIL %s:%d: sunk %d is kind %d at (%d, %d), d (%d, %d), button %d down %d, wheel %d\n", __FILE__, line, i,
           (int)e->kind, e->x, e->y, e->dx, e->dy, e->button, e->down, e->wheel);
    fails++;
}
#define EXPECT_SUNK(...) expect_sunk(__LINE__, __VA_ARGS__)

/* script: the test's script (tests/test_input.py SCRIPT) through the game's mouse, read by read. */
static void group_script(void) {
    uint32_t di = new_di(), dev = game_mouse(di, 512);
    bw2_in_set_window(HWND);
    hle_proc_hook = recorder;
    pump_all();
    g_nss = 0;
    bw2_in_set_mouse_sink(script_sink);
    uint32_t buf = gmem(20 * 512), n;
    for (int r = 1; r <= 9; r++) {
        printf("script: read %d asked (%d failed so far)\n", r, fails);
        fflush(stdout);
        rec_reset();
        memset(hle_ptr(buf), 0xAB, 20 * 512);
        n = 512;
        CHECKEQ(read_data(dev, 20, buf, &n, 0), DI_OK);
        switch (r) {
        case 3:                                          /* 3 move 510 382: one sequence number */
            CHECKEQ(n, 2);
            CHECKEQ(el(buf, 20, 0, EL_OFS), 0);
            CHECKEQ(el(buf, 20, 0, EL_DATA), 510);
            CHECKEQ(el(buf, 20, 1, EL_OFS), 4);
            CHECKEQ(el(buf, 20, 1, EL_DATA), 382);
            CHECKEQ(el(buf, 20, 0, EL_SEQ), el(buf, 20, 1, EL_SEQ));
            break;
        case 5:                                          /* 5 down 0 */
        case 6:                                          /* 6 up 0 */
            CHECKEQ(n, 1);
            CHECKEQ(el(buf, 20, 0, EL_OFS), 12);
            CHECKEQ(el(buf, 20, 0, EL_DATA), r == 5 ? 0x80u : 0u);
            break;
        case 9:                                          /* 9 move -88 45, 9 wheel -120 */
            CHECKEQ(n, 3);
            CHECKEQ(el(buf, 20, 0, EL_OFS), 0);
            CHECKEQ(el(buf, 20, 0, EL_DATA), (uint32_t)-88);
            CHECKEQ(el(buf, 20, 1, EL_OFS), 4);
            CHECKEQ(el(buf, 20, 1, EL_DATA), 45);
            CHECKEQ(el(buf, 20, 0, EL_SEQ), el(buf, 20, 1, EL_SEQ));
            CHECKEQ(el(buf, 20, 2, EL_OFS), 8);
            CHECKEQ(el(buf, 20, 2, EL_DATA), (uint32_t)-120);
            CHECK(el(buf, 20, 2, EL_SEQ) > el(buf, 20, 1, EL_SEQ));
            break;
        default:
            CHECKEQ(n, 0);
        }
        /* the keys of read 7 were posted by its drain; the pump hands them to the window */
        CHECKEQ(g_nrec, 0);
        pump_all();
        if (r == 7) {
            /* TranslateMessage's WM_CHAR goes after input already queued (Task 1's ruling), so a down
             * and an up pushed at one read give the character after the up */
            EXPECT(3, WM_KEYDOWN, 8u, 0x000E0001u, WM_KEYUP, 8u, 0xC00E0001u, WM_CHAR, 8u, 0x000E0001u);
            if (g_nrec) CHECKEQ(g_rec[0].h, HWND);
        } else
            EXPECT(0);                                   /* the exclusive mouse: no mouse messages */
        printf("script: read %d answered (%d failed so far)\n", r, fails);
        fflush(stdout);
    }
    /* each mouse event at the running client point */
    CHECKEQ(g_nss, 5);
    EXPECT_SUNK(0, BW2_IN_MOVE, 510, 382, 510, 382);
    EXPECT_SUNK(1, BW2_IN_BUTTON, 510, 382, 0, 1);
    EXPECT_SUNK(2, BW2_IN_BUTTON, 510, 382, 0, 0);
    EXPECT_SUNK(3, BW2_IN_MOVE, 422, 427, -88, 45);
    EXPECT_SUNK(4, BW2_IN_WHEEL, 422, 427, -120, 0);
    bw2_in_set_mouse_sink(bw2_dinput8_sink);
    hle_proc_hook = NULL;
    CHECKEQ(vcall(dev, DD_Unacquire, 0), DI_OK);
    CHECKEQ(vcall(dev, 2, 0), 0);
    CHECKEQ(vcall(di, 2, 0), 0);
}

static void write_text(const char *path, const char *text) {
    FILE *f = fopen(path, "w");
    if (!f) { printf("FAIL cannot write %s\n", path); fails++; return; }
    fputs(text, f);
    fclose(f);
}

/* script_reads: a read pushes every event up to its number not yet pushed, in file order; the
 * running point is clamped to the game window's client rect; buttons, wheel, keys and close. */
static void group_script_reads(const char *path) {
    write_text(path, "2 move 5000 -5000\n"
                     "2 down 1\n"
                     "4 move -10 10   # back inside\n"
                     "4 key 0x25 0x4B down ext\n"
                     "4 key 0x25 0x4B up ext\n"
                     "6 wheel 240\n"
                     "8 up 1\n"
                     "8 close\n");
    setenv("BW2_INPUT_SCRIPT", path, 1);
    make_window(0, 0, 0, 320, 240);                      /* the first window, not the game's */
    uint32_t a = make_window(0, 50, 50, 1024, 768);
    bw2_in_set_window(a);
    hle_proc_hook = recorder;
    pump_all();
    g_nss = 0;
    bw2_in_set_mouse_sink(script_sink);
    rec_reset();
    bw2_script_at_read(1);
    bw2_in_drain_input();
    CHECKEQ(g_nss, 0);
    bw2_script_at_read(3);                               /* read 2's events, at read 3 */
    bw2_in_drain_input();
    CHECKEQ(g_nss, 2);
    EXPECT_SUNK(0, BW2_IN_MOVE, 1023, 0, 5000, -5000);
    EXPECT_SUNK(1, BW2_IN_BUTTON, 1023, 0, 1, 1);
    bw2_script_at_read(3);                               /* nothing twice */
    bw2_in_drain_input();
    CHECKEQ(g_nss, 2);
    pump_all();
    EXPECT(2, WM_MOUSEMOVE, ANY, 0x000003FFu, WM_RBUTTONDOWN, ANY, 0x000003FFu);
    rec_reset();
    bw2_script_at_read(7);                               /* reads 4 and 6 */
    bw2_in_drain_input();
    CHECKEQ(g_nss, 4);
    EXPECT_SUNK(2, BW2_IN_MOVE, 1013, 10, -10, 10);
    EXPECT_SUNK(3, BW2_IN_WHEEL, 1013, 10, 240, 0);
    pump_all();
    EXPECT(4, WM_MOUSEMOVE, ANY, 0x000A03F5u, WM_KEYDOWN, 0x25u, 0x014B0001u, WM_KEYUP, 0x25u, 0xC14B0001u,
           WM_MOUSEWHEEL, ANY, ((60u << 16) | 1063u));
    if (g_nrec == 4) CHECKEQ(g_rec[3].w >> 16, 240);
    rec_reset();
    bw2_script_at_read(100);
    bw2_in_drain_input();
    CHECKEQ(g_nss, 5);
    EXPECT_SUNK(4, BW2_IN_BUTTON, 1013, 10, 1, 0);
    pump_all();
    EXPECT(3, WM_RBUTTONUP, ANY, 0x000A03F5u, WM_SYSCOMMAND, SC_CLOSE, 0u, WM_CLOSE, 0u, 0u);
    if (g_nrec == 3) CHECKEQ(g_rec[1].h, a);
    bw2_in_set_mouse_sink(bw2_dinput8_sink);
    hle_proc_hook = NULL;
}

/* script_nowin: with no window the running point is not clamped. */
static void group_script_nowin(const char *path) {
    write_text(path, "1 move -5 -7\n1 down 2\n2 move 3000 3000\n");
    setenv("BW2_INPUT_SCRIPT", path, 1);
    CHECKEQ(hle_first_hwnd(), 0);
    g_nss = 0;
    bw2_in_set_mouse_sink(script_sink);
    bw2_script_at_read(2);
    bw2_in_drain_input();
    CHECKEQ(g_nss, 3);
    EXPECT_SUNK(0, BW2_IN_MOVE, -5, -7, -5, -7);
    EXPECT_SUNK(1, BW2_IN_BUTTON, -5, -7, 2, 1);
    EXPECT_SUNK(2, BW2_IN_MOVE, 2995, 2993, 3000, 3000);
    bw2_in_set_mouse_sink(bw2_dinput8_sink);
}

/* ---- plan 11 Task 1: script blocks anchored on movies and wall time ---- */

/* The fake clock: the value the case sets before each read. */
static double g_fake_now;
static double fake_now(void) { return g_fake_now; }

/* stderr into a file, read back after each read and passed on to the real stderr, so a case sees
 * which read printed each script line. */
static int g_err_in = -1, g_err_orig = -1;
static void capture_stderr(const char *path) {
    fflush(stderr);
    g_err_orig = dup(2);
    int fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0644);
    if (fd < 0 || g_err_orig < 0) { printf("FAIL cannot capture stderr in %s\n", path); fails++; return; }
    dup2(fd, 2);
    close(fd);
    g_err_in = open(path, O_RDONLY);
}
/* The "[bw2] script:" lines printed since the last call, each ending in a newline. */
static const char *new_script_lines(void) {
    static char buf[1 << 16], out[1 << 16];
    out[0] = 0;
    if (g_err_in < 0) return out;
    fflush(stderr);
    ssize_t k = read(g_err_in, buf, sizeof buf - 1);
    if (k < 0) k = 0;
    buf[k] = 0;
    if (k && write(g_err_orig, buf, (size_t)k) < 0) { /* the echo is best effort */ }
    size_t o = 0;
    for (char *s = buf, *e; *s; s = e) {
        e = strchr(s, '\n');
        e = e ? e + 1 : s + strlen(s);
        if (!strncmp(s, "[bw2] script:", 13) && o + (size_t)(e - s) < sizeof out) {
            memcpy(out + o, s, (size_t)(e - s));
            o += (size_t)(e - s);
            out[o] = 0;
        }
    }
    return out;
}

struct after_read { int sunk; int keydown, keyup; char lines[512]; };

/* One real GetDeviceData read, numbered r, then the pump: what it pushed and printed. */
static struct after_read after_read(uint32_t dev, uint32_t buf, int r) {
    struct after_read a = { 0 };
    int before = g_nss;
    printf("script_after: read %d asked (%d failed so far)\n", r, fails);
    fflush(stdout);
    rec_reset();
    uint32_t n = 512;
    CHECKEQ(read_data(dev, 20, buf, &n, 0), DI_OK);
    CHECKEQ(bw2_dinput_reads(), r);
    pump_all();
    a.sunk = g_nss - before;
    a.keydown = rec_has(WM_KEYDOWN), a.keyup = rec_has(WM_KEYUP);
    snprintf(a.lines, sizeof a.lines, "%s", new_script_lines());
    printf("script_after: read %d answered (%d failed so far)\n", r, fails);
    fflush(stdout);
    return a;
}
/* A read that pushed `sunk` mouse events, the given keys, and printed exactly `lines`. */
static void expect_read(int line, int r, const struct after_read *a, int sunk, int kd, int ku, const char *lines) {
    if (a->sunk == sunk && a->keydown == kd && a->keyup == ku && !strcmp(a->lines, lines)) return;
    printf("FAIL %s:%d: read %d sunk %d (want %d), key down %d (%d), up %d (%d), lines '%s' (want '%s')\n",
           __FILE__, line, r, a->sunk, sunk, a->keydown, kd, a->keyup, ku, a->lines, lines);
    fails++;
}
#define EXPECT_READ(r, a, sunk, kd, ku, lines) expect_read(__LINE__, r, a, sunk, kd, ku, lines)

static void group_script_after(const char *which, const char *path) {
    static const struct { const char *name, *text; } cases[] = {
        { "anchor", "1 move 10 0\n2 down 0\nafter open 1798\n0 up 0\n5 move 1 0\n" },
        { "stale", "3 move 1 0\nafter   last  1798   # spaces normalised\n0 down 0\n" },
        { "empty_block", "1 down 0\nafter open 1798\nafter seconds 0.5\n0 up 0\n" },
        { "holds", "after seconds 0.5\n0 key 0x1B 0x01 down\nafter seconds 0.5\n0 key 0x1B 0x01 up\n" },
        { "holds_slow", "after seconds 0.5\n0 key 0x1B 0x01 down\nafter seconds 0.5\n0 key 0x1B 0x01 up\n" },
        { "quit", "5 quit\n" },
        /* plan 12 Task 1: file anchors */
        { "file", "1 down 0\nafter file M_SCROLL_BRONZE.bwm\n0 up 0\n" },
        { "file_stale", "1 down 0\nafter file M_SCROLL_BRONZE.bwm\n0 up 0\n" },
        { "file_window", "1 move 1 1\nafter file A.bwm\n0 down 0\n4 up 0\nafter file B.bwm\n0 down 1\n" },
        { "file_window_early", "1 move 1 1\nafter file A.bwm\n0 down 0\n4 up 0\nafter file B.bwm\n0 down 1\n" },
        { "file_same_frame", "after file A.bwm\nafter file B.bwm\n0 down 0\n" },
        { "file_slash", "after file Game.sav\n0 down 0\n" },
        { "file_max", "" },                              /* built below: a 259-character name */
        { "file_seen", "1 down 0\n" },
        { "file_overflow", "after seconds 1\nafter file A.bwm\n" },   /* the clock stays put: A never arms */
    };
    char maxname[260], maxtext[320];
    memset(maxname, 'F', 255);
    strcpy(maxname + 255, ".bwm");                      /* 259 characters, as tests/test_input.py builds it */
    snprintf(maxtext, sizeof maxtext, "after file %s\n0 down 0\n", maxname);
    const char *text = NULL;
    for (size_t i = 0; i < sizeof cases / sizeof *cases; i++)
        if (!strcmp(cases[i].name, which)) text = cases[i].text;
    if (!strcmp(which, "file_max")) text = maxtext;
    if (!text) { printf("FAIL no script_after case %s\n", which); fails++; return; }
    write_text(path, text);
    setenv("BW2_INPUT_SCRIPT", path, 1);
    bw2_script_set_clock(fake_now);
    char seen[260];
    if (!strcmp(which, "file_seen")) {                  /* before the load: counted, never an event */
        bw2_script_file(NULL);
        CHECKEQ(bw2_script_files_seen(seen, sizeof seen), 0);
        bw2_script_file("x\\y.dds");
        CHECKEQ(bw2_script_files_seen(seen, sizeof seen), 1);
        CHECKSTR(seen, "y.dds");
    }
    if (!strcmp(which, "file_stale")) bw2_script_file("data\\art\\models\\M_SCROLL_BRONZE.bwm");   /* no window yet */
    uint32_t di = new_di(), dev = game_mouse(di, 512);
    bw2_in_set_window(HWND);
    hle_proc_hook = recorder;
    pump_all();
    g_nss = 0;
    bw2_in_set_mouse_sink(script_sink);
    uint32_t buf = gmem(20 * 512);
    char errpath[1024];
    snprintf(errpath, sizeof errpath, "%s.stderr", path);
    if (strcmp(which, "quit") && strcmp(which, "file_overflow"))
        capture_stderr(errpath);                         /* the quit and the overflow never return to pass it on */
    struct after_read a;
    if (!strcmp(which, "anchor")) {
        for (int r = 1; r <= 12; r++) {
            a = after_read(dev, buf, r);
            if (r == 1 || r == 2 || r == 8) EXPECT_READ(r, &a, 1, 0, 0, "");
            else if (r == 4) EXPECT_READ(r, &a, 1, 0, 0, "[bw2] script: after open 1798 at read 3\n");
            else EXPECT_READ(r, &a, 0, 0, 0, "");
            if (r == 2) bw2_script_movie(0, 2033);       /* another movie: nothing */
            if (r == 3) bw2_script_movie(0, 1798);
            if (r == 9) bw2_script_movie(0, 1798);       /* again, with no block left: nothing */
        }
        CHECKEQ(g_nss, 4);
        EXPECT_SUNK(0, BW2_IN_MOVE, 10, 0, 10, 0);
        EXPECT_SUNK(1, BW2_IN_BUTTON, 10, 0, 0, 1);
        EXPECT_SUNK(2, BW2_IN_BUTTON, 10, 0, 0, 0);
        EXPECT_SUNK(3, BW2_IN_MOVE, 11, 0, 1, 0);
    } else if (!strcmp(which, "stale")) {
        for (int r = 1; r <= 8; r++) {
            a = after_read(dev, buf, r);
            if (r == 3) EXPECT_READ(r, &a, 1, 0, 0, "");
            else if (r == 7) EXPECT_READ(r, &a, 1, 0, 0, "[bw2] script: after last 1798 at read 6\n");
            else EXPECT_READ(r, &a, 0, 0, 0, "");
            if (r == 1 || r == 2) bw2_script_movie(1, 1798);   /* before the arming call (read 3): stale */
            if (r == 4) bw2_script_movie(0, 1798);             /* an open, not a last frame */
            if (r == 5) bw2_script_movie(1, 2033);             /* another movie's last frame */
            if (r == 6) bw2_script_movie(1, 1798);
        }
        CHECKEQ(g_nss, 2);
        EXPECT_SUNK(1, BW2_IN_BUTTON, 1, 0, 0, 1);
    } else if (!strcmp(which, "empty_block")) {
        for (int r = 1; r <= 10; r++) {
            g_fake_now = r <= 5 ? r : 10.0 + 0.25 * (r - 6);
            a = after_read(dev, buf, r);
            if (r == 1) EXPECT_READ(r, &a, 1, 0, 0, "");
            else if (r == 6) EXPECT_READ(r, &a, 0, 0, 0, "[bw2] script: after open 1798 at read 5\n");
            else if (r == 8) EXPECT_READ(r, &a, 1, 0, 0, "[bw2] script: after seconds 0.5 at read 8\n");
            else EXPECT_READ(r, &a, 0, 0, 0, "");
            if (r == 5) bw2_script_movie(0, 1798);
        }
        CHECKEQ(g_nss, 2);
        EXPECT_SUNK(1, BW2_IN_BUTTON, 0, 0, 0, 0);
    } else if (!strcmp(which, "holds") || !strcmp(which, "holds_slow")) {
        int slow = which[5] != 0, down_at = slow ? 2 : 513, up_at = slow ? 3 : 1025, last = slow ? 6 : 1100;
        char dl[96], ul[96];
        snprintf(dl, sizeof dl, "[bw2] script: after seconds 0.5 at read %d\n", down_at);
        snprintf(ul, sizeof ul, "[bw2] script: after seconds 0.5 at read %d\n", up_at);
        for (int r = 1; r <= last; r++) {
            g_fake_now = slow ? r : r / 1024.0;
            a = after_read(dev, buf, r);
            if (r == down_at) EXPECT_READ(r, &a, 0, 1, 0, dl);
            else if (r == up_at) EXPECT_READ(r, &a, 0, 0, 1, ul);
            else EXPECT_READ(r, &a, 0, 0, 0, "");
        }
    } else if (!strcmp(which, "file")) {
        for (int r = 1; r <= 6; r++) {
            a = after_read(dev, buf, r);
            if (r == 1) EXPECT_READ(r, &a, 1, 0, 0, "");
            else if (r == 4) EXPECT_READ(r, &a, 1, 0, 0, "[bw2] script: after file M_SCROLL_BRONZE.bwm at read 3\n");
            else EXPECT_READ(r, &a, 0, 0, 0, "");
            if (r == 2) bw2_script_file("data\\art\\models\\M_OTHER.bwm");           /* another name: nothing */
            if (r == 3) bw2_script_file("data\\art\\models\\m_scroll_bronze.BWM");   /* any case */
        }
        CHECKEQ(g_nss, 2);
        EXPECT_SUNK(1, BW2_IN_BUTTON, 0, 0, 0, 0);
    } else if (!strcmp(which, "file_stale")) {
        for (int r = 1; r <= 4; r++) {
            a = after_read(dev, buf, r);
            if (r == 1) EXPECT_READ(r, &a, 1, 0, 0, "");
            else if (r == 3) EXPECT_READ(r, &a, 1, 0, 0, "[bw2] script: after file M_SCROLL_BRONZE.bwm at read 2\n");
            else EXPECT_READ(r, &a, 0, 0, 0, "");
            if (r == 2) bw2_script_file("data\\art\\models\\M_SCROLL_BRONZE.bwm");
        }
        CHECKEQ(g_nss, 2);
    } else if (!strcmp(which, "file_window")) {
        for (int r = 1; r <= 7; r++) {
            a = after_read(dev, buf, r);
            if (r == 1) EXPECT_READ(r, &a, 1, 0, 0, "");
            else if (r == 3) EXPECT_READ(r, &a, 1, 0, 0, "[bw2] script: after file A.bwm at read 2\n");
            else if (r == 6) EXPECT_READ(r, &a, 2, 0, 0, "[bw2] script: after file B.bwm at read 4\n");
            else EXPECT_READ(r, &a, 0, 0, 0, "");
            if (r == 2) bw2_script_file("A.bwm");
            if (r == 4) bw2_script_file("B.bwm");       /* after the down, before its up: the B block is not armed */
        }
        CHECKEQ(g_nss, 4);
        EXPECT_SUNK(0, BW2_IN_MOVE, 1, 1, 1, 1);
        EXPECT_SUNK(1, BW2_IN_BUTTON, 1, 1, 0, 1);
        EXPECT_SUNK(2, BW2_IN_BUTTON, 1, 1, 0, 0);
        EXPECT_SUNK(3, BW2_IN_BUTTON, 1, 1, 1, 1);
    } else if (!strcmp(which, "file_window_early")) {
        for (int r = 1; r <= 9; r++) {
            a = after_read(dev, buf, r);
            if (r == 1 || r == 6) EXPECT_READ(r, &a, 1, 0, 0, "");
            else if (r == 3) EXPECT_READ(r, &a, 1, 0, 0, "[bw2] script: after file A.bwm at read 2\n");
            else if (r == 8) EXPECT_READ(r, &a, 1, 0, 0, "[bw2] script: after file B.bwm at read 7\n");
            else EXPECT_READ(r, &a, 0, 0, 0, "");
            if (r == 1) bw2_script_file("B.bwm");       /* before the A block's anchor read: outside B's window */
            if (r == 2) bw2_script_file("A.bwm");
            if (r == 7) bw2_script_file("B.bwm");
        }
        CHECKEQ(g_nss, 4);
        EXPECT_SUNK(3, BW2_IN_BUTTON, 1, 1, 1, 1);
    } else if (!strcmp(which, "file_same_frame")) {
        for (int r = 1; r <= 4; r++) {
            a = after_read(dev, buf, r);
            if (r == 3)
                EXPECT_READ(r, &a, 1, 0, 0,
                            "[bw2] script: after file A.bwm at read 2\n[bw2] script: after file B.bwm at read 2\n");
            else EXPECT_READ(r, &a, 0, 0, 0, "");
            if (r == 2) bw2_script_file("A.bwm"), bw2_script_file("B.bwm");
        }
        CHECKEQ(g_nss, 1);
    } else if (!strcmp(which, "file_slash") || !strcmp(which, "file_max")) {
        int max = which[5] == 'm';
        char want[400], path2[300];
        snprintf(want, sizeof want, "[bw2] script: after file %s at read 1\n", max ? maxname : "Game.sav");
        snprintf(path2, sizeof path2, "%s", max ? "data\\" : "a/b/Game.sav");
        if (max) strcat(path2, maxname);
        for (int r = 1; r <= 3; r++) {
            a = after_read(dev, buf, r);
            if (r == 2) EXPECT_READ(r, &a, 1, 0, 0, want);
            else EXPECT_READ(r, &a, 0, 0, 0, "");
            if (r == 1) bw2_script_file(path2);
        }
        CHECKEQ(g_nss, 1);
        CHECKEQ(bw2_script_files_seen(seen, sizeof seen), 1);
        CHECKSTR(seen, max ? maxname : "Game.sav");
    } else if (!strcmp(which, "file_seen")) {
        a = after_read(dev, buf, 1);
        EXPECT_READ(1, &a, 1, 0, 0, "");
        bw2_script_file(NULL);                          /* after the load too: not counted */
        CHECKEQ(bw2_script_files_seen(seen, sizeof seen), 1);
        bw2_script_file("dir\\");                      /* an empty name: counted, no event */
        CHECKEQ(bw2_script_files_seen(seen, sizeof seen), 2);
        CHECKSTR(seen, "");
        bw2_script_file("a/b\\c.bin");
        CHECKEQ(bw2_script_files_seen(NULL, 0), 3);
        CHECKEQ(bw2_script_files_seen(seen, sizeof seen), 3);
        CHECKSTR(seen, "c.bin");
    } else if (!strcmp(which, "file_overflow")) {
        g_fake_now = 0;
        for (int r = 1; r <= 257; r++) {                /* one lookup per read: 256 are kept, the 257th stops */
            a = after_read(dev, buf, r);
            bw2_script_file("A.bwm");
            bw2_script_file("a.BWM");                   /* the same name at the same read: kept once */
        }
        printf("script_after: returned\n");
    } else if (!strcmp(which, "quit")) {
        for (int r = 1; r <= 8; r++) {
            a = after_read(dev, buf, r);
            EXPECT_READ(r, &a, 0, 0, 0, "");
        }
        printf("script_after: returned\n");
    }
    bw2_script_set_clock(NULL);
    bw2_in_set_mouse_sink(bw2_dinput8_sink);
    hle_proc_hook = NULL;
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
    else if (!strcmp(argv[1], "di_quit")) group_di_quit(argc > 2 ? argv[2] : NULL);
    else if (!strcmp(argv[1], "queue")) group_queue();
    else if (!strcmp(argv[1], "mouse_msgs")) group_mouse_msgs();
    else if (!strcmp(argv[1], "wheel_msgs")) group_wheel_msgs();
    else if (!strcmp(argv[1], "key_msgs")) group_key_msgs();
    else if (!strcmp(argv[1], "activate")) group_activate();
    else if (!strcmp(argv[1], "close")) group_close();
    else if (!strcmp(argv[1], "drain_points")) group_drain_points();
    else if (!strcmp(argv[1], "imm")) group_imm();
    else if (!strcmp(argv[1], "di_format")) group_di_format();
    else if (!strcmp(argv[1], "di_data")) group_di_data();
    else if (!strcmp(argv[1], "di_state")) group_di_state();
    else if (!strcmp(argv[1], "di_overflow")) group_di_overflow();
    else if (!strcmp(argv[1], "focus_loss")) group_focus_loss();
    else if (!strcmp(argv[1], "di_exclusive")) group_di_exclusive();
    else if (!strcmp(argv[1], "keymap")) group_keymap();
    else if (!strcmp(argv[1], "geom_letterbox")) group_geom_letterbox();
    else if (!strcmp(argv[1], "deltas")) group_deltas();
    else if (!strcmp(argv[1], "scroll_precise")) group_scroll_precise();
    else if (!strcmp(argv[1], "option_middle")) group_option_middle();
    else if (!strcmp(argv[1], "cmd_keys")) group_cmd_keys();
    else if (!strcmp(argv[1], "caps")) group_caps();
    else if (!strcmp(argv[1], "mouse_events")) group_mouse_events();
    else if (!strcmp(argv[1], "translate_reset")) group_translate_reset();
    else if (!strcmp(argv[1], "window_size")) group_window_size();
    else if (!strcmp(argv[1], "cursor_policy")) group_cursor_policy();
    else if (!strcmp(argv[1], "mode_switch")) group_mode_switch();
    else if (!strcmp(argv[1], "cursor_warp")) group_cursor_warp();
    else if (!strcmp(argv[1], "clip")) group_clip();
    else if (!strcmp(argv[1], "cursor_image")) group_cursor_image();
    else if (!strcmp(argv[1], "script")) group_script();
    else if (!strcmp(argv[1], "script_reads") && argc > 2) group_script_reads(argv[2]);
    else if (!strcmp(argv[1], "script_nowin") && argc > 2) group_script_nowin(argv[2]);
    else if (!strcmp(argv[1], "script_after") && argc > 3) group_script_after(argv[2], argv[3]);
    else { fprintf(stderr, "input_selftest: no group %s\n", argv[1]); return 2; }
    printf("%s: %d failed\n", argv[1], fails);
    fflush(stdout);
    return fails != 0;
}
