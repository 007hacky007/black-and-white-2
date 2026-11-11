/*
 * shims_selftest.c - BW2's own import answers, called as lifted code calls
 * them: arguments pushed right to left, a return address, the shim. Each
 * check also verifies the shim popped exactly its stdcall arguments. Built
 * with the engine (minus main.c) by tests/test_engine.py test_shims_selftest.
 * Every shim the boot needs gets its check here first (plan 3 Task 10).
 */
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include "win32hle.h"
#include "bw2.h"

const recomp_dispatch_entry_t recomp_dispatch_table[1] = { { 0, 0 } };
const uint32_t recomp_dispatch_count = 0;

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

static uint32_t call(const char *name, int n, ...) {
    uint32_t va = hle_resolve(name);
    if (!va) { printf("FAIL %s: not answered\n", name); fails++; return 0xDEADBEEFu; }
    recomp_func_t f = recomp_lookup_import(va);
    uint32_t a[16];
    va_list ap;
    va_start(ap, n);
    for (int i = 0; i < n; i++) a[i] = va_arg(ap, uint32_t);
    va_end(ap);
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

int main(void) {
    hle_mem_init();
    bw2_register_shims();
    mach_enter();

    /* kernel32 */
    {
        uint32_t cs = gmem(64);
        CHECK(call("InitializeCriticalSectionAndSpinCount", 2, cs, 4000u) == 1);
        call("EnterCriticalSection", 1, cs);
        call("LeaveCriticalSection", 1, cs);
        call("DeleteCriticalSection", 1, cs);
    }

    {
        uint32_t ps = gmem(16);
        CHECK(call("GetSystemPowerStatus", 1, ps) == 1 && MEM8(ps) == 1 && MEM8(ps + 1) == 128);   /* on AC, no battery */
    }

    {
        uint32_t v = gmem(4);
        MEM32(v) = 2;                                               /* HEAP_LFH */
        CHECK(call("HeapSetInformation", 4, call("GetProcessHeap", 0), 0u, v, 4u) == 1);
    }

    {
        uint32_t m = gmem(64);
        MEM32(m) = 64;
        CHECK(call("GlobalMemoryStatusEx", 1, m) == 1 && MEM32(m + 8) == 0x80000000u && MEM32(m + 12) == 0);  /* 2 GB */
        CHECK(MEM32(m + 16) == 0x60000000u);                                            /* 1.5 GB free */
        uint32_t o = gmem(32);
        call("GlobalMemoryStatus", 1, o);
        CHECK(MEM32(o + 8) == 0x80000000u && MEM32(o + 12) == 0x60000000u);             /* the same answer */
    }

    {
        uint32_t lo = gmem(4), hi = gmem(4);
        CHECK(call("GetProcessWorkingSetSize", 3, 0xFFFFFFFFu, lo, hi) == 1 && MEM32(lo) == 204800u && MEM32(hi) == 1413120u);
        CHECK(call("SetProcessWorkingSetSize", 3, 0xFFFFFFFFu, 64u << 20, 256u << 20) == 1);
    }

    /* user32 */
    {
        uint32_t v = gmem(64);
        CHECK(call("SystemParametersInfoA", 4, 0x70u, 0u, v, 0u) == 1 && MEM32(v) == 10);   /* SPI_GETMOUSESPEED */
        CHECK(call("SystemParametersInfoA", 4, 0x71u, 0u, 15u, 0u) == 1);                  /* SPI_SETMOUSESPEED */
        CHECK(call("SystemParametersInfoA", 4, 0x03u, 0u, v, 0u) == 1 && MEM32(v + 8) == 1); /* SPI_GETMOUSE */
        MEM32(v) = 8, MEM32(v + 4) = 0xFFFFFFFFu;
        CHECK(call("SystemParametersInfoA", 4, 0x3Au, 8u, v, 0u) == 1 && MEM32(v + 4) == 0); /* SPI_GETSTICKYKEYS */
        CHECK(call("SystemParametersInfoA", 4, 0x3Bu, 8u, v, 0u) == 1);
    }

    {
        uint32_t tme = gmem(16);
        MEM32(tme) = 16, MEM32(tme + 4) = 2;                       /* TME_LEAVE */
        CHECK(call("_TrackMouseEvent", 1, tme) == 1);
        CHECK(call("GetDoubleClickTime", 0) == 500);
        CHECK(call("GetKeyboardLayout", 1, 0u) == 0x04090409u);
        uint32_t l = gmem(16);
        CHECK(call("GetKeyboardLayoutList", 2, 0u, 0u) == 1);
        CHECK(call("GetKeyboardLayoutList", 2, 4u, l) == 1 && MEM32(l) == 0x04090409u);
    }

    printf("shims selftest: %d failed\n", fails);
    return fails != 0;
}
