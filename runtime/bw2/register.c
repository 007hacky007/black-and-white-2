/*
 * register.c - the shim modules the engine answers imports with, in order:
 * BW2's own first (hle_resolve takes the first match), then win32hle's.
 */
#include <string.h>
#include "win32hle.h"
#include "bw2.h"

/* BW2's import answers by DLL (shims_*.c); declared here so adding one
 * changes no header (every lifted shard depends on the headers). */
extern const win32hle_shim bw2_k32_shims[], bw2_user_shims[], bw2_adv_shims[], bw2_net_shims[];
extern const win32hle_shim bw2_later_shims[];   /* later.c: LoadLibraryA of a later plan's DLL */
void bw2_d3d_register(void);                    /* d3d9/d3d_register.c (plan 4) */
void bw2_input_register(void);                  /* input/input.c (plan 6) */
void bw2_in_drain_hook(void);                   /* input/input.c: the host input queue's full drain */
void bw2_audio_register(void);                  /* audio/dsound.c (plan 8) */

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
    win32hle_register(bw2_guard_shims);         /* BW2's own first: hle_resolve takes the first */
    win32hle_register(bw2_later_shims);
    win32hle_register(bw2_mount_shims);
    win32hle_register(bw2_shell_shims);
    win32hle_register(bw2_eh_shims);
    win32hle_register(bw2_k32_shims);
    win32hle_register(bw2_user_shims);
    win32hle_register(bw2_adv_shims);
    win32hle_register(bw2_net_shims);
    bw2_d3d_register();
    bw2_input_register();
    bw2_audio_register();
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
    /* Host input reaches the guest when its message queue is empty, inside
     * PeekMessage/GetMessage, where Windows delivers sent messages. */
    hle_set_pump_hook(bw2_in_drain_hook);
}

