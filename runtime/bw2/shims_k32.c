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

/* The memory the game is told it has, the same through both calls: 2 GB of RAM
 * with 1.5 GB free, a 4 GB page file, the 2 GB user address space. BW2 asks for
 * 512 MB and recommends 1 GB; like the CPU's clock (cpu.c), a small answer
 * could make it pick and save low settings. */
#define PHYS_TOTAL  0x80000000ull
#define PHYS_FREE   0x60000000ull
#define PAGE_TOTAL  0x100000000ull
#define PAGE_FREE   0xC0000000ull
#define VIRT_TOTAL  0x7FFE0000ull
#define VIRT_FREE   0x60000000ull

static void put64(uint32_t va, uint64_t v) { MEM32(va) = (uint32_t)v; MEM32(va + 4) = (uint32_t)(v >> 32); }

/* GlobalMemoryStatusEx(&MEMORYSTATUSEX): TRUE. */
static void k_GlobalMemoryStatusEx(void) {
    uint32_t m = A32(0);
    if (!m || MEM32(m) < 64) { hle_set_last_error(87u /* ERROR_INVALID_PARAMETER */); RET(0, 1); }
    MEM32(m + 4) = 25;                                 /* dwMemoryLoad, % */
    put64(m + 8, PHYS_TOTAL), put64(m + 16, PHYS_FREE);
    put64(m + 24, PAGE_TOTAL), put64(m + 32, PAGE_FREE);
    put64(m + 40, VIRT_TOTAL), put64(m + 48, VIRT_FREE);
    put64(m + 56, 0);                                  /* ullAvailExtendedVirtual */
    RET(1, 1);
}

/* GlobalMemoryStatus(&MEMORYSTATUS): the same, in 32-bit fields (capped as Windows caps them). */
static void k_GlobalMemoryStatus(void) {
    uint32_t m = A32(0);
    if (m) {
        MEM32(m) = 32, MEM32(m + 4) = 25;
        MEM32(m + 8) = (uint32_t)PHYS_TOTAL, MEM32(m + 12) = (uint32_t)PHYS_FREE;
        MEM32(m + 16) = 0xFFFFFFFFu, MEM32(m + 20) = (uint32_t)PAGE_FREE;
        MEM32(m + 24) = (uint32_t)VIRT_TOTAL, MEM32(m + 28) = (uint32_t)VIRT_FREE;
    }
    RETV(1);
}

/* Get/SetProcessWorkingSetSize(process, min, max): Windows' defaults for the
 * read; a write succeeds and changes nothing (macOS pages as it will). TRUE. */
static void k_GetProcessWorkingSetSize(void) {
    if (A32(1)) MEM32(A32(1)) = 204800u;
    if (A32(2)) MEM32(A32(2)) = 1413120u;
    RET(1, 3);
}
static void k_SetProcessWorkingSetSize(void) { RET(1, 3); }

const win32hle_shim bw2_k32_shims[] = {
    { "GetProcessWorkingSetSize",              k_GetProcessWorkingSetSize },
    { "SetProcessWorkingSetSize",              k_SetProcessWorkingSetSize },
    { "GlobalMemoryStatusEx",                  k_GlobalMemoryStatusEx },
    { "GlobalMemoryStatus",                    k_GlobalMemoryStatus },
    { "HeapSetInformation",                    k_HeapSetInformation },
    { "GetSystemPowerStatus",                  k_GetSystemPowerStatus },
    { "InitializeCriticalSectionAndSpinCount", k_InitializeCriticalSectionAndSpinCount },
    { NULL, NULL }
};
