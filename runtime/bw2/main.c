/*
 * main.c - the BW2 engine's process entry: arguments, the shim modules, the
 * boot of the game image and its run.
 *
 *   bw2 --game DIR [--exe white.exe] [--strict] [--trace-eh] [--lifted-dll NAME=0xBASE]...
 *
 * The support folder (saves, settings, registry, everything the game writes)
 * is $BW2_SUPPORT, or ~/Library/Application Support/Black & White 2.
 *
 * The bind is permissive: an import nothing answers is bound to a stub that
 * stops the run, naming itself, when it is first called (most of the game's
 * unanswered imports never run). --strict lists them all and exits 2 instead.
 */
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "win32hle.h"
#include "host.h"
#include "bw2.h"

/* A dword of path's PE optional header (0x1C ImageBase, 0x38 SizeOfImage). */
static uint32_t pe_field(const char *path, uint32_t opt_off) {
    FILE *f = fopen(path, "rb");
    uint32_t lfanew = 0, v = 0;
    if (f && fseek(f, 0x3C, SEEK_SET) == 0 && fread(&lfanew, 4, 1, f) == 1 &&
        fseek(f, (long)(lfanew + 0x18 + opt_off), SEEK_SET) == 0 && fread(&v, 4, 1, f) == 1) {
        fclose(f);
        return v;
    }
    if (f) fclose(f);
    hle_fatal("cannot read the PE header of %s", path);
    return 0;
}

static uint32_t g_call_va, g_call_n, g_call_args[8];
static uint32_t call_one(void) {
    uint32_t eax = hle_call_guest(g_call_va, (int)g_call_n, g_call_args);
    printf("call 0x%08X -> eax 0x%08X\n", g_call_va, eax);
    return 0;
}

static void report_eh_depth(int code) { (void)code; fprintf(stderr, "eh depth %d\n", bw2_eh_depth()); }

static int usage(void) {
    fprintf(stderr, "usage: bw2 --game DIR [--exe white.exe] [--strict] [--trace-eh] [--lifted-dll NAME=0xBASE]...\n");
    return BW2_EXIT_BOOT;
}

/* --selftest-window: from the guest's thread, a windowed D3D9 device on a real NSWindow: 60 frames of
 * clears and Presents, then the device and the window go. Opens a window (plan 4 Task 10). */
static uint32_t vm(uint32_t obj, int slot, int n, const uint32_t *args) {
    uint32_t a[12] = { obj };
    memcpy(a + 1, args, 4 * (size_t)n);
    return hle_call_guest(MEM32(MEM32(obj) + 4u * (uint32_t)slot), n + 1, a);
}
static uint32_t window_selftest(void) {
    mach_enter();
    uint32_t v = 32, d3d = hle_call_guest(hle_resolve("Direct3DCreate9"), 1, &v);
    uint32_t pp = hle_va(hle_alloc(56)), out = hle_va(hle_alloc(4)), frames = 0;
    MEM32(pp) = 640, MEM32(pp + 4) = 480, MEM32(pp + 8) = 22, MEM32(pp + 12) = 1, MEM32(pp + 24) = 1;
    MEM32(pp + 28) = 0x10001u, MEM32(pp + 32) = 1, MEM32(pp + 36) = 1, MEM32(pp + 40) = 75;
    uint32_t cd[6] = { 0, 1, 0x10001u, 0x40u, pp, out };
    uint32_t hr = d3d ? vm(d3d, 16, 6, cd) : 1;
    if (!hr) {
        uint32_t dev = MEM32(out);
        for (uint32_t i = 0; i < 60; i++) {
            uint32_t one = 0x3F800000u, c[6] = { 0, 0, 3, 0xFF000000u | (i * 4) << 16 | 0x4080, one, 0 };
            uint32_t p[4] = { 0, 0, 0, 0 };
            if (!vm(dev, 43, 6, c) && !vm(dev, 17, 4, p)) frames++;
        }
        vm(dev, 2, 0, NULL);
    }
    if (d3d) vm(d3d, 2, 0, NULL);
    mach_leave();
    printf("window ok %u\n", frames);
    fflush(stdout);
    return frames == 60 ? 0 : 1;
}

/* --selftest-mainsync: a block sent from the guest's thread runs on the process main thread. */
static uint32_t mainsync_selftest(void) {
    __block int on_main = 0;
    bw2_main_sync(^{ on_main = pthread_main_np(); });
    printf("%s\n", on_main && !pthread_main_np() ? "main sync ok" : "main sync FAILED");
    fflush(stdout);
    return on_main ? 0 : 1;
}

