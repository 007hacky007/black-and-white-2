/*
 * bw2.h - the BW2 engine's own runtime layer on top of pcrecomp's win32hle.
 *
 * Exit codes (plan 3): 0 the guest exited normally (or its own code), 2 boot
 * failure, 3 tripwire (the user decides), 4 reached a layer a later plan owns.
 * Every other loud failure ends in abort() after naming what failed.
 */
#ifndef BW2_H
#define BW2_H
#include <stdint.h>
#include "win32hle.h"

#define BW2_EXIT_BOOT     2
#define BW2_EXIT_TRIPWIRE 3
#define BW2_EXIT_LATER    4

/* dispatch.c: an O(1) recomp_lookup over the generated dispatch table. */
void bw2_dispatch_init(void);
int  bw2_dispatch_selftest(void);       /* prints "dispatch ok <n>"; 0 on success */

/* main.c: register the shim modules, BW2's own first so they take precedence. */
void bw2_register_shims(void);
/* win32hle's own answer to an import, for a BW2 shim that adds to it (main.c). */
recomp_func_t bw2_hle_shim(const char *name);

/* mount.c: the guest's disk (plan 3 Global Constraints). */
#define BW2_INSTALL_DIR "C:\\Program Files\\EA GAMES\\Black & White 2"
int  bw2_mount(const char *game_dir, const char *support_dir);
int  bw2_in_game_tree(const char *host_path);
extern const win32hle_shim bw2_mount_shims[];
/* threads.c: big host stacks, per-thread static TLS, crash reports on an alt stack. */
void     bw2_threads_init(void);
int      bw2_tls_add_module(uint32_t image_base);
uint32_t bw2_run_on_main_stack(uint32_t (*fn)(void));
/* dll.c: DLLs lifted with the game, loaded at their base with DllMain run. */
void bw2_dll_init(void);
void bw2_lifted_dll_add(const char *base_name, uint32_t image_base);
void bw2_dll_hold(uint32_t main_base, uint32_t main_span);
/* eh.c: guest C++ exceptions. */
int bw2_eh_depth(void);                 /* this thread's live landing pads */
void bw2_eh_add_image(uint32_t base, uint32_t span);   /* where EH tables may be */
extern int bw2_eh_trace;                /* --trace-eh */
extern const win32hle_shim bw2_eh_shims[];
/* guard.c: the tripwire for disc and driver checks (exit 3, the user decides). */
void bw2_guard_init(uint32_t main_image_base);
extern const win32hle_shim bw2_guard_shims[];
/* later.c: the end of plan 3's boot: a later plan's import or DLL exits 4. */
void bw2_later_register(const char *exe_path);
int  bw2_later_dll(const char *base_name);
void bw2_later_load(const char *base_name);
/* d3d9/mac_main.m (plan 4): the process main thread runs the run loop while the
 * guest runs on its own thread. bw2_main_sync runs a block there and waits. */
#include <pthread.h>
int      bw2_headless(void);                  /* BW2_HEADLESS=1: no NSApplication, no window */
void     bw2_main_sync(void (^block)(void));
uint32_t bw2_mac_main_loop(pthread_t guest);  /* until the guest thread ends; its result */
void     bw2_mac_guest_done(void);
/* shell.c: the user's folders and name. */
extern const win32hle_shim bw2_shell_shims[];

#endif
