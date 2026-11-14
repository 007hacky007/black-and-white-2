/*
 * script.c - BW2_INPUT_SCRIPT (plan 6 Task 6), a test and measurement switch
 * like BW2_QUIT_AFTER_READS. It pushes scripted events into the same host
 * queue the Mac window feeds (bw2_in_push), keyed to the DirectInput read
 * counter, so the drain, the messages and DirectInput run as for a person.
 *
 * The file is read once, at the first read. One event per line; '#' starts a
 * comment; blank lines are skipped; numbers are C syntax (strtol base 0):
 *   <read> move <dx> <dy>          a MOVE with DirectInput delta (dx, dy)
 *   <read> down <button>           BUTTON 0-4 down, at the running point
 *   <read> up <button>             BUTTON 0-4 up, at the running point
 *   <read> wheel <delta>           WHEEL, a nonzero multiple of 120
 *   <read> key <vk> <scan> <down|up> [ext]   KEY
 *   <read> close                   CLOSE
 *   <read> quit                    exit 0 at that read, before answering it (plan 11)
 * The running point (client coordinates) starts at (0,0), where the game's
 * own cursor starts; each move adds its delta, clamped to the game window's
 * client rect while there is one. Read numbers start at 1 and never
 * decrease. A bad line, or a file that cannot be opened, stops the run.
 *
 * Blocks (plan 11 Task 1). A line `after <anchor>` starts a block; the lines
 * after it, up to the next `after`, are its events, with read numbers from 0,
 * non-decreasing, counted from the block's anchor read A:
 *   after open <frames>     a movie of that many frames opened (_BinkOpen@8)
 *   after last <frames>     a movie of that many frames showed its last frame
 *   after seconds <s>       s wall seconds (0 < s <= 3600, strtod)
 *   after file <name>       the game asked for a file of that name (plan 12 Task 1)
 * The first block is armed at the first read at which every line before it
 * was pushed; each later block at the first read at which the block before
 * it fired and all its events were pushed. A movie anchor counts only events
 * recorded after the arming read's call (A: the read counter at the event); a
 * seconds anchor fires at the first read whose clock is s past the clock at
 * the arming read (A: that read). Event k of a block is pushed at the first
 * read at or after the firing whose number is at least A + k. Each firing
 * prints "[bw2] script: after <anchor words> at read <A>".
 *
 * File anchors (plan 12 Task 1). guard.c's CreateFileA and CreateFileW (the
 * shims the game's imports reach) pass every name to bw2_script_file before
 * forwarding the call untouched. <name> is one token of 1 to 259 characters
 * with no '\' or '/', matched ASCII case-insensitively against the guest
 * path's last component. A lookup is recorded (with the read counter's value)
 * only once the script is loaded and only when its name is that of a file
 * block not yet fired. A file block's window opens at the anchor read of the
 * block before it (block 0: the first read), not at its own arming read, so a
 * lookup that the previous block's own events cause counts; the block fires at
 * the first read, once armed, at which a recorded lookup in its window has its
 * name (A: that lookup's read). The log holds at most 256 lookups (one per
 * name and read); a firing drops those before A and those no unfired block
 * names, and a log that would grow past 256 stops the run.
 */
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include "bw2.h"
#include "d3d_com.h"
#include "input.h"

struct script_ev {
    unsigned long long read;             /* absolute (block 0), or from the block's anchor read */
    size_t block;
    int quit;                            /* the quit verb; e is unused */
    struct bw2_in_event e;               /* a MOVE's and a BUTTON's or WHEEL's point is set when pushed */
};

enum anchor { A_NONE, A_OPEN, A_LAST, A_SECONDS, A_FILE };
#define FILE_NAME_MAX 259                /* a file anchor's longest name (MAX_PATH less its NUL) */
struct block {
    enum anchor kind;                    /* block 0, the lines before the first `after`: A_NONE */
    uint32_t frames;
    double seconds;
    char words[272];                     /* "after ...", single-spaced, as written ("after file " + 259) */
    char name[FILE_NAME_MAX + 1];        /* A_FILE: the name, as written */
    size_t first;                        /* its first event's index (g_n if it has none) */
    int armed, fired;
    size_t arm_movie;                    /* movie events before this index were recorded before arming */
    double arm_clock;
    unsigned long long a;                /* the anchor read */
};