int main(int argc, char **argv) {
    const char *game = NULL, *exe = "white.exe";
    int strict = 0, st_dispatch = 0, st_mem = 0, st_mainsync = 0, st_window = 0, nlifted = 0, ncall = -1;
    uint32_t call_args[8];
    struct { const char *name; uint32_t base; } lifted[8];
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--game") && i + 1 < argc) game = argv[++i];
        else if (!strcmp(argv[i], "--exe") && i + 1 < argc) exe = argv[++i];
        else if (!strcmp(argv[i], "--strict")) strict = 1;
        else if (!strcmp(argv[i], "--trace-eh")) bw2_eh_trace = 1;
        else if (!strcmp(argv[i], "--call") && i + 1 < argc) {   /* debugging: one guest function, fresh state */
            ncall = 0;
            g_call_va = (uint32_t)strtoul(argv[++i], NULL, 0);
            while (i + 1 < argc && argv[i + 1][0] != '-' && ncall < 8) call_args[ncall++] = (uint32_t)strtoul(argv[++i], NULL, 0);
        }
        else if (!strcmp(argv[i], "--lifted-dll") && i + 1 < argc) {
            char *eq = strchr(argv[++i], '=');
            if (!eq) return usage();
            *eq = 0;
            lifted[nlifted].name = argv[i], lifted[nlifted++].base = (uint32_t)strtoul(eq + 1, NULL, 0);
            if (nlifted == 8) return usage();
        }
        else if (!strcmp(argv[i], "--selftest-dispatch")) st_dispatch = 1;
        else if (!strcmp(argv[i], "--selftest-mem")) st_mem = 1;
        else if (!strcmp(argv[i], "--selftest-eh-depth")) hle_on_exit = report_eh_depth;
        else if (!strcmp(argv[i], "--selftest-mainsync")) st_mainsync = 1;
        else if (!strcmp(argv[i], "--selftest-window")) st_window = 1;
        else { fprintf(stderr, "bw2: unknown argument %s\n", argv[i]); return usage(); }
    }
    if (getenv("BW2_TRACE_IMPORTS")) win32hle_trace = 1;   /* one line per import call, with its caller */
    bw2_dispatch_init();
    if (st_dispatch) return bw2_dispatch_selftest();
    hle_mem_init();
    if (st_mem) {
        printf("mem base 0x%llx\n", (unsigned long long)(uintptr_t)g_mem_base);
        return 0;
    }
    if (st_mainsync) return (int)bw2_run_on_main_stack(mainsync_selftest);
    if (st_window) {
        bw2_register_shims();
        return (int)bw2_run_on_main_stack(window_selftest);
    }
    if (!game) return usage();
    char support[PATH_MAX];
    const char *env = getenv("BW2_SUPPORT"), *home = getenv("HOME");
    if (env && *env) snprintf(support, sizeof support, "%s", env);
    else snprintf(support, sizeof support, "%s/Library/Application Support/Black & White 2", home ? home : ".");
    char game_abs[PATH_MAX];
    if (!realpath(game, game_abs)) { fprintf(stderr, "bw2: no game folder %s\n", game); return BW2_EXIT_BOOT; }
    if (bw2_mount(game_abs, support)) return BW2_EXIT_BOOT;
    game = game_abs;
    char path[4096];
    snprintf(path, sizeof path, "%s/%s", game, exe);
    bw2_later_register(path);                   /* before the rest: these names must stop the run */
    bw2_register_shims();
    bw2_threads_init();
    bw2_dll_init();
    for (int i = 0; i < nlifted; i++) bw2_lifted_dll_add(lifted[i].name, lifted[i].base);
    bw2_dll_hold(pe_field(path, 0x1C), pe_field(path, 0x38));
    if (strict ? recomp_host_boot(path) : recomp_host_boot_permissive(path)) {
        fprintf(stderr, "bw2: cannot boot %s%s\n", path, strict ? " (strict: unanswered imports above)" : "");
        return BW2_EXIT_BOOT;
    }
    bw2_tls_add_module(pe_field(path, 0x1C));
    bw2_guard_init(pe_field(path, 0x1C));
    bw2_eh_add_image(pe_field(path, 0x1C), pe_field(path, 0x38));
    if (ncall >= 0) {
        g_call_n = (uint32_t)ncall;
        memcpy(g_call_args, call_args, sizeof call_args);
        return (int)bw2_run_on_main_stack(call_one);
    }
    return (int)bw2_run_on_main_stack(recomp_host_run);
}
