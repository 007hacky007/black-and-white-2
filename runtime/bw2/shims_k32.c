/*
 * shims_k32.c - kernel32 imports win32hle does not answer, as BW2 calls them
 * (each one checked in shims_selftest.c first).
 */
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
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

/* ---- ReadFileEx and the alertable waits ----
 * The game's streaming reads (0xB524F0) use ReadFileEx on a file opened with
 * FILE_FLAG_OVERLAPPED, then SleepEx(.., TRUE) or WaitForSingleObjectEx(..,
 * TRUE) to run the completion routines. The read is done at once, at the
 * OVERLAPPED's offset; its routine is queued for the calling thread and runs
 * at that thread's next alertable wait, as an APC does on Windows. Only the
 * thread itself queues to its list, so a wait need only look at entry. */
struct apc { uint32_t routine, error, bytes, ovl; };
static __thread struct apc t_apc[64];
static __thread int t_napc;

/* ReadFileEx(file, buf, n, overlapped, routine). TRUE with the routine queued;
 * FALSE with ERROR_HANDLE_EOF at or past the end, ERROR_INVALID_HANDLE for a
 * handle that is not a file. */
static void k_ReadFileEx(void) {
    uint32_t h = A32(0), buf = A32(1), n = A32(2), ovl = A32(3), routine = A32(4);
    int fd = hle_file_fd(h);
    if (fd < 0) { hle_set_last_error(6); RET(0, 5); }               /* ERROR_INVALID_HANDLE */
    if (!ovl || !routine || (n && !buf)) { hle_set_last_error(87); RET(0, 5); }   /* ERROR_INVALID_PARAMETER */
    off_t at = (off_t)((uint64_t)MEM32(ovl + 12) << 32 | MEM32(ovl + 8)), end = lseek(fd, 0, SEEK_END);
    if (n && at >= end) { hle_set_last_error(38); RET(0, 5); }       /* ERROR_HANDLE_EOF */
    uint32_t got = 0;
    while (got < n) {
        ssize_t r = pread(fd, (uint8_t *)hle_ptr(buf) + got, n - got, at + got);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) break;
        got += (uint32_t)r;
    }
    if (t_napc == (int)(sizeof t_apc / sizeof t_apc[0])) hle_fatal("ReadFileEx: more than %d completions queued", t_napc);
    MEM32(ovl) = 0, MEM32(ovl + 4) = got;                             /* Internal STATUS_SUCCESS, InternalHigh */
    t_apc[t_napc++] = (struct apc){ routine, 0, got, ovl };
    hle_set_last_error(0);
    RET(1, 5);
}

/* Runs the thread's queued completion routines, oldest first; 1 if any ran. */
static int run_apcs(void) {
    int ran = 0;
    while (t_napc) {
        struct apc a = t_apc[0];
        memmove(t_apc, t_apc + 1, (size_t)--t_napc * sizeof t_apc[0]);
        uint32_t args[3] = { a.error, a.bytes, a.ovl };
        hle_call_guest(a.routine, 3, args);
        ran = 1;
    }
    return ran;
}

/* SleepEx(ms, alertable): WAIT_IO_COMPLETION at once when routines ran, else Sleep. */
static void k_SleepEx(void) {
    if (A32(1) && run_apcs()) RET(0xC0, 2);
    bw2_hle_shim("SleepEx")();
}

/* WaitForSingleObjectEx(h, ms, alertable): WAIT_IO_COMPLETION when routines ran, else the wait. */
static void k_WaitForSingleObjectEx(void) {
    if (A32(2) && run_apcs()) RET(0xC0, 3);
    bw2_hle_shim("WaitForSingleObjectEx")();
}

const win32hle_shim bw2_k32_shims[] = {
    { "ReadFileEx",                            k_ReadFileEx },
    { "SleepEx",                               k_SleepEx },
    { "WaitForSingleObjectEx",                 k_WaitForSingleObjectEx },
    { "GetProcessWorkingSetSize",              k_GetProcessWorkingSetSize },
    { "SetProcessWorkingSetSize",              k_SetProcessWorkingSetSize },
    { "GlobalMemoryStatusEx",                  k_GlobalMemoryStatusEx },
    { "GlobalMemoryStatus",                    k_GlobalMemoryStatus },
    { "HeapSetInformation",                    k_HeapSetInformation },
    { "GetSystemPowerStatus",                  k_GetSystemPowerStatus },
    { "InitializeCriticalSectionAndSpinCount", k_InitializeCriticalSectionAndSpinCount },
    { NULL, NULL }
};
