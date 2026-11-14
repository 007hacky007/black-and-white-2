/*
 * app.c - the engine as a macOS app's executable (plan 13 Task 4).
 *
 * In `<X>.app/Contents/MacOS/<name>` the engine finds its game (Resources/game) and
 * the lifted d3dx9_25.dll (Resources; codesign refuses a DLL in MacOS) in the
 * bundle, and writes everything it prints to <support>/Logs/bw2.log. Opened from
 * Finder there is no terminal, and a loud stop has about 90 exit sites, so a
 * watcher (this same binary, run as `bw2 --log-watcher LOG`, started before
 * anything can stop the run) reads the engine's output through a pipe, copies it
 * to the log, and when the pipe ends looks at the last engine line:
 *
 *   The decisive line is the last line, or the part of a line from a run-in
 *   "[bw2] " or "[win32hle] " on (game debug text without a newline runs into the
 *   next engine line), that starts with "[bw2]", "[win32hle]", "bw2:" or
 *   "TRIPWIRE:". The end is clean only when it is "[win32hle] ExitProcess(0)" or
 *   starts with "[bw2] quit " (BW2_QUIT_AFTER_READS, BW2_QUIT_AFTER_MOVIE) or
 *   "[bw2] script: quit " (the input script's quit). No decisive line is not clean.
 *
 * A loud end shows an alert naming that line and the log (BW2_NO_ALERT=1 prints
 * "[bw2] alert: <line>" to stdout instead). BW2_LOG=stderr keeps the output on
 * stderr. The watcher never touches AppKit and ignores SIGINT, SIGHUP and SIGPIPE,
 * so it outlives a Ctrl-C or a closed terminal and still reports the end; the
 * engine ignores SIGPIPE in a bundle, so a killed watcher costs only log lines.
 */
#include <CoreFoundation/CoreFoundation.h>
#include <CoreServices/CoreServices.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <mach-o/dyld.h>
#include <signal.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

/* The real path of this executable, or 0. */
static int exe_path(char *out, size_t n) {
    char exe[PATH_MAX], real[PATH_MAX];
    uint32_t len = sizeof exe;
    if (_NSGetExecutablePath(exe, &len) != 0 || !realpath(exe, real)) return 0;
    return (size_t)snprintf(out, n, "%s", real) < n;
}

static int ends_with(const char *s, size_t len, const char *tail) {
    size_t t = strlen(tail);
    return len >= t && !memcmp(s + len - t, tail, t);
}

int bw2_app_contents(char *out, size_t n) {
    char p[PATH_MAX];
    if (!exe_path(p, sizeof p)) return 0;
    char *slash = strrchr(p, '/');                        /* .../X.app/Contents/MacOS/<name> */
    if (!slash || slash == p) return 0;
    *slash = 0;
    size_t len = strlen(p);
    if (!ends_with(p, len, "/Contents/MacOS")) return 0;
    p[len -= sizeof "/MacOS" - 1] = 0;                    /* .../X.app/Contents */
    size_t app = len - (sizeof "/Contents" - 1);          /* .../X.app */
    if (!ends_with(p, app, ".app") || app < sizeof "/x.app" - 1 || p[app - 5] == '/') return 0;
    return (size_t)snprintf(out, n, "%s", p) < n;
}

int bw2_app_resource(const char *name, char *out, size_t n) {
    char base[PATH_MAX];
    if (bw2_app_contents(base, sizeof base)) {
        size_t l = strlen(base);
        if (l + sizeof "/Resources" > sizeof base) return -1;
        strcat(base, "/Resources");
    } else {
        if (!exe_path(base, sizeof base)) return -1;
        char *slash = strrchr(base, '/');
        if (slash) *slash = 0;
    }
    return (size_t)snprintf(out, n, "%s/%s", base, name) < n ? 0 : -1;
}

/* ---- the engine's side: the pipe and the watcher --------------------------- */

static int mkdirs(const char *path) {
    char p[PATH_MAX];
    if ((size_t)snprintf(p, sizeof p, "%s", path) >= sizeof p) return -1;
    for (char *s = p + 1; *s; s++)
        if (*s == '/') {
            *s = 0;
            if (mkdir(p, 0755) != 0 && errno != EEXIST) return -1;
            *s = '/';
        }
    return mkdir(p, 0755) != 0 && errno != EEXIST ? -1 : 0;
}

static int log_fail(const char *what, int err) {
    fprintf(stderr, "bw2: cannot start the log: %s: %s\n", what, strerror(err));
    return -1;
}

