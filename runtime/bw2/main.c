/*
 * main.c - the BW2 engine's process entry: arguments, the shim modules, the
 * boot of the game image and its run.
 *
 *   bw2 --game DIR [--exe white.exe] [--strict]
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

static const win32hle_shim *const g_hle_tables[] = {
    win32hle_kernel32, win32hle_kernel32_ext, win32hle_kernel32_crt, win32hle_module, win32hle_gdi32,
    win32hle_user32, win32hle_winmm, win32hle_lz32, win32hle_wsock32, win32hle_ole32, win32hle_storage,
    win32hle_advapi32,
};

recomp_func_t bw2_hle_shim(const char *name) {
    for (size_t t = 0; t < sizeof g_hle_tables / sizeof *g_hle_tables; t++)
        for (const win32hle_shim *s = g_hle_tables[t]; s->name; s++)
            if (!strcmp(s->name, name)) return s->fn;
    hle_fatal("bw2_hle_shim: win32hle has no %s", name);
    return NULL;
}

void bw2_register_shims(void) {
    win32hle_register(bw2_mount_shims);         /* BW2's own first: hle_resolve takes the first */
    win32hle_register(bw2_shell_shims);
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

/* The ImageBase in path's PE header: where recomp_pe_map put it. */
static uint32_t image_base(const char *path) {
    FILE *f = fopen(path, "rb");
    uint32_t lfanew = 0, base = 0;
    if (f && fseek(f, 0x3C, SEEK_SET) == 0 && fread(&lfanew, 4, 1, f) == 1 &&
        fseek(f, (long)lfanew + 0x34, SEEK_SET) == 0 && fread(&base, 4, 1, f) == 1) {
        fclose(f);
        return base;
    }
    if (f) fclose(f);
    hle_fatal("cannot read the image base of %s", path);
    return 0;
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
    char support[PATH_MAX];
    const char *env = getenv("BW2_SUPPORT"), *home = getenv("HOME");
    if (env && *env) snprintf(support, sizeof support, "%s", env);
    else snprintf(support, sizeof support, "%s/Library/Application Support/Black & White 2", home ? home : ".");
    char game_abs[PATH_MAX];
    if (!realpath(game, game_abs)) { fprintf(stderr, "bw2: no game folder %s\n", game); return BW2_EXIT_BOOT; }
    if (bw2_mount(game_abs, support)) return BW2_EXIT_BOOT;
    game = game_abs;
    bw2_register_shims();
    char path[4096];
    snprintf(path, sizeof path, "%s/%s", game, exe);
    bw2_threads_init();
    if (strict ? recomp_host_boot(path) : recomp_host_boot_permissive(path)) {
        fprintf(stderr, "bw2: cannot boot %s%s\n", path, strict ? " (strict: unanswered imports above)" : "");
        return BW2_EXIT_BOOT;
    }
    bw2_tls_add_module(image_base(path));
    return (int)bw2_run_on_main_stack(recomp_host_run);
}
