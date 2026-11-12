/*
 * input_selftest.c - the DirectInput mouse and the host input queue driven as
 * lifted code drives them: arguments pushed right to left, a return address,
 * then the shim; COM methods through the guest vtable. Each check also
 * verifies the stdcall pop. The queue groups (plan 6) push host events and
 * pump the messages as the game does (PeekMessageA, TranslateMessage,
 * DispatchMessageA), recording what the window procedures are given.
 * Built with the engine (minus main.c) by tests/test_input.py;
 * `input_selftest <group> [arg]` runs one group and prints "<group>: <n> failed".
 */
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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
    bw2_in_set_mouse_sink(NULL);
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
    EXPECT(1, WM_CLOSE, 0u, 0u);
    if (g_nrec) CHECKEQ(g_rec[0].h, a);
    bw2_in_set_window(b);
    rec_reset();
    bw2_in_push(&e);
    bw2_in_drain_input();                                /* a close is posted: a read may drain it */
    CHECKEQ(g_nrec, 0);
    pump_all();
    EXPECT(1, WM_CLOSE, 0u, 0u);
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
    else { fprintf(stderr, "input_selftest: no group %s\n", argv[1]); return 2; }
    printf("%s: %d failed\n", argv[1], fails);
    fflush(stdout);
    return fails != 0;
}