int bw2_app_log_start(const char *support) {
    char contents[PATH_MAX];
    if (!bw2_app_contents(contents, sizeof contents)) return 0;
    const char *mode = getenv("BW2_LOG");
    if (mode && !strcmp(mode, "stderr")) return 0;
    for (int fd = 0; fd <= 2; fd++)                       /* a closed 0-2 must not be taken by the pipe */
        if (fcntl(fd, F_GETFD) == -1 && open("/dev/null", fd ? O_WRONLY : O_RDONLY) != fd)
            return log_fail("/dev/null", errno);
    char logs[PATH_MAX], log[PATH_MAX], prev[PATH_MAX], self[PATH_MAX];
    if ((size_t)snprintf(logs, sizeof logs, "%s/Logs", support) >= sizeof logs ||
        (size_t)snprintf(log, sizeof log, "%s/bw2.log", logs) >= sizeof log ||
        (size_t)snprintf(prev, sizeof prev, "%s/bw2.previous.log", logs) >= sizeof prev)
        return log_fail(support, ENAMETOOLONG);
    if (mkdirs(logs) != 0) return log_fail(logs, errno);
    if (rename(log, prev) != 0 && errno != ENOENT) return log_fail(prev, errno);
    if (!exe_path(self, sizeof self)) return log_fail("the engine's own path", errno ? errno : ENOENT);
    int fds[2];
    if (pipe(fds) != 0) return log_fail("pipe", errno);
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_adddup2(&fa, fds[0], 0);     /* stdin: the read end */
    posix_spawn_file_actions_addclose(&fa, fds[0]);
    posix_spawn_file_actions_addclose(&fa, fds[1]);       /* or the watcher never sees end of input */
    char *argv[] = { self, "--log-watcher", log, NULL };
    pid_t pid;
    int err = posix_spawn(&pid, self, &fa, NULL, argv, environ);
    posix_spawn_file_actions_destroy(&fa);
    if (err) {
        close(fds[0]), close(fds[1]);
        return log_fail("the watcher", err);
    }
    close(fds[0]);
    const char *kill_test = getenv("BW2_TEST_KILL_WATCHER");   /* test switch: the watcher dies at once */
    if (kill_test && !strcmp(kill_test, "1")) {
        kill(pid, SIGKILL);
        waitpid(pid, NULL, 0);
    }
    signal(SIGPIPE, SIG_IGN);                             /* a dead watcher must not kill the game */
    fflush(stdout);
    fflush(stderr);
    if (dup2(fds[1], 1) < 0 || dup2(fds[1], 2) < 0) return log_fail("dup2", errno);
    close(fds[1]);
    setvbuf(stdout, NULL, _IOLBF, 0);
    return 0;
}

/* ---- the watcher's side ---------------------------------------------------- */

static const char *decisive_part(const char *line) {
    static const char *const starts[] = { "[bw2]", "[win32hle]", "bw2:", "TRIPWIRE:" };
    static const char *const run_in[] = { "[bw2] ", "[win32hle] " };
    const char *last = NULL;
    for (size_t i = 0; i < 4; i++)
        if (!strncmp(line, starts[i], strlen(starts[i]))) last = line;
    for (size_t i = 0; i < 2; i++)
        for (const char *p = *line ? strstr(line + 1, run_in[i]) : NULL; p; p = strstr(p + 1, run_in[i]))
            if (!last || p > last) last = p;
    return last;
}

static int clean_end(const char *d) {
    return d && (!strcmp(d, "[win32hle] ExitProcess(0)") || !strncmp(d, "[bw2] quit ", 11) ||
                 !strncmp(d, "[bw2] script: quit ", 19));
}

/* ~ for $HOME at the start of path. */
static void shown_path(const char *path, char *out, size_t n) {
    const char *home = getenv("HOME");
    size_t h = home ? strlen(home) : 0;
    if (h > 1 && !strncmp(path, home, h) && path[h] == '/') snprintf(out, n, "~%s", path + h);
    else snprintf(out, n, "%s", path);
}

static void show_alert(const char *decisive, const char *log_path) {
    char shown[PATH_MAX], msg[8192];
    shown_path(log_path, shown, sizeof shown);
    snprintf(msg, sizeof msg, "%s\n\nThe log is in %s.", decisive ? decisive : "It stopped without a message.", shown);
    CFStringRef text = CFStringCreateWithCString(NULL, msg, kCFStringEncodingUTF8);
    if (!text) text = CFStringCreateWithCString(NULL, msg, kCFStringEncodingISOLatin1);
    CFOptionFlags response = 0;
    CFUserNotificationDisplayAlert(0, kCFUserNotificationStopAlertLevel, NULL, NULL, NULL,
                                   CFSTR("Black & White 2 stopped"), text, CFSTR("OK"), CFSTR("Show Log"), NULL,
                                   &response);
    if (text) CFRelease(text);
    if (response == kCFUserNotificationAlternateResponse) {
        char dir[PATH_MAX];
        snprintf(dir, sizeof dir, "%s", log_path);
        char *slash = strrchr(dir, '/');
        if (slash) *slash = 0;
        CFURLRef url = CFURLCreateFromFileSystemRepresentation(NULL, (const UInt8 *)dir, (CFIndex)strlen(dir), true);
        if (url) {
            LSOpenCFURLRef(url, NULL);
            CFRelease(url);
        }
    }
}

int bw2_app_log_watch(const char *log_path) {
    signal(SIGINT, SIG_IGN);
    signal(SIGHUP, SIG_IGN);
    signal(SIGPIPE, SIG_IGN);
    FILE *log = fopen(log_path, "wb");
    if (!log) fprintf(stderr, "bw2: cannot write the log %s: %s\n", log_path, strerror(errno));
    char *line = NULL, *decisive = NULL;
    size_t cap = 0;
    ssize_t len;
    while ((len = getline(&line, &cap, stdin)) > 0) {
        if (log) {
            fwrite(line, 1, (size_t)len, log);
            fflush(log);
        }
        while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) line[--len] = 0;
        const char *d = decisive_part(line);
        if (d) {
            free(decisive);
            decisive = strdup(d);
        }
    }
    if (log) fclose(log);
    if (!clean_end(decisive)) {
        const char *quiet = getenv("BW2_NO_ALERT");
        if (quiet && !strcmp(quiet, "1")) {
            printf("[bw2] alert: %s\n", decisive ? decisive : "no message");
            fflush(stdout);
        } else {
            show_alert(decisive, log_path);
        }
    }
    free(line);
    free(decisive);
    return 0;
}
