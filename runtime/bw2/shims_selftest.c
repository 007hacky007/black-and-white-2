/*
 * shims_selftest.c - BW2's own import answers, called as lifted code calls
 * them: arguments pushed right to left, a return address, the shim. Each
 * check also verifies the shim popped exactly its stdcall arguments. Built
 * with the engine (minus main.c) by tests/test_engine.py test_shims_selftest.
 * Every shim the boot needs gets its check here first (plan 3 Task 10).
 */
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
static void c_dsound_class(void) {
    static const uint8_t clsid[16] = { 0x46, 0xD9, 0xD4, 0x47, 0xE8, 0x62, 0xCF, 0x11,
                                       0x93, 0xBC, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00 };
    static const uint8_t iid[16] = { 0, 0, 0, 0, 0, 0, 0, 0, 0xC0, 0, 0, 0, 0, 0, 0, 0x46 };
    uint32_t c = gmem(16), i = gmem(16), out = gmem(4);
    memcpy(hle_ptr(c), clsid, 16), memcpy(hle_ptr(i), iid, 16);
    call("CoCreateInstance", 5, c, 0u, 1u, i, out);
}

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

    /* the audio layer is a later plan's: each stop exits 4, naming it */
    {
        char err[8192];
        uint32_t name = gmem(32);
        strcpy(hle_ptr(name), "dsound.dll");
        g_child_name = name;
        CHECK(in_child(c_load, err, sizeof err) == 4 && strstr(err, "reached dsound.dll: owned by a later plan"));
        CHECK(in_child(c_dsound_class, err, sizeof err) == 4
              && strstr(err, "reached DirectSound (CoCreateInstance): owned by a later plan"));
    }

    printf("shims selftest: %d failed\n", fails);
    return fails != 0;
}
