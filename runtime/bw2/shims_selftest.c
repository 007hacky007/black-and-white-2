/*
 * shims_selftest.c - BW2's own import answers, called as lifted code calls
 * them: arguments pushed right to left, a return address, the shim. Each
 * check also verifies the shim popped exactly its stdcall arguments. Built
 * with the engine (minus main.c) by tests/test_engine.py test_shims_selftest.
 * Every shim the boot needs gets its check here first (plan 3 Task 10).
 */
#define RECOMP_HOST_CPU      /* recomp_cpuid is cpu.c's */
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include "win32hle.h"
#include "bw2.h"

const recomp_dispatch_entry_t recomp_dispatch_table[1] = { { 0, 0 } };
const uint32_t recomp_dispatch_count = 0;

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

static uint32_t call(const char *name, int n, ...) {
    uint32_t va = hle_resolve(name);
    if (!va) { printf("FAIL %s: not answered\n", name); fails++; return 0xDEADBEEFu; }
    recomp_func_t f = recomp_lookup_import(va);
    uint32_t a[16];
    va_list ap;
    va_start(ap, n);
    for (int i = 0; i < n; i++) a[i] = va_arg(ap, uint32_t);
    va_end(ap);
    uint32_t esp0 = g_esp;
    for (int i = n - 1; i >= 0; i--) { g_esp -= 4; MEM32(g_esp) = a[i]; }
    g_esp -= 4;
    MEM32(g_esp) = 0x12345678u;
    f();
    if (g_esp != esp0) {
        printf("FAIL %s: popped %d bytes, want %d\n", name, (int)(g_esp - (esp0 - 4u - 4u * (uint32_t)n)), 4 + 4 * n);
        fails++;
        g_esp = esp0;
    }
    return g_eax;
}

static uint32_t gmem(uint32_t n) { return hle_va(hle_alloc(n)); }

/* A cdecl call (msvcrt): the shim pops only its return address, the caller its arguments. */
static uint32_t ccall(const char *name, int n, ...) {
    uint32_t va = hle_resolve(name);
    if (!va) { printf("FAIL %s: not answered\n", name); fails++; return 0xDEADBEEFu; }
    recomp_func_t f = recomp_lookup_import(va);
    uint32_t a[16];
    va_list ap;
    va_start(ap, n);
    for (int i = 0; i < n; i++) a[i] = va_arg(ap, uint32_t);
    va_end(ap);
    uint32_t esp0 = g_esp;
    for (int i = n - 1; i >= 0; i--) { g_esp -= 4; MEM32(g_esp) = a[i]; }
    g_esp -= 4;
    MEM32(g_esp) = 0x12345678u;
    f();
    if (g_esp != esp0 - 4u * (uint32_t)n) {
        printf("FAIL %s: cdecl, popped %d bytes, want 4\n", name, (int)(g_esp - (esp0 - 4u - 4u * (uint32_t)n)));
        fails++;
    }
    g_esp = esp0;
    return g_eax;
}
/* A double as the two argument dwords a cdecl call pushes for it. */
#define DLO(d) dbits(d, 0)
#define DHI(d) dbits(d, 1)
static uint32_t dbits(double d, int hi) { uint64_t u; memcpy(&u, &d, 8); return hi ? (uint32_t)(u >> 32) : (uint32_t)u; }
static double mem_double(uint32_t va) { uint64_t u = (uint64_t)MEM32(va) | (uint64_t)MEM32(va + 4) << 32; double d; memcpy(&d, &u, 8); return d; }
static uint32_t gstr(const char *s) { uint32_t va = gmem((uint32_t)strlen(s) + 1); strcpy(hle_ptr(va), s); return va; }

uint32_t bw2_bind_resolve(const char *name);                 /* dll.c */

/* Guest-callable host functions for _initterm and qsort (cdecl). */
static int g_init_order[8], g_ninit;
static void t_init1(void) { if (g_ninit < 8) g_init_order[g_ninit] = 1; g_ninit++; g_esp += 4; }
static void t_init2(void) { if (g_ninit < 8) g_init_order[g_ninit] = 2; g_ninit++; g_esp += 4; }
static void t_cmp(void) {
    int32_t a = (int32_t)MEM32(A32(0)), b = (int32_t)MEM32(A32(1));
    g_eax = (uint32_t)(a < b ? -1 : a > b);
    g_esp += 4;
}
static const win32hle_shim t_init1_tab[] = { { "selftest_crt_init1", t_init1 }, { NULL, NULL } };
static const win32hle_shim t_init2_tab[] = { { "selftest_crt_init2", t_init2 }, { NULL, NULL } };
static const win32hle_shim t_cmp_tab[] = { { "selftest_crt_cmp", t_cmp }, { NULL, NULL } };
static void c_purecall(void) { ccall("_purecall", 0); }

/* A completion routine for ReadFileEx, as a guest function: (error, bytes, overlapped), stdcall. */
static uint32_t g_done[8][3];
static int g_ndone;
static void t_completion(void) {
    if (g_ndone < 8) g_done[g_ndone][0] = A32(0), g_done[g_ndone][1] = A32(1), g_done[g_ndone][2] = A32(2);
    g_ndone++;
    RET(0, 3);
}
/* A guest thread's start routine (stdcall, one argument): COM on a second thread, while the main one is initialized. */
static uint32_t g_thread_coinit = 0xDEADBEEFu;
static void t_coinit_thread(void) {
    g_thread_coinit = call("CoInitializeEx", 2, 0u, 0u);
    call("CoUninitialize", 0);
    RET(0, 1);
}
static const win32hle_shim t_shims[] = {
    { "selftest_completion", t_completion }, { "selftest_coinit_thread", t_coinit_thread }, { NULL, NULL } };

/* A lifted-DLL hook that records what LoadLibraryA gave it. */
static char g_hook_base[64], g_hook_host[1024];
static int g_hook_calls, g_hook_r, g_hook_had_host;
static int t_hook(const char *base, const char *host, uint32_t *hmod) {
    (void)hmod;
    g_hook_calls++;
    snprintf(g_hook_base, sizeof g_hook_base, "%s", base);
    g_hook_had_host = host != NULL;
    snprintf(g_hook_host, sizeof g_hook_host, "%s", host ? host : "");
    return g_hook_r;
}
static int ends_with(const char *s, const char *t) {
    size_t a = strlen(s), b = strlen(t);
    return a >= b && !strcmp(s + a - b, t);
}
static int count_of(const char *s, const char *t) {
    int n = 0;
    for (const char *p = s; (p = strstr(p, t)); p += strlen(t)) n++;
    return n;
}
static void write_file(const char *path, const void *data, size_t n) {
    FILE *f = fopen(path, "wb");
    if (f) fwrite(data, 1, n, f), fclose(f);
}

