/*
 * threads.c - guest threads as BW2 needs them (spec 7.2):
 *
 *   - big host stacks: lifted code nests one host frame per guest call, so
 *     the guest's main thread runs on a 1 GB host stack and every
 *     CreateThread thread on 256 MB (pthread's default for a secondary
 *     thread is 512 KB; one lifted white.exe function alone has a 557 KB frame);
 *   - an alternate signal stack on every thread, so a host stack overflow
 *     still reports which guest function it was in;
 *   - static TLS from each image's TLS directory: every thread gets its own
 *     copy of each module's template, the module's index is written to its
 *     AddressOfIndex, and TEB+0x2C points at the thread's slot array.
 */
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "win32hle.h"
#include "bw2.h"

#define MAIN_STACK   (1ull << 30)
#define WORKER_STACK (256ull << 20)
#define ALT_STACK    (256u << 10)
#define MAX_TLS      16

static struct { uint32_t start, end, index_va, zero_fill; } g_tls[MAX_TLS];
static int g_ntls;

/* The image at base's TLS directory, if it has one: its module gets the next index. */
int bw2_tls_add_module(uint32_t base) {
    uint32_t nt = base + MEM32(base + 0x3C);
    uint32_t dir_rva = MEM32(nt + 0x18 + 96 + 9 * 8), dir_size = MEM32(nt + 0x18 + 96 + 9 * 8 + 4);
    if (!dir_rva || dir_size < 24) return -1;
    if (g_ntls == MAX_TLS) hle_fatal("more than %d modules with TLS", MAX_TLS);
    uint32_t d = base + dir_rva;
    int idx = g_ntls++;
    g_tls[idx].start = MEM32(d);
    g_tls[idx].end = MEM32(d + 4);
    g_tls[idx].index_va = MEM32(d + 8);
    g_tls[idx].zero_fill = MEM32(d + 16);
    if (MEM32(d + 12) && MEM32(MEM32(d + 12)))
        hle_fatal("image at 0x%08X has TLS callbacks; not supported", base);
    if (g_tls[idx].index_va) MEM32(g_tls[idx].index_va) = (uint32_t)idx;
    return idx;
}

static void crash(int sig, siginfo_t *si, void *uc) {
    (void)uc;
    char msg[512];
    uintptr_t addr = si ? (uintptr_t)si->si_addr : 0, base = (uintptr_t)g_mem_base;
    int n = snprintf(msg, sizeof msg, "[bw2] FATAL: signal %d at host address %p", sig, (void *)addr);
    if (addr - base < (1ull << 32)) n += snprintf(msg + n, sizeof msg - (size_t)n, " (guest 0x%08X)", (uint32_t)(addr - base));
    n += snprintf(msg + n, sizeof msg - (size_t)n,
                  " in guest sub_%08X (last import %s)\n[bw2] eax=%08X ebx=%08X ecx=%08X edx=%08X esi=%08X edi=%08X "
                  "ebp=%08X esp=%08X\n[bw2] guest stack:",
                  g_cur_func, g_cur_import ? g_cur_import : "-", g_eax, g_ebx, g_ecx, g_edx, g_esi, g_edi, g_ebp, g_esp);
    write(2, msg, (size_t)n);
    for (int i = 0; i < 24; i++) {                      /* the stack's top words, if mapped */
        uint32_t a = g_esp + 4u * (uint32_t)i;
        if (a < 0x10000u) break;
        n = snprintf(msg, sizeof msg, " %08X", MEM32(a));
        write(2, msg, (size_t)n);
    }
    write(2, "\n", 1);
    signal(sig, SIG_DFL);
    raise(sig);
}

static void alt_stack(void) {
    stack_t ss = { .ss_sp = malloc(ALT_STACK), .ss_size = ALT_STACK, .ss_flags = 0 };
    if (ss.ss_sp) sigaltstack(&ss, NULL);
}

/* Each thread's first moment under the machine (hle_new_thread_hook). */
static void new_thread(uint32_t tib) {
    alt_stack();
    if (!g_ntls) return;
    uint32_t slots = hle_va(hle_alloc(4 * MAX_TLS));
    for (int i = 0; i < g_ntls; i++) {
        uint32_t len = g_tls[i].end - g_tls[i].start;
        uint32_t blk = hle_va(hle_alloc(len + g_tls[i].zero_fill + 16));    /* zeroed */
        memcpy(hle_ptr(blk), hle_ptr(g_tls[i].start), len);
        MEM32(slots + 4 * (uint32_t)i) = blk;
    }
    MEM32(tib + 0x2C) = slots;
}

void bw2_threads_init(void) {
    hle_thread_stack_size = WORKER_STACK;
    hle_new_thread_hook = new_thread;
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = crash;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGBUS, &sa, NULL);
    sigaction(SIGILL, &sa, NULL);
    sigaction(SIGFPE, &sa, NULL);
}

static void *run_main(void *p) {
    uint32_t (*fn)(void) = (uint32_t (*)(void))p;
    void *r = (void *)(uintptr_t)fn();
    bw2_mac_guest_done();
    return r;
}

/* fn on a thread with the guest main thread's big stack; its result. */
uint32_t bw2_run_on_main_stack(uint32_t (*fn)(void)) {
    pthread_attr_t attr;
    pthread_t t;
    void *ret = NULL;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, MAIN_STACK);
    if (pthread_create(&t, &attr, run_main, (void *)fn) != 0) hle_fatal("no thread for the guest's main stack");
    pthread_attr_destroy(&attr);
    (void)ret;
    return bw2_mac_main_loop(t);          /* the main thread serves the window meanwhile (mac_main.m) */
}