static struct script_ev *g_ev;
static size_t g_n, g_cap, g_next;
static struct block *g_blk;
static size_t g_nblk, g_blkcap, g_cur;   /* g_cur: the first block not yet fired */
static int g_loaded;
static int g_px, g_py;                   /* the running client point */

/* The movie events binkw32.c reports, in order. */
struct movie_ev { int last; uint32_t frames; unsigned long long read; };
static struct movie_ev *g_mv;
static size_t g_nmv, g_mvcap;

/* The file lookups bw2_script_file records (plan 12 Task 1): only names an unfired file block has, at most
 * FILE_LOG_MAX. Guarded by g_file_mu (any guest thread may open a file); g_file_ready is set, under it, once
 * the script is loaded. */
#define FILE_LOG_MAX 256
struct file_ev { char name[FILE_NAME_MAX + 1]; unsigned long long read; };
static struct file_ev g_fev[FILE_LOG_MAX];
static size_t g_nfev;
static int g_file_ready;
static unsigned long long g_files_seen;
static char g_file_last[FILE_NAME_MAX + 1];
static pthread_mutex_t g_file_mu = PTHREAD_MUTEX_INITIALIZER;

static double mono_now(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + ts.tv_nsec / 1e9;
}
static double (*g_now)(void) = mono_now;

#define MAX_FIELDS 8

__attribute__((format(printf, 2, 3)))
static _Noreturn void bad(int line, const char *fmt, const char *a) {
    char what[256];
    snprintf(what, sizeof what, fmt, a);
    d3d_fail("BW2_INPUT_SCRIPT line %d: %s", line, what);
}

/* A whole token as a C number (strtol base 0) in [lo, hi]. */
static long number(int line, const char *tok, long lo, long hi, const char *what) {
    char *end = NULL;
    errno = 0;
    long v = strtol(tok, &end, 0);
    if (!*tok || !end || *end || errno) bad(line, "'%s' is not a number", tok);
    if (v < lo || v > hi) {
        char s[160];
        snprintf(s, sizeof s, "%s %s is out of range (%ld to %ld)", what, tok, lo, hi);
        bad(line, "%s", s);
    }
    return v;
}

static void fields(int line, const char *verb, int have, int lo, int hi) {
    if (have < lo || have > hi) {
        char s[96];
        if (lo == hi) snprintf(s, sizeof s, "%s takes %d field%s, not %d", verb, lo, lo == 1 ? "" : "s", have);
        else snprintf(s, sizeof s, "%s takes %d to %d fields, not %d", verb, lo, hi, have);
        bad(line, "%s", s);
    }
}

static void add(const struct script_ev *ev) {
    if (g_n == g_cap) {
        g_cap = g_cap ? g_cap * 2 : 64;
        g_ev = realloc(g_ev, g_cap * sizeof *g_ev);
        if (!g_ev) abort();
    }
    g_ev[g_n++] = *ev;
}

static struct block *add_block(void) {
    if (g_nblk == g_blkcap) {
        g_blkcap = g_blkcap ? g_blkcap * 2 : 16;
        g_blk = realloc(g_blk, g_blkcap * sizeof *g_blk);
        if (!g_blk) abort();
    }
    struct block *b = &g_blk[g_nblk++];
    memset(b, 0, sizeof *b);
    b->first = g_n;
    return b;
}