/* GetKeyNameTextA, and GetKeyNameTextW (UTF-16) when w: lParam's name in a buffer of n characters
   is want, and the count copied is its length. The buffer starts filled with 0x55. */
static void key_name(int w, uint32_t lp, uint32_t n, const char *want) {
    uint32_t buf = gmem(160);
    memset(hle_ptr(buf), 0x55, 160);
    uint32_t got = call(w ? "GetKeyNameTextW" : "GetKeyNameTextA", 3, lp, buf, n);
    int ok = got == strlen(want);
    for (size_t i = 0; ok && i <= strlen(want); i++)
        ok = (w ? MEM16(buf + 2u * (uint32_t)i) : MEM8(buf + (uint32_t)i)) == (uint8_t)want[i];
    if (!ok) { printf("FAIL GetKeyNameText%c(0x%08X, %u): %u, want \"%s\"\n", w ? 'W' : 'A', lp, n, got, want); fails++; }
}

/* Run fn in a child (fork) with its stderr captured in out: the child's exit status (fn returning is 0). */
static int in_child(void (*fn)(void), char *out, size_t n) {
    fflush(stdout), fflush(stderr);
    int p[2];
    if (pipe(p) != 0) return -1;
    pid_t pid = fork();
    if (pid == 0) {
        close(p[0]);
        dup2(p[1], 2);
        close(p[1]);
        fn();
        fflush(stderr);
        _exit(0);
    }
    close(p[1]);
    size_t k = 0;
    char sink[4096];
    for (ssize_t r; (r = read(p[0], k + 1 < n ? out + k : sink, k + 1 < n ? n - 1 - k : sizeof sink)) > 0; )
        if (k + 1 < n) k += (size_t)r;
    out[k] = 0;
    close(p[0]);
    int st = 0;
    waitpid(pid, &st, 0);
    return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
}
static uint32_t g_child_name;
static void c_load(void) { call("LoadLibraryA", 1, g_child_name); }
static void c_load_image(void) { call("LoadImageA", 6, 0u, g_child_name, 0u, 0u, 0u, 0x2010u); }
int main(void) {
    hle_mem_init();
    bw2_register_shims();
    mach_enter();

    /* kernel32 */
    {
        uint32_t cs = gmem(64);
        CHECK(call("InitializeCriticalSectionAndSpinCount", 2, cs, 4000u) == 1);
        call("EnterCriticalSection", 1, cs);
        call("LeaveCriticalSection", 1, cs);
        call("DeleteCriticalSection", 1, cs);
    }

    {
        uint32_t ps = gmem(16);
        CHECK(call("GetSystemPowerStatus", 1, ps) == 1 && MEM8(ps) == 1 && MEM8(ps + 1) == 128);   /* on AC, no battery */
    }

    {
        uint32_t v = gmem(4);
        MEM32(v) = 2;                                               /* HEAP_LFH */
        CHECK(call("HeapSetInformation", 4, call("GetProcessHeap", 0), 0u, v, 4u) == 1);
    }

    {
        uint32_t m = gmem(64);
        MEM32(m) = 64;
        CHECK(call("GlobalMemoryStatusEx", 1, m) == 1 && MEM32(m + 8) == 0x80000000u && MEM32(m + 12) == 0);  /* 2 GB */
        CHECK(MEM32(m + 16) == 0x60000000u);                                            /* 1.5 GB free */
        uint32_t o = gmem(32);
        call("GlobalMemoryStatus", 1, o);
        CHECK(MEM32(o + 8) == 0x80000000u && MEM32(o + 12) == 0x60000000u);             /* the same answer */
    }

    {
        uint32_t lo = gmem(4), hi = gmem(4);
        CHECK(call("GetProcessWorkingSetSize", 3, 0xFFFFFFFFu, lo, hi) == 1 && MEM32(lo) == 204800u && MEM32(hi) == 1413120u);
        CHECK(call("SetProcessWorkingSetSize", 3, 0xFFFFFFFFu, 64u << 20, 256u << 20) == 1);
    }

    /* user32 */
    {
        uint32_t v = gmem(64);
        CHECK(call("SystemParametersInfoA", 4, 0x70u, 0u, v, 0u) == 1 && MEM32(v) == 10);   /* SPI_GETMOUSESPEED */
        CHECK(call("SystemParametersInfoA", 4, 0x71u, 0u, 15u, 0u) == 1);                  /* SPI_SETMOUSESPEED */
        CHECK(call("SystemParametersInfoA", 4, 0x03u, 0u, v, 0u) == 1 && MEM32(v + 8) == 1); /* SPI_GETMOUSE */
        MEM32(v) = 8, MEM32(v + 4) = 0xFFFFFFFFu;
        CHECK(call("SystemParametersInfoA", 4, 0x3Au, 8u, v, 0u) == 1 && MEM32(v + 4) == 0); /* SPI_GETSTICKYKEYS */
        CHECK(call("SystemParametersInfoA", 4, 0x3Bu, 8u, v, 0u) == 1);
    }

    {
        uint32_t tme = gmem(16);
        MEM32(tme) = 16, MEM32(tme + 4) = 2;                       /* TME_LEAVE */
        CHECK(call("_TrackMouseEvent", 1, tme) == 1);
        CHECK(call("GetDoubleClickTime", 0) == 500);
        CHECK(call("GetKeyboardLayout", 1, 0u) == 0x04090409u);
        uint32_t l = gmem(16);
        CHECK(call("GetKeyboardLayoutList", 2, 0u, 0u) == 1);
        CHECK(call("GetKeyboardLayoutList", 2, 4u, l) == 1 && MEM32(l) == 0x04090409u);
    }

    /* advapi32: privileges, as for a standard user who does not hold SeLockMemoryPrivilege */
    {
        uint32_t name = gmem(32), luid = gmem(8), tok = gmem(4), tp = gmem(16);
        strcpy(hle_ptr(name), "SeLockMemoryPrivilege");
        CHECK(call("LookupPrivilegeValueA", 3, 0u, name, luid) == 1 && MEM32(luid) == 4 && MEM32(luid + 4) == 0);
        strcpy(hle_ptr(name), "SeNoSuchPrivilege");
        CHECK(call("LookupPrivilegeValueA", 3, 0u, name, luid) == 0 && call("GetLastError", 0) == 1313);
        CHECK(call("OpenProcessToken", 3, 0xFFFFFFFFu, 0x28u, tok) == 1 && MEM32(tok) != 0);
        MEM32(tp) = 1, MEM32(tp + 4) = 4, MEM32(tp + 8) = 0, MEM32(tp + 12) = 2;      /* SE_PRIVILEGE_ENABLED */
        CHECK(call("AdjustTokenPrivileges", 6, MEM32(tok), 0u, tp, 16u, 0u, 0u) == 1);
        CHECK(call("GetLastError", 0) == 1300);                                    /* ERROR_NOT_ALL_ASSIGNED */
        CHECK(call("CloseHandle", 1, MEM32(tok)) == 1);
    }

    /* ws2_32 (by ordinal): no network stack; the game carries on offline */
    {
        uint32_t wsa = gmem(400);
        CHECK(call("ws2_32.dll#115", 2, 0x202u, wsa) == 10091u);                    /* WSAStartup: WSASYSNOTREADY */
        CHECK(call("ws2_32.dll#111", 0) == 10093u);                                 /* WSAGetLastError: not initialised */
        CHECK(call("ws2_32.dll#116", 0) == 0xFFFFFFFFu);                            /* WSACleanup: SOCKET_ERROR */
    }

    /* ReadFileEx: the read is done at once, its completion routine runs at the thread's next alertable wait
       (SleepEx or WaitForSingleObjectEx with alertable TRUE), which then gives WAIT_IO_COMPLETION */
    {
        win32hle_register(t_shims);
        uint32_t routine = hle_resolve("selftest_completion");
        FILE *f = fopen("rfx.bin", "wb");
        for (int i = 0; i < 10000; i++) fputc(i * 7 & 0xFF, f);
        fclose(f);
        uint32_t name = gmem(16), buf = gmem(8192), ovl = gmem(20), ovl2 = gmem(20);
        strcpy(hle_ptr(name), "rfx.bin");
        uint32_t h = call("CreateFileA", 7, name, 0x80000000u, 1u, 0u, 3u, 0x48000000u, 0u);
        CHECK(h != 0xFFFFFFFFu);
        MEM32(ovl + 8) = 4096, MEM32(ovl + 12) = 0;
        CHECK(call("ReadFileEx", 5, h, buf, 4096u, ovl, routine) == 1 && g_ndone == 0);
        CHECK(call("SleepEx", 2, 0u, 0u) == 0 && g_ndone == 0);                      /* not alertable: nothing runs */
        CHECK(call("SleepEx", 2, 0u, 1u) == 0xC0u && g_ndone == 1);                  /* WAIT_IO_COMPLETION */
        CHECK(g_done[0][0] == 0 && g_done[0][1] == 4096 && g_done[0][2] == ovl);
        CHECK(MEM32(ovl) == 0 && MEM32(ovl + 4) == 4096);                              /* Internal, InternalHigh */
        int same = 1;
        for (int i = 0; i < 4096; i++) same &= MEM8(buf + (uint32_t)i) == ((4096 + i) * 7 & 0xFF);
        CHECK(same);
        CHECK(call("SleepEx", 2, 0u, 1u) == 0 && g_ndone == 1);                      /* nothing queued */
        MEM32(ovl + 8) = 0, MEM32(ovl2 + 8) = 8192;
        CHECK(call("ReadFileEx", 5, h, buf, 4096u, ovl, routine) == 1);
        CHECK(call("ReadFileEx", 5, h, buf + 4096, 4096u, ovl2, routine) == 1 && g_ndone == 1);
        uint32_t ev = call("CreateEventA", 4, 0u, 1u, 1u, 0u);
        CHECK(call("WaitForSingleObjectEx", 3, ev, 0u, 0u) == 0 && g_ndone == 1);    /* signalled, not alertable */
        CHECK(call("WaitForSingleObjectEx", 3, ev, 0xFFFFFFFFu, 1u) == 0xC0u && g_ndone == 3);
        CHECK(g_done[1][1] == 4096 && g_done[1][2] == ovl && g_done[2][1] == 10000 - 8192 && g_done[2][2] == ovl2);
        CHECK(MEM8(buf) == 0 && MEM8(buf + 4096) == (8192 * 7 & 0xFF));
        MEM32(ovl + 8) = 20000;
        CHECK(call("ReadFileEx", 5, h, buf, 4096u, ovl, routine) == 0 && call("GetLastError", 0) == 38);  /* ERROR_HANDLE_EOF */
        CHECK(call("ReadFileEx", 5, 0x7777u, buf, 4096u, ovl, routine) == 0 && call("GetLastError", 0) == 6);
        CHECK(call("SleepEx", 2, 0u, 1u) == 0 && g_ndone == 3);
        CHECK(call("CloseHandle", 1, h) == 1 && call("CloseHandle", 1, ev) == 1);
    }

    /* paths: GetFullPathNameA answers in the guest's own folders, as GetCurrentDirectoryA does (the game
       turns a full path back into one relative to its folder, so a host folder here breaks the name) */
    {
        CHECK(bw2_mount(".", "support") == 0);
        uint32_t name = gmem(64), buf = gmem(512), part = gmem(4), cwd = gmem(512);
        uint32_t n = call("GetCurrentDirectoryA", 2, 512u, cwd);
        CHECK(n == strlen(BW2_INSTALL_DIR) && !strcmp(hle_ptr(cwd), BW2_INSTALL_DIR));
        strcpy(hle_ptr(name), "data\\art\\t.dds");
        n = call("GetFullPathNameA", 4, name, 512u, buf, part);
        CHECK(!strcmp(hle_ptr(buf), BW2_INSTALL_DIR "\\data\\art\\t.dds") && n == strlen(hle_ptr(buf)));
        CHECK(MEM32(part) == buf + n - 5);                                          /* "t.dds" */
        strcpy(hle_ptr(name), ".\\data/./x\\..\\t.dds");
        call("GetFullPathNameA", 4, name, 512u, buf, 0u);
        CHECK(!strcmp(hle_ptr(buf), BW2_INSTALL_DIR "\\data\\t.dds"));
        strcpy(hle_ptr(name), "\\Temp\\a.txt");
        call("GetFullPathNameA", 4, name, 512u, buf, 0u);
        CHECK(!strcmp(hle_ptr(buf), "C:\\Temp\\a.txt"));
        strcpy(hle_ptr(name), "D:/x/../y.txt");
        call("GetFullPathNameA", 4, name, 512u, buf, 0u);
        CHECK(!strcmp(hle_ptr(buf), "D:\\y.txt"));
        strcpy(hle_ptr(name), "data\\art\\t.dds");
        n = call("GetFullPathNameA", 4, name, 8u, buf, 0u);                        /* too small: the size needed */
        CHECK(n == strlen(BW2_INSTALL_DIR "\\data\\art\\t.dds") + 1);
    }

    /* CreateFileA: a read-only OPEN_ALWAYS of an existing file opens it in place, as Windows does (the audio
       library opens its banks so); a missing one is still made in the overlay, a write still copies there */
    {
        struct stat st;
        mkdir("Audio", 0755);
        write_file("Audio/a.bin", "abc", 3);
        write_file("Audio/b.bin", "abc", 3);
        uint32_t name = gmem(64), buf = gmem(16), got = gmem(4);
        strcpy(hle_ptr(name), "Audio\\a.bin");
        uint32_t h = call("CreateFileA", 7, name, 0x80000000u, 1u, 0u, 3u, 0x80u, 0u);          /* OPEN_EXISTING */
        memset(hle_ptr(buf), 0, 16);
        CHECK(h != 0xFFFFFFFFu && call("ReadFile", 5, h, buf, 8u, got, 0u) == 1 && MEM32(got) == 3
              && !memcmp(hle_ptr(buf), "abc", 3));
        CHECK(call("CloseHandle", 1, h) == 1);
        strcpy(hle_ptr(name), "Audio\\new.bin");
        h = call("CreateFileA", 7, name, 0x80000000u, 0u, 0u, 4u, 0u, 0u);             /* missing: made in the overlay */
        CHECK(h != 0xFFFFFFFFu && call("CloseHandle", 1, h) == 1);
        CHECK(stat("support/Game/Audio/new.bin", &st) == 0 && stat("Audio/new.bin", &st) != 0);
        strcpy(hle_ptr(name), "Audio\\b.bin");
        h = call("CreateFileA", 7, name, 0xC0000000u, 0u, 0u, 4u, 0x80u, 0u);          /* read and write: copied */
        CHECK(h != 0xFFFFFFFFu && call("CloseHandle", 1, h) == 1);
        char c[8] = "";
        FILE *f = fopen("support/Game/Audio/b.bin", "rb");
        CHECK(f && fread(c, 1, sizeof c, f) == 3 && !memcmp(c, "abc", 3));
        if (f) fclose(f);
        strcpy(hle_ptr(name), "Audio\\a.bin");
        h = call("CreateFileA", 7, name, 0x80000000u, 1u, 0u, 4u, 0x80u, 0u);          /* read-only OPEN_ALWAYS */
        CHECK(h != 0xFFFFFFFFu && call("GetLastError", 0) == 183u);                    /* ERROR_ALREADY_EXISTS */
        memset(hle_ptr(buf), 0, 16);
        CHECK(call("ReadFile", 5, h, buf, 8u, got, 0u) == 1 && MEM32(got) == 3 && !memcmp(hle_ptr(buf), "abc", 3));
        CHECK(call("CloseHandle", 1, h) == 1);
        CHECK(stat("support/Game/Audio/a.bin", &st) != 0);                             /* no empty copy made */
    }

    /* GetKeyNameTextA and W: Windows' US English names. Bit 24 is extended, bit 25 "don't care" (merges
       only Ctrl and Shift). The game asks W first, then A when W gave nothing. */
    {
        int have_w = hle_resolve("GetKeyNameTextW") != 0;
        CHECK(have_w);
        for (int w = 0; w <= (have_w ? 1 : 0); w++) {
            key_name(w, 0x02360000u, 64u, "Shift");
            key_name(w, 0x00360000u, 64u, "Right Shift");
            key_name(w, 0x031D0000u, 64u, "Ctrl");
            key_name(w, 0x011D0000u, 64u, "Right Ctrl");
            key_name(w, 0x03480000u, 64u, "Up");
            key_name(w, 0x02480000u, 64u, "Num 8");
            key_name(w, 0x021E0000u, 64u, "A");
            key_name(w, 0x02390000u, 64u, "Space");
            key_name(w, 0x020C0000u, 64u, "-");
            key_name(w, 0x01380000u, 64u, "Right Alt");
            key_name(w, 0x02000000u, 64u, "");
            key_name(w, 0x000E0000u, 3u, "Ba");
            uint32_t buf = gmem(8);
            memset(hle_ptr(buf), 0x55, 8);
            CHECK(call(w ? "GetKeyNameTextW" : "GetKeyNameTextA", 3, 0x000E0000u, buf, 0u) == 0 && MEM8(buf) == 0x55);
        }
    }

    /* LoadLibraryA: Windows' default extension. A module already loaded is found with or without ".dll" */
    {
        uint32_t name = gmem(260), name2 = gmem(260);
        strcpy(hle_ptr(name), "kernel32");
        strcpy(hle_ptr(name2), "kernel32.dll");
        uint32_t k32 = call("LoadLibraryA", 1, name);
        CHECK(k32 != 0 && call("LoadLibraryA", 1, name2) == k32);
        CHECK(call("GetModuleHandleA", 1, name) == k32);
        strcpy(hle_ptr(name), "user32");
        strcpy(hle_ptr(name2), "USER32.DLL");
        uint32_t u32 = call("LoadLibraryA", 1, name);
        CHECK(u32 != 0 && u32 != k32 && call("LoadLibraryA", 1, name2) == u32);

        /* what the lifted-DLL hook is given: the base name, and the host file with the extension added */
        mkdir("support/C/PlugIns", 0755);
        mkdir("support/C/a.b", 0755);
        write_file("support/C/PlugIns/Thing.dll", "x", 1);
        write_file("support/C/a.b/Thing.dll", "x", 1);
        int (*saved)(const char *, const char *, uint32_t *) = hle_lifted_dll_hook;
        hle_lifted_dll_hook = t_hook;
        g_hook_r = 0;
        strcpy(hle_ptr(name), ".\\PlugIns\\Thing");
        g_hook_calls = 0;
        call("LoadLibraryA", 1, name);
        CHECK(g_hook_calls == 1 && !strcmp(g_hook_base, "thing.dll"));
        strcpy(hle_ptr(name), "Thing.bin");
        call("LoadLibraryA", 1, name);
        CHECK(!strcmp(g_hook_base, "thing.bin"));
        strcpy(hle_ptr(name), "Thing.");                            /* a trailing '.': no extension */
        call("LoadLibraryA", 1, name);
        CHECK(!strcmp(g_hook_base, "thing"));
        strcpy(hle_ptr(name), "C:\\PlugIns\\Thing");
        call("LoadLibraryA", 1, name);
        CHECK(g_hook_had_host && ends_with(g_hook_host, "Thing.dll"));
        strcpy(hle_ptr(name), "C:\\a.b\\Thing");                    /* a '.' in a folder is no extension */
        call("LoadLibraryA", 1, name);
        CHECK(!strcmp(g_hook_base, "thing.dll") && g_hook_had_host && ends_with(g_hook_host, "a.b/Thing.dll"));

        /* a hook that fails: "not found" only when there was no file to give it */
        char err[8192];
        g_hook_r = -1;
        g_child_name = name;
        strcpy(hle_ptr(name), "C:\\NoSuch\\Thing");
        CHECK(in_child(c_load, err, sizeof err) == 0 && count_of(err, "not found") == 1);
        strcpy(hle_ptr(name), "C:\\PlugIns\\Thing");
        CHECK(in_child(c_load, err, sizeof err) == 0 && count_of(err, "not found") == 0);
        hle_lifted_dll_hook = saved;
    }

    /* ole32: COM initialization, counted per thread */
    {
        CHECK(call("CoInitializeEx", 2, 0u, 2u) == 0);              /* COINIT_APARTMENTTHREADED: S_OK */
        CHECK(call("CoInitializeEx", 2, 0u, 2u) == 1);              /* S_FALSE: already */
        CHECK(call("CoInitializeEx", 2, 0u, 0u) == 0x80010106u);    /* RPC_E_CHANGED_MODE */
        CHECK(call("CoInitializeEx", 2, 1u, 2u) == 0x80070057u);    /* reserved not NULL: E_INVALIDARG */
        uint32_t tid = gmem(4);
        uint32_t th = call("CreateThread", 6, 0u, 0u, hle_resolve("selftest_coinit_thread"), 0u, 0u, tid);
        CHECK(th != 0 && call("WaitForSingleObject", 2, th, 10000u) == 0);
        CHECK(g_thread_coinit == 0);                                /* its own apartment: S_OK */
        call("CoUninitialize", 0);
        call("CoUninitialize", 0);
        CHECK(call("CoInitializeEx", 2, 0u, 0u) == 0);              /* uninitialized again: any model */
        call("CoUninitialize", 0);
    }

    /* ole32: CoFileTimeToDosDateTime, no time-zone conversion */
    {
        uint32_t ft = gmem(8), d = gmem(4), t = gmem(4);
        MEM32(ft) = 0x85E7D800u, MEM32(ft + 4) = 0x01C5CB3Bu;     /* 2005-10-07 12:34:56 */
        MEM32(d) = MEM32(t) = 0;
        CHECK(call("CoFileTimeToDosDateTime", 3, ft, d, t) == 1 && MEM16(d) == 0x3347 && MEM16(t) == 0x645C);
        MEM32(ft) = 0xE0A45300u, MEM32(ft + 4) = 0x01A8E79Fu;     /* 1979-12-31 23:59:58 */
        CHECK(call("CoFileTimeToDosDateTime", 3, ft, d, t) == 0);
    }

    /* LoadImageA from a file: a missing one is NULL with ERROR_FILE_NOT_FOUND. One that is there is
       win32hle's, and this engine links no GDI bitmaps (gdidc.c), so a real BMP stops the run, naming
       what is missing; a file win32hle cannot read as a bitmap stops it, naming the file */
    {
        char err[8192];
        uint32_t name = gmem(64);
        strcpy(hle_ptr(name), "Data\\NoSuchFile.bmp");
        CHECK(call("LoadImageA", 6, 0u, name, 0u, 0u, 0u, 0x2010u) == 0 && call("GetLastError", 0) == 2);
        strcpy(hle_ptr(name), "C:\\NoSuchFile.bmp");
        CHECK(call("LoadImageA", 6, 0u, name, 0u, 0u, 0u, 0x2010u) == 0 && call("GetLastError", 0) == 2);
        uint8_t bmp[70] = { 'B', 'M', 70, 0, 0, 0, 0, 0, 0, 0, 54, 0, 0, 0,
                            40, 0, 0, 0, 2, 0, 0, 0, 2, 0, 0, 0, 1, 0, 24, 0 };     /* 2x2, 24 bits, rows of 8 */
        for (int i = 54; i < 70; i++) bmp[i] = (uint8_t)(i * 13);
        write_file("support/C/two.bmp", bmp, sizeof bmp);
        write_file("support/C/text.bmp", "not a bitmap", 12);
        hle_path_changed();
        g_child_name = name;
        strcpy(hle_ptr(name), "C:\\two.bmp");
        CHECK(in_child(c_load_image, err, sizeof err) == -1 && strstr(err, "hle_gdi_make_dib (gdidc.c): not linked"));
        strcpy(hle_ptr(name), "C:\\text.bmp");
        CHECK(in_child(c_load_image, err, sizeof err) == -1
              && strstr(err, "LoadImageA: \"C:\\text.bmp\" is not a bitmap win32hle reads"));
        CHECK(call("LoadImageA", 6, 0u, 0u, 2u, 0u, 0u, 0x10u) == 0x1C000003u);  /* an icon: win32hle's placeholder */
    }

    /* OpenAL is a later plan's (plan 8 answers DirectSound): each load exits 4, naming it */
    {
        char err[8192];
        uint32_t name = gmem(32);
        g_child_name = name;
        strcpy(hle_ptr(name), "wrap_oal.dll");
        CHECK(in_child(c_load, err, sizeof err) == 4 && strstr(err, "reached wrap_oal.dll: owned by a later plan"));
        strcpy(hle_ptr(name), "OPENAL32");
        CHECK(in_child(c_load, err, sizeof err) == 4 && strstr(err, "reached openal32.dll: owned by a later plan"));
    }

    /* the profile dialog's name field (plan 6 Task 6): code page 1252 has no DBCS lead bytes, and
       PathGetCharTypeW gives shlwapi's GCT_* classes (0 invalid, 1 long names only, 3 both, 4 wild,
       8 separator); a WCHAR is the low 16 bits of its argument slot */
    {
        static const uint32_t bytes[] = { 0x00, 0x41, 0x81, 0xE0, 0xFF, 0x12345681u };
        for (size_t i = 0; i < sizeof bytes / sizeof *bytes; i++) {
            uint32_t r = call("IsDBCSLeadByte", 1, bytes[i]);
            if (r != 0) printf("FAIL IsDBCSLeadByte(0x%X) = %u\n", bytes[i], r), fails++;
        }
        CHECK(call("GetACP", 0) == 1252);
        static const struct { uint32_t ch, want; } ct[] = {
            { 'a', 3 }, { 'P', 3 }, { '0', 3 }, { '.', 3 }, { '~', 3 }, { 0xE9, 3 }, { '_', 3 }, { '-', 3 },
            { 0x4E2D, 3 }, { 0x7F, 3 }, { 0xFFFF, 3 },
            { ' ', 1 }, { ';', 1 }, { ',', 1 }, { '+', 1 }, { '=', 1 }, { '[', 1 }, { ']', 1 },
            { '*', 4 }, { '?', 4 },
            { '\\', 8 }, { '/', 8 }, { ':', 8 },
            { 0x00, 0 }, { 0x09, 0 }, { 0x1F, 0 }, { '"', 0 }, { '<', 0 }, { '>', 0 }, { '|', 0 },
            { 0xABCD002Au, 4 }, { 0x00010041u, 3 } };
        for (size_t i = 0; i < sizeof ct / sizeof *ct; i++) {
            uint32_t r = call("PathGetCharTypeW", 1, ct[i].ch);
            if (r != ct[i].want) printf("FAIL PathGetCharTypeW(0x%X) = %u, want %u\n", ct[i].ch, r, ct[i].want), fails++;
        }
    }

    /* ---- msvcrt for the lifted D3DX (plan 9 Task 3): cdecl, the caller pops ---- */

    /* crt_heap */
    {
        uint32_t p = ccall("malloc", 1, 100u);
        CHECK(p != 0 && (p & 7) == 0);
        for (uint32_t i = 0; i < 100; i++) MEM8(p + i) = (uint8_t)(i * 3);
        uint32_t q = ccall("realloc", 2, p, 1000u);
        int kept = q != 0;
        for (uint32_t i = 0; kept && i < 100; i++) kept = MEM8(q + i) == (uint8_t)(i * 3);
        CHECK(kept);
        uint32_t c = ccall("calloc", 2, 10u, 10u);
        int zero = c != 0;
        for (uint32_t i = 0; zero && i < 100; i++) zero = MEM8(c + i) == 0;
        CHECK(zero);
        ccall("free", 1, 0u);
        ccall("??3@YAXPAX@Z", 1, 0u);
        ccall("free", 1, q);
        ccall("free", 1, c);
        CHECK(ccall("??2@YAPAXI@Z", 1, 16u) != 0);
        uint32_t s = gstr("ab");
        uint32_t d = ccall("_strdup", 1, s);
        CHECK(d != 0 && d != s && !strcmp(hle_ptr(d), "ab"));
        CHECK(ccall("realloc", 2, 0u, 8u) != 0);                 /* realloc(NULL, n) is malloc(n) */
        CHECK(ccall("malloc", 1, 0xFFFFFFF0u) == 0);              /* a failed allocation is 0 */
    }

    /* crt_string */
    {
        uint32_t abc = gstr("abc");
        CHECK(ccall("strchr", 2, abc, (uint32_t)'c') == abc + 2);
        CHECK(ccall("strchr", 2, abc, 0u) == abc + 3);
        CHECK(ccall("strchr", 2, abc, (uint32_t)'z') == 0);
        CHECK((int32_t)ccall("_stricmp", 2, gstr("ABC"), gstr("abd")) < 0);
        CHECK(ccall("_stricmp", 2, gstr("aBc"), gstr("AbC")) == 0);
        uint32_t m = gstr("0123456789");
        CHECK(ccall("memmove", 3, m + 2, m, 5u) == m + 2 && !strcmp(hle_ptr(m), "0101234789"));
        m = gstr("0123456789");
        CHECK(ccall("memmove", 3, m, m + 2, 5u) == m && !strcmp(hle_ptr(m), "2345656789"));
        uint32_t w = gmem(8);
        MEM16(w) = 'a', MEM16(w + 2) = 0x4E2D, MEM16(w + 4) = 'c', MEM16(w + 6) = 0;
        CHECK(ccall("wcslen", 1, w) == 3);
        CHECK(ccall("isalpha", 1, 0xE9u) == 0 && ccall("isalpha", 1, (uint32_t)'q') != 0);
        CHECK(ccall("tolower", 1, 0xC9u) == 0xC9u && ccall("tolower", 1, (uint32_t)'Q') == 'q');
        CHECK(ccall("toupper", 1, (uint32_t)'q') == 'Q' && ccall("toupper", 1, 0xE9u) == 0xE9u);
        CHECK(ccall("isdigit", 1, (uint32_t)'7') != 0 && ccall("isdigit", 1, (uint32_t)'a') == 0);
        CHECK(ccall("isxdigit", 1, (uint32_t)'F') != 0 && ccall("isxdigit", 1, (uint32_t)'g') == 0);
        CHECK(ccall("isalnum", 1, (uint32_t)'_') == 0 && ccall("isalnum", 1, (uint32_t)'5') != 0);
        CHECK(ccall("isspace", 1, (uint32_t)'\t') != 0 && ccall("isspace", 1, 0xA0u) == 0);
        CHECK(ccall("isalpha", 1, 0xFFFFFFFFu) == 0);                /* EOF */
    }

    /* crt_numbers: results that are doubles come back in st(0) */
    {
        CHECK((int32_t)ccall("atoi", 1, gstr("-42x")) == -42);
        CHECK((int32_t)ccall("atol", 1, gstr("  +17")) == 17);
        int top = g_fp_top;
        ccall("atof", 1, gstr("1.5e2"));
        CHECK(g_fp_top == top + 1 && g_st[0] == 150.0);
        ccall("atof", 1, gstr("2.5d1"));                              /* msvcrt also takes a 'd' exponent */
        CHECK(g_fp_top == top + 2 && g_st[0] == 25.0 && g_st[1] == 150.0);
        ccall("floor", 2, DLO(-1.5), DHI(-1.5));
        CHECK(g_st[0] == -2.0);
        ccall("ceil", 2, DLO(-1.5), DHI(-1.5));
        CHECK(g_st[0] == -1.0);
        ccall("ldexp", 3, DLO(3.0), DHI(3.0), 4u);
        CHECK(g_st[0] == 48.0);
        uint32_t e = gmem(8);
        ccall("frexp", 3, DLO(8.0), DHI(8.0), e);
        CHECK(g_st[0] == 0.5 && MEM32(e) == 4);
        ccall("modf", 3, DLO(2.75), DHI(2.75), e);
        CHECK(g_st[0] == 0.75 && mem_double(e) == 2.0);
        CHECK(g_fp_top == top + 7);
        while (g_fp_top > top) fp_pop_impl(g_st, &g_fp_top);
        double nan = __builtin_nan(""), inf = __builtin_inf();
        CHECK(ccall("_isnan", 2, DLO(nan), DHI(nan)) == 1 && ccall("_isnan", 2, DLO(1.0), DHI(1.0)) == 0);
        CHECK(ccall("_finite", 2, DLO(inf), DHI(inf)) == 0 && ccall("_finite", 2, DLO(1.0), DHI(1.0)) != 0);
        CHECK(ccall("_fpclass", 2, DLO(-0.0), DHI(-0.0)) == 0x20);
        CHECK(ccall("_fpclass", 2, DLO(-inf), DHI(-inf)) == 0x4 && ccall("_fpclass", 2, DLO(2.0), DHI(2.0)) == 0x100);
        CHECK(ccall("_fpclass", 2, DLO(nan), DHI(nan)) == 0x2 && ccall("_fpclass", 2, DLO(5e-320), DHI(5e-320)) == 0x80);
        CHECK(g_fp_top == top);
    }

    /* crt_x87: the helpers take st(0) (and st(1)) and leave what lies under them */
    {
        int top = g_fp_top;
        fp_push_impl(g_st, &g_fp_top, 7.0);
        fp_push_impl(g_st, &g_fp_top, 2.0);
        fp_push_impl(g_st, &g_fp_top, 3.0);
        ccall("_CIpow", 0);
        CHECK(g_st[0] == 8.0 && g_st[1] == 7.0 && g_fp_top == top + 2);
        fp_pop_impl(g_st, &g_fp_top);
        fp_push_impl(g_st, &g_fp_top, 3.0);                           /* 7.0 under 3.0 */
        ccall("_CIfmod", 0);
        CHECK(g_st[0] == 1.0 && g_fp_top == top + 1);
        fp_push_impl(g_st, &g_fp_top, 16.0);
        ccall("_CIsqrt", 0);
        CHECK(g_st[0] == 4.0 && g_st[1] == 1.0 && g_fp_top == top + 2);
        fp_push_impl(g_st, &g_fp_top, 1.0);
        ccall("_CIacos", 0);
        CHECK(g_st[0] == 0.0 && g_fp_top == top + 3);
        fp_push_impl(g_st, &g_fp_top, 0.0);
        ccall("_CIexp", 0);
        CHECK(g_st[0] == 1.0 && g_fp_top == top + 4);
        while (g_fp_top > top) fp_pop_impl(g_st, &g_fp_top);
    }

    /* crt_controlfp: _controlfp's encoding mapped to and from the x87 control word */
    {
        uint16_t cw = g_fpu_cw;
        uint32_t w = ccall("_controlfp", 2, 0x300u /* _RC_CHOP */, 0x300u /* _MCW_RC */);
        CHECK(((g_fpu_cw >> 10) & 3) == 3 && (w & 0x300u) == 0x300u);
        CHECK((g_fpu_cw & ~0x0C00u) == (cw & ~0x0C00u));
        uint16_t now = g_fpu_cw;
        CHECK(ccall("_controlfp", 2, 0u, 0u) == w && g_fpu_cw == now);
        w = ccall("_controlfp", 2, 0x20000u /* _PC_24 */, 0x30000u /* _MCW_PC */);
        CHECK(((g_fpu_cw >> 8) & 3) == 0 && (w & 0x30000u) == 0x20000u);
        w = ccall("_controlfp", 2, 0x10000u /* _PC_53 */, 0x30000u);
        CHECK(((g_fpu_cw >> 8) & 3) == 2 && (w & 0x30000u) == 0x10000u);
        w = ccall("_controlfp", 2, 0u /* _EM_ZERODIVIDE unmasked */, 0x08u);
        CHECK((g_fpu_cw & 4) == 0 && (w & 0x08u) == 0);
        g_fpu_cw = 0x027F;                                         /* Windows' default: all masked, 53 bits, near */
        CHECK(ccall("_controlfp", 2, 0u, 0u) == 0x0009001Fu);
        CHECK(ccall("_controlfp", 2, 0u, 0x80000u /* _EM_DENORMAL: _controlfp leaves it */) == 0x0009001Fu && g_fpu_cw == 0x027F);
        g_fpu_cw = cw;
    }

    /* crt_vsnprintf: msvcrt's three cases */
    {
        uint32_t va = gmem(8), fmt = gstr("%d-%s"), buf = gmem(32);
        MEM32(va) = 7, MEM32(va + 4) = gstr("ab");
        memset(hle_ptr(buf), 'Z', 32);
        CHECK(ccall("_vsnprintf", 4, buf, 16u, fmt, va) == 4 && !memcmp(hle_ptr(buf), "7-ab\0", 5));
        memset(hle_ptr(buf), 'Z', 32);
        CHECK(ccall("_vsnprintf", 4, buf, 4u, fmt, va) == 4 && !memcmp(hle_ptr(buf), "7-abZ", 5));
        memset(hle_ptr(buf), 'Z', 32);
        CHECK(ccall("_vsnprintf", 4, buf, 3u, fmt, va) == 0xFFFFFFFFu && !memcmp(hle_ptr(buf), "7-aZ", 4));
        /* a long %s is not cut, and msvcrt's exponents have three digits */
        char lng[701];
        memset(lng, 'q', 700), lng[700] = 0;
        uint32_t big = gmem(1024);
        MEM32(va) = gstr(lng);
        CHECK(ccall("_vsnprintf", 4, big, 1024u, gstr("<%s>"), va) == 702 && MEM8(big + 701) == '>');
        uint32_t va2 = gmem(24);
        MEM32(va2) = DLO(1500.0), MEM32(va2 + 4) = DHI(1500.0), MEM32(va2 + 8) = DLO(1500.0), MEM32(va2 + 12) = DHI(1500.0);
        MEM32(va2 + 16) = DLO(0.5), MEM32(va2 + 20) = DHI(0.5);
        CHECK(ccall("_vsnprintf", 4, big, 64u, gstr("%e|%11.1E|%g"), va2) == 29
              && !strcmp(hle_ptr(big), "1.500000e+003|   1.5E+003|0.5"));
        CHECK(ccall("_vsnprintf", 4, big, 64u, gstr("%-11.1e|%010.1e|"), va2) == 23
              && !strcmp(hle_ptr(big), "1.5e+003   |001.5e+003|"));
        uint32_t va3 = gmem(12);
        MEM32(va3) = (uint32_t)-5, MEM32(va3 + 4) = gstr("x"), MEM32(va3 + 8) = 0xAB;
        CHECK(ccall("_vsnprintf", 4, big, 64u, gstr("[%*s]%p"), va3) == 15 && !strcmp(hle_ptr(big), "[x    ]000000AB"));
    }

    /* crt_data_import: _adjust_fdiv is a dword in mapped guest memory, not a shim address */
    {
        uint32_t a = bw2_bind_resolve("_adjust_fdiv");
        CHECK(a != 0 && !(a >= 0x7F000000u && a <= 0x7FFFFFFFu) && hle_ptr(a) && MEM32(a) == 0);
        CHECK(bw2_bind_resolve("_adjust_fdiv") == a);
        CHECK(bw2_bind_resolve("GetTickCount") == hle_resolve_or_stub("GetTickCount"));
    }

    /* k32_dll_features: DisableThreadLibraryCalls, and IsProcessorFeaturePresent from cpu.c's CPU */
    {
        CHECK(call("DisableThreadLibraryCalls", 1, 0x08000000u) == 1);
        static const struct { uint32_t pf; int bit; } feat[] = {
            { 2, 8 }, { 3, 23 }, { 6, 25 }, { 8, 4 }, { 9, 6 }, { 10, 26 } };
        uint32_t sa = g_eax, sb = g_ebx, sc = g_ecx, sd = g_edx;
        recomp_cpuid(1, 0);
        uint32_t edx1 = g_edx;
        g_eax = sa, g_ebx = sb, g_ecx = sc, g_edx = sd;
        for (size_t i = 0; i < sizeof feat / sizeof *feat; i++) {
            uint32_t r = call("IsProcessorFeaturePresent", 1, feat[i].pf);
            if (r != 1 || r != ((edx1 >> feat[i].bit) & 1))
                printf("FAIL IsProcessorFeaturePresent(%u) = %u, cpuid bit %d = %u\n", feat[i].pf, r, feat[i].bit,
                       (edx1 >> feat[i].bit) & 1), fails++;
        }
        static const uint32_t absent[] = { 0, 7, 12, 13, 0xFFFF };
        for (size_t i = 0; i < sizeof absent / sizeof *absent; i++)
            if (call("IsProcessorFeaturePresent", 1, absent[i]) != 0)
                printf("FAIL IsProcessorFeaturePresent(%u) is not 0\n", absent[i]), fails++;
    }

    /* crt_initterm_qsort */
    {
        uint32_t f1 = MEM32(hle_com_vtable(t_init1_tab)), f2 = MEM32(hle_com_vtable(t_init2_tab));
        uint32_t cmp = MEM32(hle_com_vtable(t_cmp_tab));
        uint32_t t = gmem(12);
        MEM32(t) = f1, MEM32(t + 4) = 0, MEM32(t + 8) = f2;
        ccall("_initterm", 2, t, t + 12);
        CHECK(g_ninit == 2 && g_init_order[0] == 1 && g_init_order[1] == 2);
        static const int32_t in[5] = { 5, -3, 9, 0, -3 }, want[5] = { -3, -3, 0, 5, 9 };
        uint32_t a = gmem(20);
        for (int i = 0; i < 5; i++) MEM32(a + 4u * (uint32_t)i) = (uint32_t)in[i];
        ccall("qsort", 4, a, 5u, 4u, cmp);
        int sorted = 1;
        for (int i = 0; i < 5; i++) sorted &= (int32_t)MEM32(a + 4u * (uint32_t)i) == want[i];
        CHECK(sorted);
    }

    /* crt_setlocale: the "C" locale only */
    {
        uint32_t r = ccall("setlocale", 2, 4u /* LC_NUMERIC */, gstr("C"));
        CHECK(r != 0 && !strcmp(hle_ptr(r), "C"));
        r = ccall("setlocale", 2, 0u, 0u);
        CHECK(r != 0 && !strcmp(hle_ptr(r), "C"));
        CHECK(ccall("setlocale", 2, 0u, gstr("German")) == 0);
    }

    /* crt_purecall: a loud stop */
    {
        char err[8192];
        CHECK(in_child(c_purecall, err, sizeof err) == -1 && strstr(err, "[bw2] FATAL: msvcrt: _purecall"));
    }

    printf("shims selftest: %d failed\n", fails);
    return fails != 0;
}
