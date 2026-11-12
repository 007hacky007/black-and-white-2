/*
 * main.c - the BW2 engine's process entry: arguments, the shim modules, the
 * boot of the game image and its run.
 *
 *   bw2 --game DIR [--exe white.exe] [--strict] [--trace-eh] [--lifted-dll NAME=0xBASE]...
 *       [--check-d3dx DIR]
 *
 * --check-d3dx runs the lifted D3DX's checks named on stdin (d3dx/d3dx_check.c)
 * after the boot's setup, in place of the game's entry.
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
#include <unistd.h>
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

int bw2_d3dx_check(const char *dir);                    /* d3dx/d3dx_check.c */
static const char *g_check_dir;
static uint32_t check_d3dx(void) { return (uint32_t)bw2_d3dx_check(g_check_dir); }

static void report_eh_depth(int code) { (void)code; fprintf(stderr, "eh depth %d\n", bw2_eh_depth()); }

static int usage(void) {
    fprintf(stderr, "usage: bw2 --game DIR [--exe white.exe] [--strict] [--trace-eh] [--lifted-dll NAME=0xBASE]... "
                    "[--check-d3dx DIR]\n");
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

/* --selftest-input-window: a windowed 640x480 device on a game window (CS_DBLCLKS); synthetic NSEvents
 * posted to its Mac window (a click at the view's centre, the A key, a 3-line scroll, then the close
 * button) reach the window procedure as Windows messages, pumped as the game pumps. Prints what the
 * procedure was given. Opens a window (plan 6 Task 4). */
int bw2_mac_selftest_input(void);
void bw2_mac_selftest_close(void);
static int g_iw_close;
static void iw_record(uint32_t h, uint32_t m, uint32_t w, uint32_t l) {
    (void)h;
    int x = (int16_t)(l & 0xFFFF), y = (int16_t)(l >> 16);
    switch (m) {
    case 0x0200: printf("WM_MOUSEMOVE %d,%d\n", x, y); break;
    case 0x0201: printf("WM_LBUTTONDOWN %d,%d\n", x, y); break;
    case 0x0202: printf("WM_LBUTTONUP %d,%d\n", x, y); break;
    case 0x0203: printf("WM_LBUTTONDBLCLK %d,%d\n", x, y); break;
    case 0x0100: printf("WM_KEYDOWN %X %08X\n", w, l); break;
    case 0x0101: printf("WM_KEYUP %X %08X\n", w, l); break;
    case 0x0102: printf("WM_CHAR %X\n", w); break;
    case 0x020A: printf("WM_MOUSEWHEEL %d\n", (int16_t)(w >> 16)); break;
    case 0x0112: printf("WM_SYSCOMMAND %X\n", w); break;
    case 0x0010: printf("WM_CLOSE\n"); g_iw_close = 1; break;
    case 0x0006: printf("WM_ACTIVATE %X\n", w); break;
    default: return;
    }
    fflush(stdout);
}
static uint32_t gcall(const char *name, int n, const uint32_t *a) { return hle_call_guest(hle_resolve(name), n, a); }
/* Peek, translate, dispatch until the queue is empty, for at least ms milliseconds or until *until. */
static void iw_pump(uint32_t msg, int ms, const int *until) {
    for (int t = 0; t < ms; t += 10) {
        uint32_t peek[5] = { msg, 0, 0, 0, 1 };
        while (gcall("PeekMessageA", 5, peek)) {
            gcall("TranslateMessage", 1, &msg);
            gcall("DispatchMessageA", 1, &msg);
        }
        if (until && *until) return;
        mach_leave();
        usleep(10000);
        mach_enter();
    }
}
static uint32_t input_window_selftest(void) {
    mach_enter();
    hle_proc_hook = iw_record;
    uint32_t wc = hle_va(hle_alloc(40)), nm = hle_va(hle_alloc(16)), msg = hle_va(hle_alloc(28));
    memset(hle_ptr(wc), 0, 40);
    strcpy(hle_ptr(nm), "bw2iw");
    MEM32(wc) = 8u;                                       /* CS_DBLCLKS */
    MEM32(wc + 4) = hle_resolve("DefWindowProcA"), MEM32(wc + 16) = 0x00400000u, MEM32(wc + 36) = nm;
    uint32_t ok = gcall("RegisterClassA", 1, &wc) != 0;
    uint32_t cw[12] = { 0, nm, nm, 0x90000000u, 0, 0, 640, 480, 0, 0, 0x00400000u, 0 };   /* WS_POPUP|WS_VISIBLE */
    uint32_t hwnd = ok ? gcall("CreateWindowExA", 12, cw) : 0;
    uint32_t v = 32, d3d = hle_call_guest(hle_resolve("Direct3DCreate9"), 1, &v);
    uint32_t pp = hle_va(hle_alloc(56)), out = hle_va(hle_alloc(4)), dev = 0;
    memset(hle_ptr(pp), 0, 56);
    MEM32(pp) = 640, MEM32(pp + 4) = 480, MEM32(pp + 8) = 22, MEM32(pp + 12) = 1, MEM32(pp + 24) = 1;
    MEM32(pp + 28) = hwnd, MEM32(pp + 32) = 1;
    uint32_t cd[6] = { 0, 1, hwnd, 0x40u, pp, out };
    if (hwnd && d3d && !vm(d3d, 16, 6, cd)) dev = MEM32(out);
    if (dev) {
        iw_pump(msg, 500, NULL);                          /* the window opens and takes the focus */
        printf("posting\n");
        fflush(stdout);
        if (!bw2_mac_selftest_input()) dev = 0;
        iw_pump(msg, 500, NULL);
        bw2_mac_selftest_close();
        iw_pump(msg, 5000, &g_iw_close);
    }
    if (dev) vm(dev, 2, 0, NULL);
    if (d3d) vm(d3d, 2, 0, NULL);
    hle_proc_hook = NULL;
    mach_leave();
    int pass = dev && g_iw_close;
    printf("%s\n", pass ? "input window ok" : "input window FAILED");
    fflush(stdout);
    return pass ? 0 : 1;
}

/* --selftest-cursor-window: the cursor on a real window (plan 6 Task 5). The pointer is parked outside
 * the window before it opens; a windowed 640x480 device; the game's DirectInput setup and mode switch
 * (coop 6, Acquire, Unacquire, coop 5, Acquire), with no cursor call after them; then the app hidden
 * and brought back, with the game's read-and-reacquire loop running. Prints the cursor as applied
 * after each step. Opens a window and moves the pointer (put back at the end). */
void bw2_mac_selftest_park_pointer(void);
void bw2_mac_selftest_restore_pointer(void);
void bw2_mac_selftest_app_active(int active);
int bw2_mac_selftest_cursor(int *key, int *hidden, int *detached, int *inside, int *cx, int *cy);
static int cw_state(const char *step) {
    int key = 0, hidden = 0, detached = 0, inside = 0, cx = -1, cy = -1;
    if (!bw2_mac_selftest_cursor(&key, &hidden, &detached, &inside, &cx, &cy)) return 0;
    printf("cursor %s: key %d hidden %d detached %d inside %d at %d,%d\n", step, key, hidden, detached, inside, cx, cy);
    fflush(stdout);
    return 1;
}
/* The game's frame: pump, one buffered read, and Acquire again after a loss. */
static void cw_frames(uint32_t msg, uint32_t dev, uint32_t buf, uint32_t pn, int ms) {
    for (int t = 0; t < ms; t += 10) {
        uint32_t peek[5] = { msg, 0, 0, 0, 1 };
        while (gcall("PeekMessageA", 5, peek)) {
            gcall("TranslateMessage", 1, &msg);
            gcall("DispatchMessageA", 1, &msg);
        }
        if (dev) {                                        /* slot 10 GetDeviceData, 7 Acquire */
            MEM32(pn) = 64;
            uint32_t rd[4] = { 20, buf, pn, 0 };
            uint32_t hr = vm(dev, 10, 4, rd);
            if (hr == 0x8007001Eu || hr == 0x8007000Cu) vm(dev, 7, 0, NULL);   /* INPUTLOST, NOTACQUIRED */
        }
        mach_leave();
        usleep(10000);
        mach_enter();
    }
}
static uint32_t cursor_window_selftest(void) {
    bw2_mac_selftest_park_pointer();
    mach_enter();
    uint32_t wc = hle_va(hle_alloc(40)), nm = hle_va(hle_alloc(16)), msg = hle_va(hle_alloc(28));
    memset(hle_ptr(wc), 0, 40);
    strcpy(hle_ptr(nm), "bw2cw");
    MEM32(wc + 4) = hle_resolve("DefWindowProcA"), MEM32(wc + 16) = 0x00400000u, MEM32(wc + 36) = nm;
    uint32_t ok = gcall("RegisterClassA", 1, &wc) != 0;
    uint32_t cw[12] = { 0, nm, nm, 0x90000000u, 0, 0, 640, 480, 0, 0, 0x00400000u, 0 };   /* WS_POPUP|WS_VISIBLE */
    uint32_t hwnd = ok ? gcall("CreateWindowExA", 12, cw) : 0;
    uint32_t v = 32, d3d = hle_call_guest(hle_resolve("Direct3DCreate9"), 1, &v);
    uint32_t pp = hle_va(hle_alloc(56)), out = hle_va(hle_alloc(4)), dev = 0;
    memset(hle_ptr(pp), 0, 56);
    MEM32(pp) = 640, MEM32(pp + 4) = 480, MEM32(pp + 8) = 22, MEM32(pp + 12) = 1, MEM32(pp + 24) = 1;
    MEM32(pp + 28) = hwnd, MEM32(pp + 32) = 1;
    uint32_t cd[6] = { 0, 1, hwnd, 0x40u, pp, out };
    if (hwnd && d3d && !vm(d3d, 16, 6, cd)) dev = MEM32(out);
    int pass = 0;
    if (dev) {
        static const uint8_t iid[16] = { 0x30, 0x80, 0x79, 0xBF, 0x3A, 0x48, 0xA2, 0x4D,
                                         0xAA, 0x99, 0x5D, 0x64, 0xED, 0x36, 0x97, 0x00 };   /* IID_IDirectInput8A */
        static const uint8_t mouse[16] = { 0x60, 0x2B, 0x1D, 0x6F, 0xA0, 0xD5, 0xCF, 0x11,
                                           0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00 };   /* GUID_SysMouse */
        uint32_t g = hle_va(hle_alloc(32)), di_out = hle_va(hle_alloc(4)), m_out = hle_va(hle_alloc(4));
        memcpy(hle_ptr(g), iid, 16), memcpy(hle_ptr(g + 16), mouse, 16);
        /* three axes and four buttons, any instance, as the game's format names them */
        uint32_t objs = hle_va(hle_alloc(7 * 16)), f = hle_va(hle_alloc(24)), prop = hle_va(hle_alloc(20));
        for (uint32_t i = 0; i < 7; i++)
            MEM32(objs + 16 * i) = 0, MEM32(objs + 16 * i + 4) = i < 3 ? 4 * i : 12 + (i - 3),
            MEM32(objs + 16 * i + 8) = i < 3 ? 0x00FFFF03u : 0x00FFFF0Cu, MEM32(objs + 16 * i + 12) = 0;
        MEM32(f) = 24, MEM32(f + 4) = 16, MEM32(f + 8) = 2, MEM32(f + 12) = 16, MEM32(f + 16) = 7, MEM32(f + 20) = objs;
        MEM32(prop) = 20, MEM32(prop + 4) = 16, MEM32(prop + 8) = 0, MEM32(prop + 12) = 0, MEM32(prop + 16) = 64;
        uint32_t a[5] = { 0x00400000u, 0x800u, g, di_out, 0 }, di = 0, mdev = 0;
        if (!gcall("DirectInput8Create", 5, a)) di = MEM32(di_out);
        uint32_t c[3] = { g + 16, m_out, 0 };
        if (di && !vm(di, 3, 3, c)) mdev = MEM32(m_out);
        uint32_t buf = hle_va(hle_alloc(20 * 64)), pn = hle_va(hle_alloc(4));
        if (mdev) {
            uint32_t sf[1] = { f }, c6[2] = { hwnd, 6 }, c5[2] = { hwnd, 5 }, sp[2] = { 1, prop };
            cw_frames(msg, 0, 0, 0, 300);                /* the window opens */
            bw2_mac_selftest_app_active(1);              /* started from a terminal: the app comes to the front */
            cw_frames(msg, 0, 0, 0, 500);
            cw_state("open");
            /* slots: 6 SetProperty, 7 Acquire, 8 Unacquire, 11 SetDataFormat, 13 SetCooperativeLevel */
            uint32_t hr = vm(mdev, 11, 1, sf) | vm(mdev, 13, 2, c6) | vm(mdev, 6, 2, sp) | vm(mdev, 7, 0, NULL) |
                          vm(mdev, 8, 0, NULL) | vm(mdev, 13, 2, c5) | vm(mdev, 7, 0, NULL);
            printf("dinput setup 0x%X\n", hr);
            cw_frames(msg, mdev, buf, pn, 300);
            pass = hr == 0 && cw_state("exclusive");
            bw2_mac_selftest_app_active(0);
            cw_frames(msg, mdev, buf, pn, 700);
            pass = pass && cw_state("hidden");
            bw2_mac_selftest_app_active(1);
            cw_frames(msg, mdev, buf, pn, 1000);
            pass = pass && cw_state("back");
            vm(mdev, 2, 0, NULL);
        }
        if (di) vm(di, 2, 0, NULL);
    }
    if (dev) vm(dev, 2, 0, NULL);
    if (d3d) vm(d3d, 2, 0, NULL);
    mach_leave();
    bw2_mac_selftest_restore_pointer();
    printf("%s\n", pass ? "cursor window ok" : "cursor window FAILED");
    fflush(stdout);
    return pass ? 0 : 1;
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
    int strict = 0, st_dispatch = 0, st_mem = 0, st_mainsync = 0, st_window = 0, st_input_window = 0, st_cursor_window = 0;
    int nlifted = 0, ncall = -1;
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
        else if (!strcmp(argv[i], "--check-d3dx") && i + 1 < argc) g_check_dir = argv[++i];
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
        else if (!strcmp(argv[i], "--selftest-input-window")) st_input_window = 1;
        else if (!strcmp(argv[i], "--selftest-cursor-window")) st_cursor_window = 1;
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
    if (st_window || st_input_window || st_cursor_window) {
        bw2_register_shims();
        return (int)bw2_run_on_main_stack(st_window ? window_selftest
                                          : st_input_window ? input_window_selftest : cursor_window_selftest);
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
    if (g_check_dir) return (int)bw2_run_on_main_stack(check_d3dx);
    if (ncall >= 0) {
        g_call_n = (uint32_t)ncall;
        memcpy(g_call_args, call_args, sizeof call_args);
        return (int)bw2_run_on_main_stack(call_one);
    }
    return (int)bw2_run_on_main_stack(recomp_host_run);
}
