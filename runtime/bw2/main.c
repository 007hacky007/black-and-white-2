/*
 * main.c - the BW2 engine's process entry: arguments, the shim modules, the
 * boot of the game image and its run.
 *
 *   bw2 --game DIR [--exe white.exe] [--strict]
 *
 * The bind is permissive: an import nothing answers is bound to a stub that
 * stops the run, naming itself, when it is first called (most of the game's
 * unanswered imports never run). --strict lists them all and exits 2 instead.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "win32hle.h"
#include "host.h"
#include "bw2.h"

void bw2_register_shims(void) {
    win32hle_register(win32hle_kernel32);
    win32hle_register(win32hle_kernel32_ext);
    win32hle_register(win32hle_kernel32_crt);
    win32hle_register(win32hle_module);
    win32hle_register(win32hle_gdi32);
    win32hle_register(win32hle_user32);
    win32hle_register(win32hle_winmm);
    win32hle_register(win32hle_lz32);
    win32hle_register(win32hle_wsock32);
    win32hle_register(win32hle_ole32);
    win32hle_register(win32hle_storage);
    win32hle_register(win32hle_advapi32);
}

static int usage(void) {
    fprintf(stderr, "usage: bw2 --game DIR [--exe white.exe] [--strict]\n");
    return BW2_EXIT_BOOT;
}

int main(int argc, char **argv) {
    const char *game = NULL, *exe = "white.exe";
    int strict = 0, st_dispatch = 0, st_mem = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--game") && i + 1 < argc) game = argv[++i];
        else if (!strcmp(argv[i], "--exe") && i + 1 < argc) exe = argv[++i];
        else if (!strcmp(argv[i], "--strict")) strict = 1;
        else if (!strcmp(argv[i], "--selftest-dispatch")) st_dispatch = 1;
        else if (!strcmp(argv[i], "--selftest-mem")) st_mem = 1;
        else { fprintf(stderr, "bw2: unknown argument %s\n", argv[i]); return usage(); }
    }
    bw2_dispatch_init();
    if (st_dispatch) return bw2_dispatch_selftest();
    hle_mem_init();
    if (st_mem) {
        printf("mem base 0x%llx\n", (unsigned long long)(uintptr_t)g_mem_base);
        return 0;
    }
    if (!game) return usage();
    bw2_register_shims();
    char path[4096];
    snprintf(path, sizeof path, "%s/%s", game, exe);
    if (strict ? recomp_host_boot(path) : recomp_host_boot_permissive(path)) {
        fprintf(stderr, "bw2: cannot boot %s%s\n", path, strict ? " (strict: unanswered imports above)" : "");
        return BW2_EXIT_BOOT;
    }
    return (int)recomp_host_run();
}