/* `after <kind> <value>`: a new block; the read numbers after it start again from 0. */
static void parse_after(int line, char **f, int nf, unsigned long long *last) {
    if (nf != 2) {
        char s[96];
        snprintf(s, sizeof s, "after%s%s takes 2 fields (a kind and a value), not %d", nf ? " " : "", nf ? f[0] : "",
                 nf);
        bad(line, "%s", s);
    }
    struct block nb = { 0 };
    if (!strcmp(f[0], "open") || !strcmp(f[0], "last")) {
        nb.kind = f[0][0] == 'o' ? A_OPEN : A_LAST;
        nb.frames = (uint32_t)number(line, f[1], 1, 0xFFFFFFFFL, "frames");
    } else if (!strcmp(f[0], "seconds")) {
        char *end = NULL;
        errno = 0;
        double v = strtod(f[1], &end);
        if (!*f[1] || !end || *end || errno) bad(line, "'%s' is not a number", f[1]);
        if (!(v > 0 && v <= 3600)) bad(line, "after seconds %s is not more than 0 and at most 3600", f[1]);
        nb.kind = A_SECONDS, nb.seconds = v;
    } else if (!strcmp(f[0], "file")) {
        size_t len = strlen(f[1]);
        if (len > FILE_NAME_MAX) {
            char s[96];
            snprintf(s, sizeof s, "after file name of %zu characters is longer than %d", len, FILE_NAME_MAX);
            bad(line, "%s", s);
        }
        if (strpbrk(f[1], "\\/")) bad(line, "after file name '%s' is a path (no \\ or / allowed)", f[1]);
        nb.kind = A_FILE;
        memcpy(nb.name, f[1], len + 1);
    } else
        bad(line, "unknown anchor 'after %s' (open, last, seconds or file)", f[0]);
    if (strlen(f[0]) + strlen(f[1]) + 8 > sizeof nb.words) bad(line, "anchor value '%s' is too long", f[1]);
    snprintf(nb.words, sizeof nb.words, "after %s %s", f[0], f[1]);
    struct block *b = add_block();
    nb.first = b->first;
    *b = nb;
    *last = 0;
}

static void parse_line(int line, char *s, unsigned long long *last) {
    char *hash = strchr(s, '#');
    if (hash) *hash = 0;
    char *tok[MAX_FIELDS + 3];
    int nt = 0;
    char *save = NULL;
    for (char *t = strtok_r(s, " \t\r\n\v\f", &save); t; t = strtok_r(NULL, " \t\r\n\v\f", &save))
        if (nt < MAX_FIELDS + 3) tok[nt++] = t;
        else bad(line, "%s", "too many fields");
    if (!nt) return;                     /* blank or a comment */
    if (!strcmp(tok[0], "after")) {
        parse_after(line, tok + 1, nt - 1, last);
        return;
    }
    static const char *const verbs[] = { "move", "down", "up", "wheel", "key", "close", "quit" };
    for (size_t i = 0; i < sizeof verbs / sizeof *verbs; i++)
        if (!strcmp(tok[0], verbs[i])) bad(line, "'%s' has no read number", tok[0]);
    if (nt < 2) bad(line, "'%s' has no verb", tok[0]);
    struct script_ev ev = { 0 };
    ev.block = g_nblk - 1;
    ev.read = (unsigned long long)number(line, tok[0], g_nblk > 1 ? 0 : 1, 0x7FFFFFFFL, "read");
    if (ev.read < *last) {
        char s2[96];
        snprintf(s2, sizeof s2, "read %llu is before read %llu", ev.read, *last);
        bad(line, "%s", s2);
    }
    const char *verb = tok[1];
    int nf = nt - 2;
    char **f = tok + 2;
    if (!strcmp(verb, "move")) {
        fields(line, verb, nf, 2, 2);
        ev.e.kind = BW2_IN_MOVE;
        ev.e.dx = (int)number(line, f[0], -0x40000000L, 0x40000000L, "dx");
        ev.e.dy = (int)number(line, f[1], -0x40000000L, 0x40000000L, "dy");
    } else if (!strcmp(verb, "down") || !strcmp(verb, "up")) {
        fields(line, verb, nf, 1, 1);
        char *end = NULL;
        errno = 0;
        long b = strtol(f[0], &end, 0);
        if (!*f[0] || *end || errno) bad(line, "'%s' is not a number", f[0]);
        if (b < 0 || b > 4) bad(line, "button %s is not 0-4", f[0]);
        ev.e.kind = BW2_IN_BUTTON, ev.e.button = (int)b, ev.e.down = verb[0] == 'd';
    } else if (!strcmp(verb, "wheel")) {
        fields(line, verb, nf, 1, 1);
        char *end = NULL;
        errno = 0;
        long w = strtol(f[0], &end, 0);
        if (!*f[0] || *end || errno) bad(line, "'%s' is not a number", f[0]);
        if (!w || w % 120 || w > 0x40000000L || w < -0x40000000L)
            bad(line, "wheel %s is not a nonzero multiple of 120", f[0]);
        ev.e.kind = BW2_IN_WHEEL, ev.e.wheel = (int)w;
    } else if (!strcmp(verb, "key")) {
        fields(line, verb, nf, 3, 4);
        ev.e.kind = BW2_IN_KEY;
        ev.e.vk = (uint32_t)number(line, f[0], 1, 0xFE, "vk");
        ev.e.scan = (uint32_t)number(line, f[1], 0, 0xFF, "scan");
        if (!strcmp(f[2], "down")) ev.e.down = 1;
        else if (strcmp(f[2], "up")) bad(line, "key direction '%s' is not down or up", f[2]);
        if (nf == 4) {
            if (strcmp(f[3], "ext")) bad(line, "key flag '%s' is not ext", f[3]);
            ev.e.extended = 1;
        }
    } else if (!strcmp(verb, "close")) {
        fields(line, verb, nf, 0, 0);
        ev.e.kind = BW2_IN_CLOSE;
    } else if (!strcmp(verb, "quit")) {
        fields(line, verb, nf, 0, 0);
        ev.quit = 1;
    } else
        bad(line, "unknown verb '%s'", verb);
    *last = ev.read;
    add(&ev);
}

