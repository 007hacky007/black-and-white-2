/*
 * shims_k32.c - kernel32 imports win32hle does not answer, as BW2 calls them
 * (each one checked in shims_selftest.c first).
 */
#include <stdio.h>
#include <string.h>
#include "win32hle.h"
#include "bw2.h"

/* win32hle's answer to `name` (which pops its own arguments), then `extra`
 * more argument dwords: an extended form whose extra arguments change nothing. */
static void forward_more(const char *name, int extra) {
    bw2_hle_shim(name)();
    g_esp += 4u * (uint32_t)extra;
}

/* InitializeCriticalSectionAndSpinCount(cs, spin): the spin count is a hint. TRUE. */
static void k_InitializeCriticalSectionAndSpinCount(void) {
    forward_more("InitializeCriticalSection", 1);
    g_eax = 1;
}

/* GetSystemPowerStatus(status): on AC power with no system battery, so the
 * game never picks a battery-saving path. TRUE. */
static void k_GetSystemPowerStatus(void) {
    uint32_t p = A32(0);
    if (p) {
        MEM8(p) = 1;                  /* ACLineStatus: online */
        MEM8(p + 1) = 128;            /* BatteryFlag: no system battery */
        MEM8(p + 2) = 255;            /* BatteryLifePercent: unknown */
        MEM8(p + 3) = 0;
        MEM32(p + 4) = 0xFFFFFFFFu;   /* BatteryLifeTime: unknown */
        MEM32(p + 8) = 0xFFFFFFFFu;   /* BatteryFullLifeTime: unknown */
    }
    RET(1, 1);
}

/* HeapSetInformation(heap, class, info, len): the low-fragmentation-heap
 * switch and the like are hints this heap takes as done. TRUE. */
static void k_HeapSetInformation(void) { RET(1, 4); }

const win32hle_shim bw2_k32_shims[] = {
    { "HeapSetInformation",                    k_HeapSetInformation },
    { "GetSystemPowerStatus",                  k_GetSystemPowerStatus },
    { "InitializeCriticalSectionAndSpinCount", k_InitializeCriticalSectionAndSpinCount },
    { NULL, NULL }
};
