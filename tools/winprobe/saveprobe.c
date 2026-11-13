/*
 * saveprobe.c - what Windows answers for the file calls the game's save path
 * makes (plan 11 Task 2). Ours, MIT. Built on the Mac with mingw-w64
 * (tools/winprobe/build.sh) and run on a Windows machine:
 *
 *   saveprobe.exe <work folder>
 *
 * One line per case, "<case> ret=<hex> err=<decimal>[ <extra>]": ret is the
 * call's return (a HANDLE prints as 1 when valid, ffffffff when it is
 * INVALID_HANDLE_VALUE), err is GetLastError() right after the call (set to
 * 0 before it). Everything the probe makes is inside the work folder, and it
 * removes all of it before it exits (the work folder itself is made if it is
 * missing and left in place).
 */
#include <windows.h>
#include <shlobj.h>
#include <objbase.h>
#include <stdio.h>
#include <string.h>

static char W[MAX_PATH];

static const char *at(const char *rel) {          /* W\rel, in one of a few rotating buffers */
    static char buf[8][MAX_PATH * 2];
    static int i;
    char *b = buf[i++ & 7];
    snprintf(b, MAX_PATH * 2, "%s\\%s", W, rel);
    return b;
}

static void line(const char *name, DWORD ret, DWORD err, const char *extra) {
    printf("%s ret=%lx err=%lu%s%s\n", name, (unsigned long)ret, (unsigned long)err, extra ? " " : "", extra ? extra : "");
    fflush(stdout);
}
static DWORD hret(HANDLE h) { return h == INVALID_HANDLE_VALUE ? 0xFFFFFFFFu : 1u; }

static void make_file(const char *rel, const char *text) {
    HANDLE h = CreateFileA(at(rel), GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    DWORD n;
    if (h == INVALID_HANDLE_VALUE) { printf("setup: cannot make %s (%lu)\n", rel, GetLastError()); return; }
    WriteFile(h, text, (DWORD)strlen(text), &n, NULL);
    CloseHandle(h);
}
static void make_dir(const char *rel) {
    if (!CreateDirectoryA(at(rel), NULL)) printf("setup: cannot make %s\\ (%lu)\n", rel, GetLastError());
}
static int exists(const char *rel) { return GetFileAttributesA(at(rel)) != INVALID_FILE_ATTRIBUTES; }

/* Everything inside dir (a full path), then dir itself if self. */
static void rm_tree(const char *dir, int self) {
    char pat[MAX_PATH * 2], sub[MAX_PATH * 2];
    WIN32_FIND_DATAA fd;
    snprintf(pat, sizeof pat, "%s\\*", dir);
    HANDLE h = FindFirstFileA(pat, &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (!strcmp(fd.cFileName, ".") || !strcmp(fd.cFileName, "..")) continue;
            snprintf(sub, sizeof sub, "%s\\%s", dir, fd.cFileName);
            SetFileAttributesA(sub, FILE_ATTRIBUTE_NORMAL);
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) rm_tree(sub, 1);
            else if (!DeleteFileA(sub)) printf("cleanup: cannot delete %s (%lu)\n", sub, GetLastError());
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    }
    if (self && !RemoveDirectoryA(dir)) printf("cleanup: cannot remove %s (%lu)\n", dir, GetLastError());
}

#define CALL(var, expr) do { SetLastError(0); var = (expr); err = GetLastError(); } while (0)