static void load(void) {
    g_loaded = 1;
    add_block();                         /* block 0: the absolute lines, fired from the start */
    g_blk[0].armed = g_blk[0].fired = 1;
    g_cur = 1;
    const char *path = getenv("BW2_INPUT_SCRIPT");
    if (!path) return;
    FILE *fp = fopen(path, "r");
    if (!fp) d3d_fail("BW2_INPUT_SCRIPT '%s' cannot be opened: %s", path, strerror(errno));
    char *buf = NULL;
    size_t cap = 0;
    int line = 0;
    unsigned long long last = 0;
    while (getline(&buf, &cap, fp) >= 0) parse_line(++line, buf, &last);
    if (ferror(fp)) d3d_fail("BW2_INPUT_SCRIPT '%s' cannot be read: %s", path, strerror(errno));
    free(buf);
    fclose(fp);
    pthread_mutex_lock(&g_file_mu);
    g_file_ready = 1;                    /* lookups are recorded from here on */
    pthread_mutex_unlock(&g_file_mu);
}

/* The running point moved by a delta, clamped to the game window's client rect (win32hle windows have
 * no frame, so it is the window rect's size); with no window, not clamped. */
static void move_point(int dx, int dy) {
    long long x = (long long)g_px + dx, y = (long long)g_py + dy;
    int l, t, r, b;
    uint32_t h = bw2_in_window();
    if (h && hle_window_screen_rect(h, &l, &t, &r, &b)) {
        long long w = r - l, hh = b - t;
        if (x > w - 1) x = w - 1;
        if (y > hh - 1) y = hh - 1;
        if (x < 0) x = 0;
        if (y < 0) y = 0;
    } else {
        if (x > 0x7FFFFFFF) x = 0x7FFFFFFF;
        if (x < -0x7FFFFFFF) x = -0x7FFFFFFF;
        if (y > 0x7FFFFFFF) y = 0x7FFFFFFF;
        if (y < -0x7FFFFFFF) y = -0x7FFFFFFF;
    }
    g_px = (int)x, g_py = (int)y;
}

void bw2_script_movie(int last, uint32_t frames) {
    if (g_nmv == g_mvcap) {
        g_mvcap = g_mvcap ? g_mvcap * 2 : 16;
        g_mv = realloc(g_mv, g_mvcap * sizeof *g_mv);
        if (!g_mv) abort();
    }
    g_mv[g_nmv++] = (struct movie_ev){ last != 0, frames, bw2_dinput_reads() };
}

void bw2_script_set_clock(double (*now)(void)) { g_now = now ? now : mono_now; }

/* 1 if an unfired file block (g_cur on; every block before g_cur fired) has this name. g_file_mu held. */
static int unfired_file_name(const char *name) {
    for (size_t i = g_cur; i < g_nblk; i++)
        if (g_blk[i].kind == A_FILE && !g_blk[i].fired && !strcasecmp(g_blk[i].name, name)) return 1;
    return 0;
}

