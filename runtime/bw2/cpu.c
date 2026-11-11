/*
 * cpu.c - the CPU the guest sees on Apple silicon (plan 3 Task 4).
 *
 * CPUID answers a fixed virtual CPU: GenuineIntel, family 6 model 15, with
 * FPU through CLFSH, MMX, FXSR, SSE and SSE2 (what the lifter implements; no
 * SSE3, no HTT, no 3DNow!), and the brand "BW2 virtual CPU @ 3.00GHz".
 *
 * RDTSC counts at a fixed 3 GHz: nanoseconds of a monotonic clock times 3, so
 * it keeps real time. A plain nanosecond count would read as a 1 GHz CPU,
 * below BW2's minimum of about 1.6 GHz; a game that sets its detail level
 * from the clock it measures would pick low settings and save them (spec 7.1).
 */
#define RECOMP_HOST_CPU      /* this file defines recomp_rdtsc and recomp_cpuid */
#include <string.h>
#include <time.h>
#include "recomp_types.h"

#define BW2_TSC_PER_NS 3ull          /* 3 GHz */

uint64_t recomp_rdtsc(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ((uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec) * BW2_TSC_PER_NS;
}

static const char brand[48] = "BW2 virtual CPU @ 3.00GHz";

void recomp_cpuid(uint32_t leaf, uint32_t subleaf) {
    (void)subleaf;
    uint32_t r[4] = { 0, 0, 0, 0 };                     /* eax, ebx, ecx, edx */
    switch (leaf) {
    case 0:
        r[0] = 1, r[1] = 0x756E6547u, r[3] = 0x49656E69u, r[2] = 0x6C65746Eu;   /* GenuineIntel */
        break;
    case 1:
        r[0] = 0x000006F6u, r[1] = 0x00010800u, r[2] = 0, r[3] = 0x0789FBFFu;
        break;
    case 0x80000000u:
        r[0] = 0x80000004u;
        break;
    case 0x80000002u: case 0x80000003u: case 0x80000004u:
        memcpy(r, brand + 16 * (leaf - 0x80000002u), 16);
        break;
    default:
        break;
    }
    g_eax = r[0], g_ebx = r[1], g_ecx = r[2], g_edx = r[3];
}