int main(int argc, char **argv) {
    DWORD r, err;
    HANDLE h;
    char extra[2048];
    if (argc != 2) { fprintf(stderr, "usage: saveprobe <work folder>\n"); return 2; }
    snprintf(W, sizeof W, "%s", argv[1]);
    CreateDirectoryA(W, NULL);
    if (!SetCurrentDirectoryA(W)) { fprintf(stderr, "saveprobe: no work folder %s\n", W); return 2; }
    rm_tree(W, 0);

    /* CreateDirectoryA */
    CALL(r, CreateDirectoryA(at("d1"), NULL)); line("mkdir_new", r, err, NULL);
    CALL(r, CreateDirectoryA(at("d1"), NULL)); line("mkdir_exists", r, err, NULL);

    /* SHCreateDirectoryExA(NULL, ...): returns an error code */
    CALL(r, (DWORD)SHCreateDirectoryExA(NULL, at("s1\\s2\\s3"), NULL));
    snprintf(extra, sizeof extra, "made=%d", exists("s1\\s2\\s3"));
    line("shmkdir_nested_new", r, err, extra);
    CALL(r, (DWORD)SHCreateDirectoryExA(NULL, at("s1\\s2\\s3"), NULL)); line("shmkdir_exists", r, err, NULL);
    CALL(r, (DWORD)SHCreateDirectoryExA(NULL, "rel1\\rel2", NULL));       /* the current folder is W */
    snprintf(extra, sizeof extra, "made=%d", exists("rel1"));
    line("shmkdir_relative", r, err, extra);

    /* GetFileAttributesA */
    make_file("f.txt", "hello");
    CALL(r, GetFileAttributesA(at("d1"))); line("attr_dir", r, err, NULL);
    CALL(r, GetFileAttributesA(at("d1\\"))); line("attr_dir_slash", r, err, NULL);
    CALL(r, GetFileAttributesA(at("f.txt"))); line("attr_file", r, err, NULL);
    CALL(r, GetFileAttributesA(at("d1\\nofile"))); line("attr_missing_file", r, err, NULL);
    CALL(r, GetFileAttributesA(at("nodir\\nofile"))); line("attr_missing_dir", r, err, NULL);

    /* FindFirstFileA / FindNextFileA */
    {
        static const char *names[] = { "a_b", "aB", "a", "A1", "~x", "_y", "Zeta", "b.txt", "[q", "^r" };
        WIN32_FIND_DATAA fd;
        char rel[64];
        make_dir("f");
        for (unsigned i = 0; i < sizeof names / sizeof *names; i++) {
            snprintf(rel, sizeof rel, "f\\%s", names[i]);
            make_file(rel, "x");
        }
        make_dir("f\\sub");
        CALL(h, FindFirstFileA(at("f\\*"), &fd));
        r = hret(h);
        extra[0] = 0;
        if (h != INVALID_HANDLE_VALUE) {
            DWORD more, e2, e1 = err;
            snprintf(extra, sizeof extra, "names=%s", fd.cFileName);
            for (;;) {
                CALL(more, FindNextFileA(h, &fd));
                e2 = err;
                if (!more) break;
                strncat(extra, "/", sizeof extra - strlen(extra) - 1);
                strncat(extra, fd.cFileName, sizeof extra - strlen(extra) - 1);
            }
            line("find_order", r, e1, extra);
            line("find_order_end", more, e2, NULL);
            FindClose(h);
        } else {
            line("find_order", r, err, NULL);
        }
        CALL(h, FindFirstFileA(at("f\\sub"), &fd));
        r = hret(h);
        snprintf(extra, sizeof extra, "attr=0x%lx size=%lu:%lu name=%s", (unsigned long)fd.dwFileAttributes,
                 (unsigned long)fd.nFileSizeHigh, (unsigned long)fd.nFileSizeLow, h != INVALID_HANDLE_VALUE ? fd.cFileName : "-");
        line("find_dir_data", r, err, extra);
        if (h != INVALID_HANDLE_VALUE) FindClose(h);
        CALL(h, FindFirstFileA(at("nodir\\*"), &fd));
        line("find_missing_dir", hret(h), err, NULL);
        if (h != INVALID_HANDLE_VALUE) FindClose(h);
        make_dir("empty");
        CALL(h, FindFirstFileA(at("empty\\*.sav"), &fd));
        line("find_no_match", hret(h), err, NULL);
        if (h != INVALID_HANDLE_VALUE) FindClose(h);
    }

    /* MoveFileA: extra is which of source and target exist after the call */
    make_dir("m");
    make_file("m\\a", "aaa");
    make_file("m\\c", "c");
    make_dir("m\\d1");
    make_file("m\\d1\\in", "i");
    make_dir("m\\d3");
    CALL(r, MoveFileA(at("m\\a"), at("m\\b")));
    snprintf(extra, sizeof extra, "src=%d dst=%d", exists("m\\a"), exists("m\\b"));
    line("move_file_new", r, err, extra);
    CALL(r, MoveFileA(at("m\\b"), at("m\\c")));
    snprintf(extra, sizeof extra, "src=%d dst=%d", exists("m\\b"), exists("m\\c"));
    line("move_file_onto", r, err, extra);
    CALL(r, MoveFileA(at("m\\d1"), at("m\\d2")));
    snprintf(extra, sizeof extra, "src=%d dst=%d inner=%d", exists("m\\d1"), exists("m\\d2"), exists("m\\d2\\in"));
    line("move_dir_new", r, err, extra);
    CALL(r, MoveFileA(at("m\\d2"), at("m\\d3")));
    snprintf(extra, sizeof extra, "src=%d dst=%d", exists("m\\d2"), exists("m\\d3"));
    line("move_dir_onto", r, err, extra);
    CALL(r, MoveFileA(at("m\\nothere"), at("m\\x")));
    snprintf(extra, sizeof extra, "dst=%d", exists("m\\x"));
    line("move_missing", r, err, extra);

    /* DeleteFileA */
    make_dir("x");
    make_dir("x\\dir");
    make_file("x\\ro", "r");
    SetFileAttributesA(at("x\\ro"), FILE_ATTRIBUTE_READONLY);
    CALL(r, DeleteFileA(at("x\\missing"))); line("delete_missing", r, err, NULL);
    CALL(r, DeleteFileA(at("x\\dir")));
    snprintf(extra, sizeof extra, "after=%d", exists("x\\dir"));
    line("delete_dir", r, err, extra);
    CALL(r, DeleteFileA(at("x\\ro")));
    snprintf(extra, sizeof extra, "after=%d", exists("x\\ro"));
    line("delete_readonly", r, err, extra);
    SetFileAttributesA(at("x\\ro"), FILE_ATTRIBUTE_NORMAL);

    /* RemoveDirectoryA */
    make_dir("r");
    make_dir("r\\full");
    make_file("r\\full\\f", "f");
    make_dir("r\\e1");
    make_dir("r\\e2");
    CALL(r, RemoveDirectoryA(at("r\\full")));
    snprintf(extra, sizeof extra, "after=%d", exists("r\\full\\f"));
    line("rmdir_nonempty", r, err, extra);
    CALL(r, RemoveDirectoryA(at("r\\missing"))); line("rmdir_missing", r, err, NULL);
    CALL(r, RemoveDirectoryA(at("r\\e1")));
    snprintf(extra, sizeof extra, "after=%d", exists("r\\e1"));
    line("rmdir_empty", r, err, extra);
    CALL(r, RemoveDirectoryA(at("r\\e2\\")));
    snprintf(extra, sizeof extra, "after=%d", exists("r\\e2"));
    line("rmdir_slash", r, err, extra);

    /* CreateFileA dispositions: err is the last error after a successful open too */
    make_file("c_exists", "12345");
    CALL(h, CreateFileA(at("c_exists"), GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL));
    snprintf(extra, sizeof extra, "size=%lu", h != INVALID_HANDLE_VALUE ? (unsigned long)GetFileSize(h, NULL) : 0ul);
    line("create_always_exists", hret(h), err, extra);
    if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
    make_file("c_exists", "12345");
    CALL(h, CreateFileA(at("c_exists"), GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL));
    snprintf(extra, sizeof extra, "size=%lu", h != INVALID_HANDLE_VALUE ? (unsigned long)GetFileSize(h, NULL) : 0ul);
    line("open_always_exists", hret(h), err, extra);
    if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
    CALL(h, CreateFileA(at("c_new"), GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL));
    snprintf(extra, sizeof extra, "made=%d", exists("c_new"));
    line("open_always_new", hret(h), err, extra);
    if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
    CALL(h, CreateFileA(at("c_exists"), GENERIC_WRITE, 0, NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL));
    line("create_new_exists", hret(h), err, NULL);
    if (h != INVALID_HANDLE_VALUE) CloseHandle(h);

    /* CoFileTimeToDosDateTime of 2026-11-13 12:34:56.500 UTC */
    {
        SYSTEMTIME st = { 2026, 11, 5, 13, 12, 34, 56, 500 };
        FILETIME ft;
        WORD d = 0, t = 0;
        SystemTimeToFileTime(&st, &ft);
        CALL(r, (DWORD)CoFileTimeToDosDateTime(&ft, &d, &t));
        snprintf(extra, sizeof extra, "ft=%08lx:%08lx date=0x%04x time=0x%04x", (unsigned long)ft.dwHighDateTime,
                 (unsigned long)ft.dwLowDateTime, d, t);
        line("dosdate", r, err, extra);
    }
    /* how the two-second step rounds: 56.000, 56.000 plus one 100 ns tick, 57.000, and 23:59:59.000 on the
       last day of a year */
    {
        static const struct { const char *name; SYSTEMTIME st; DWORD ticks; } c[] = {
            { "dosdate_even", { 2026, 11, 5, 13, 12, 34, 56, 0 }, 0 },
            { "dosdate_tick", { 2026, 11, 5, 13, 12, 34, 56, 0 }, 1 },
            { "dosdate_odd",  { 2026, 11, 5, 13, 12, 34, 57, 0 }, 0 },
            { "dosdate_carry", { 2026, 12, 4, 31, 23, 59, 59, 0 }, 0 },
        };
        for (unsigned i = 0; i < sizeof c / sizeof *c; i++) {
            FILETIME ft;
            WORD d = 0, t = 0;
            ULARGE_INTEGER u;
            SystemTimeToFileTime(&c[i].st, &ft);
            u.LowPart = ft.dwLowDateTime, u.HighPart = ft.dwHighDateTime;
            u.QuadPart += c[i].ticks;
            ft.dwLowDateTime = u.LowPart, ft.dwHighDateTime = u.HighPart;
            CALL(r, (DWORD)CoFileTimeToDosDateTime(&ft, &d, &t));
            snprintf(extra, sizeof extra, "ft=%08lx:%08lx date=0x%04x time=0x%04x", (unsigned long)ft.dwHighDateTime,
                     (unsigned long)ft.dwLowDateTime, d, t);
            line(c[i].name, r, err, extra);
        }
    }

    SetCurrentDirectoryA("\\");
    rm_tree(W, 0);
    return 0;
}