void bw2_script_file(const char *guest_path) {
    if (!guest_path) return;
    const char *c = guest_path;
    for (const char *p = guest_path; *p; p++)
        if (*p == '\\' || *p == '/') c = p + 1;
    size_t len = strlen(c);
    pthread_mutex_lock(&g_file_mu);
    g_files_seen++;
    snprintf(g_file_last, sizeof g_file_last, "%s", c);
    if (g_file_ready && len && len <= FILE_NAME_MAX && unfired_file_name(c)) {
        unsigned long long n = bw2_dinput_reads();
        int dup = 0;
        for (size_t i = 0; i < g_nfev && !dup; i++) dup = g_fev[i].read == n && !strcasecmp(g_fev[i].name, c);
        if (!dup) {
            if (g_nfev == FILE_LOG_MAX)
                d3d_fail("BW2_INPUT_SCRIPT: more than %d file lookups wait for a file anchor at read %llu "
                         "(the last '%s')", FILE_LOG_MAX, n, c);
            memcpy(g_fev[g_nfev].name, c, len + 1);
            g_fev[g_nfev++].read = n;
        }
    }
    pthread_mutex_unlock(&g_file_mu);
}

unsigned long long bw2_script_files_seen(char *last, size_t n) {
    pthread_mutex_lock(&g_file_mu);
    unsigned long long seen = g_files_seen;
    if (last && n) snprintf(last, n, "%s", g_file_last);
    pthread_mutex_unlock(&g_file_mu);
    return seen;
}

/* File block b (armed, block g_cur): the read of its first recorded lookup in its window (at or after the
 * anchor read of the block before it; block 0 counts as fired at the first read, before which nothing is
 * recorded). 1 if there is one. */
static int file_found(const struct block *b, unsigned long long *a) {
    unsigned long long from = g_cur > 1 ? g_blk[g_cur - 1].a : 0;
    int found = 0;
    pthread_mutex_lock(&g_file_mu);
    for (size_t k = 0; k < g_nfev && !found; k++)
        if (g_fev[k].read >= from && !strcasecmp(g_fev[k].name, b->name)) *a = g_fev[k].read, found = 1;
    pthread_mutex_unlock(&g_file_mu);
    return found;
}

/* Block g_cur fires with anchor read a: the file log keeps only lookups a later window can still use (at or
 * after a, with a name an unfired block has). Under g_file_mu, since bw2_script_file reads g_cur and the
 * fired flags from any thread. */
static void fire(struct block *b, unsigned long long a) {
    pthread_mutex_lock(&g_file_mu);
    b->a = a;
    b->fired = 1;
    g_cur++;
    size_t o = 0;
    for (size_t j = 0; j < g_nfev; j++)
        if (g_fev[j].read >= a && unfired_file_name(g_fev[j].name)) g_fev[o++] = g_fev[j];
    g_nfev = o;
    pthread_mutex_unlock(&g_file_mu);
    fprintf(stderr, "[bw2] script: %s at read %llu\n", b->words, a);
    fflush(stderr);
}

/* Block g_cur: armed once the block before it fired and its events are all pushed; then fired when its
 * anchor is seen. 1 if it fired at this read. */
static int try_block(unsigned long long n) {
    struct block *b = &g_blk[g_cur];
    if (!b->armed) {
        if (g_next < b->first) return 0;     /* the block before it still has events to push */
        b->armed = 1, b->arm_movie = g_nmv;
        if (b->kind == A_SECONDS) b->arm_clock = g_now();
    }
    unsigned long long a;
    if (b->kind == A_FILE) {
        if (!file_found(b, &a)) return 0;
    } else if (b->kind == A_SECONDS) {
        if (g_now() - b->arm_clock < b->seconds) return 0;
        a = n;
    } else {
        size_t i = b->arm_movie;
        int want_last = b->kind == A_LAST;
        while (i < g_nmv && !(g_mv[i].last == want_last && g_mv[i].frames == b->frames)) i++;
        if (i == g_nmv) return 0;
        a = g_mv[i].read;
    }
    fire(b, a);
    return 1;
}

void bw2_script_at_read(unsigned long long n) {
    if (!g_loaded) load();
    for (;;) {
        while (g_next < g_n) {
            const struct script_ev *ev = &g_ev[g_next];
            const struct block *b = &g_blk[ev->block];
            if (!b->fired || b->a + ev->read > n) break;
            g_next++;
            if (ev->quit) {
                fprintf(stderr, "[bw2] script: quit at read %llu\n", n);
                fflush(stderr);
                hle_exit(0);
            }
            struct bw2_in_event e = ev->e;
            if (e.kind == BW2_IN_MOVE) move_point(e.dx, e.dy);
            e.x = g_px, e.y = g_py;
            bw2_in_push(&e);
        }
        if (g_cur >= g_nblk || !try_block(n)) return;
    }
}
