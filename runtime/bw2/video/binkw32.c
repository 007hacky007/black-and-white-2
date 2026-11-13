/*
 * binkw32.c - the six binkw32.dll imports white.exe calls (plan 10 Task 6),
 * answered with the decoder core in bink_dec.c.
 *
 * The game opens a movie from memory (BINKFROMMEMORY, a pointer into its view
 * of Data/Everything.stuff), then once per rendered frame (sub_0093C460) calls
 * Goto, DoFrame, DX9SurfaceType and CopyToBuffer into the texture it has
 * locked. The handle is a zeroed 0x400-byte guest block holding Bink 1.x's
 * leading fields, the ones the game reads (Width, Height, Frames, FrameRate,
 * FrameRateDiv; sub_0093C7B0) and a few more; the rest is padding. Our decoder
 * state lives in a host table keyed by the block's address (Ruling 8), never
 * in guest memory. Every argument or flag no run has shown stops loudly.
 *
 * Test switches (read once): BW2_BINK_LOG=1 prints numbers about each movie;
 * BW2_QUIT_AFTER_MOVIE=<frames>:<frame>[+<presents>] ends a run with exit 0 at
 * a frame of the movie with that many frames (Ruling 15).
 * A movie's open and the copy that shows its last frame are told to the
 * input script (bw2_script_movie, plan 11 Task 1), whatever the log switch.
 */
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "win32hle.h"
#include "bw2.h"
#include "d3d_com.h"
#include "d3d9_device.h"
#include "bink_dec.h"
#include "input.h"                       /* bw2_script_movie: the script's movie anchors (plan 11) */

int hle_vm_is_reserved(uint32_t va, uint32_t size);   /* guest_mem.c (based build) */

#define BINKFROMMEMORY 0x04000000u
#define BLOCK_SIZE     0x400u
/* the block's fields (Bink 1.x's BINK struct, leading part) */
#define F_WIDTH      0x00u
#define F_HEIGHT     0x04u
#define F_FRAMES     0x08u
#define F_FRAMENUM   0x0Cu
#define F_LASTFRAME  0x10u
#define F_RATE       0x14u
#define F_RATEDIV    0x18u
#define F_OPENFLAGS  0x20u
#define F_SIZE       0x28u

_Noreturn static void bink_fail(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void bink_fail(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "[bw2] FATAL: bink: ");
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fprintf(stderr, " (from guest 0x%08X, sub_%08X)\n", MEM32(g_esp), g_cur_func);
    fflush(stderr);
    abort();
}

/* ---- the test switches ---- */
static int log_on(void) {
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("BW2_BINK_LOG");
        on = e && !strcmp(e, "1");
    }
    return on;
}

static struct {
    int parsed, on;
    uint32_t frames, frame, presents;
    int armed;
    uint32_t left;                       /* Presents still to come once armed */
} g_quit;

/* A decimal number of 1 to 10 digits at *p, at most 0xFFFFFFFF; advances *p. 0 if there is none. */
static int number(const char **p, uint32_t *out) {
    const char *s = *p;
    if (*s < '0' || *s > '9') return 0;
    char *end = NULL;
    errno = 0;
    unsigned long long v = strtoull(s, &end, 10);
    if (errno || v > 0xFFFFFFFFull || end - s > 10) return 0;
    *out = (uint32_t)v, *p = end;
    return 1;
}
/* Read once, at the first Bink call or Present; a malformed value stops there, naming it. */
static void quit_conf(void) {
    if (g_quit.parsed) return;
    g_quit.parsed = 1;
    const char *v = getenv("BW2_QUIT_AFTER_MOVIE");
    if (!v) return;
    const char *p = v;
    uint32_t presents = 0;
    int ok = number(&p, &g_quit.frames) && *p++ == ':' && number(&p, &g_quit.frame);
    if (ok && *p == '+') p++, ok = number(&p, &presents);
    if (!ok || *p || !g_quit.frames || !g_quit.frame)
        bink_fail("BW2_QUIT_AFTER_MOVIE='%s' is not <frames>:<frame>[+<presents>] (frames and frame from 1)", v);
    g_quit.presents = presents;
    g_quit.on = 1;
}
_Noreturn static void quit_now(void) {
    fprintf(stderr, "[bw2] quit at movie frame %u:%u + %u presents (BW2_QUIT_AFTER_MOVIE)\n", g_quit.frames,
            g_quit.frame, g_quit.presents);
    fflush(stderr);
    hle_exit(0);
    abort();
}

/* From present_bb, after the frame log: counts the Presents after the switch armed. */
void bw2_movie_present(void) {
    quit_conf();
    if (g_quit.armed && --g_quit.left == 0) quit_now();
}

/* ---- the handles ---- */
struct movie {
    uint32_t h;                          /* the guest block */
    struct bk_movie *m;
    uint32_t frames;
    uint32_t shown;                      /* the frame DoFrame decoded last (0: none) */
    uint32_t largest;                    /* the largest frame Goto was asked, before the clamp */
    int last_said;
};
static struct movie *g_movies;
static size_t g_nmovies, g_cap;

static struct movie *find(uint32_t h) {
    for (size_t i = 0; i < g_nmovies; i++)
        if (g_movies[i].h == h) return &g_movies[i];
    return NULL;
}
static struct movie *movie_of(uint32_t h, const char *fn) {
    struct movie *e = h ? find(h) : NULL;
    if (!e) bink_fail("%s: unknown handle 0x%08X (no _BinkOpen@8 handed it out, or it was closed)", fn, h);
    return e;
}
/* For the selftest: the decoder behind a handle, NULL if unknown. */
struct bk_movie *bw2_bink_movie_of(uint32_t h) {
    struct movie *e = find(h);
    return e ? e->m : NULL;
}

/* _BinkOpen@8(source, flags): BINKFROMMEMORY only (Ruling 4). */
static void b_BinkOpen(void) {
    quit_conf();
    uint32_t p = A32(0), flags = A32(1);
    if (flags != BINKFROMMEMORY)
        bink_fail("_BinkOpen@8 flags 0x%08X (only BINKFROMMEMORY 0x%08X is measured), source 0x%08X", flags,
                  BINKFROMMEMORY, p);
    if (!p || !hle_vm_is_reserved(p, 44))
        bink_fail("_BinkOpen@8: the movie at 0x%08X is not in guest memory", p);
    const uint8_t *data = hle_ptr(p);
    int tracks = bk_peek_tracks(data, 44);
    if (tracks < 0) bink_fail("_BinkOpen@8: cannot open the movie at 0x%08X: not a Bink movie", p);
    if (tracks > 0) bw2_later_load("Bink audio track");     /* Ruling 1: exits 4 */
    uint32_t size = MEM32(p + 4);
    uint64_t bytes = (uint64_t)size + 8;
    if (bytes > 0xFFFFFFFFull || !hle_vm_is_reserved(p, (uint32_t)bytes))
        bink_fail("_BinkOpen@8: the movie at 0x%08X declares %llu bytes, not all in guest memory", p,
                  (unsigned long long)bytes);
    const char *err = "?";
    struct bk_movie *m = bk_open(data, (uint32_t)bytes, &err);
    if (!m) bink_fail("_BinkOpen@8: cannot open the movie at 0x%08X: %s", p, err);
    const struct bk_info *in = bk_info_of(m);
    void *blk = hle_alloc(BLOCK_SIZE);
    if (!blk) bink_fail("_BinkOpen@8: no guest memory for the handle");
    uint32_t h = hle_va(blk);
    MEM32(h + F_WIDTH) = in->width, MEM32(h + F_HEIGHT) = in->height, MEM32(h + F_FRAMES) = in->frames;
    MEM32(h + F_FRAMENUM) = 1, MEM32(h + F_LASTFRAME) = 0;
    MEM32(h + F_RATE) = in->rate, MEM32(h + F_RATEDIV) = in->rate_div;
    MEM32(h + F_OPENFLAGS) = flags, MEM32(h + F_SIZE) = size;
    if (g_nmovies == g_cap) {
        size_t cap = g_cap ? g_cap * 2 : 4;
        struct movie *n = realloc(g_movies, cap * sizeof *n);
        if (!n) bink_fail("_BinkOpen@8: out of host memory");
        g_movies = n, g_cap = cap;
    }
    g_movies[g_nmovies++] = (struct movie){ .h = h, .m = m, .frames = in->frames };
    if (log_on())
        fprintf(stderr, "[bw2] bink open: %ux%u, %u frames at %u/%u fps, %u audio tracks, %u bytes from memory\n",
                in->width, in->height, in->frames, in->rate, in->rate_div, in->tracks, in->bytes);
    bw2_script_movie(0, in->frames);
    RET(h, 2);
}

/* _BinkGoto@12(h, frame, flags): binkw32 1.8's own clamp (0x18011A60, Ruling 5): 0 becomes 1, past the
 * end becomes the last frame (unsigned compare). Returns nothing. */
static void b_BinkGoto(void) {
    quit_conf();
    struct movie *e = movie_of(A32(0), "_BinkGoto@12");
    uint32_t f = A32(1), flags = A32(2);
    if (flags) bink_fail("_BinkGoto@12 flags 0x%08X (only 0 is measured), frame %u, handle 0x%08X", flags, f, e->h);
    if (f > e->largest) e->largest = f;
    if (f == 0) f = 1;
    if (f > e->frames) f = e->frames;
    bk_goto(e->m, f);
    MEM32(e->h + F_FRAMENUM) = f;
    RETV(3);
}

/* _BinkDoFrame@4(h): decodes the current frame; 0. */
static void b_BinkDoFrame(void) {
    quit_conf();
    struct movie *e = movie_of(A32(0), "_BinkDoFrame@4");
    uint32_t f = bk_current(e->m);
    if (!bk_frame(e->m)) bink_fail("_BinkDoFrame@4: decoder error at frame %u of %u, handle 0x%08X", f, e->frames, e->h);
    e->shown = f;
    MEM32(e->h + F_LASTFRAME) = f;
    RET(0, 1);
}

/* _BinkCopyToBuffer@28(h, dst, pitch, height, x, y, flags): BINKSURFACE32 (3) or 32A (5) only; 0. */
static void b_BinkCopyToBuffer(void) {
    quit_conf();
    struct movie *e = movie_of(A32(0), "_BinkCopyToBuffer@28");
    uint32_t dst = A32(1), pitch = A32(2), height = A32(3), x = A32(4), y = A32(5), flags = A32(6);
    if (flags != 3 && flags != 5)
        bink_fail("_BinkCopyToBuffer@28 flags 0x%08X (only 3 BINKSURFACE32 and 5 BINKSURFACE32A are measured), "
                  "handle 0x%08X", flags, e->h);
    if (!dst) bink_fail("_BinkCopyToBuffer@28: no destination (handle 0x%08X)", e->h);
    uint64_t span = (uint64_t)pitch * height;
    if (span && (span > 0xFFFFFFFFull || !hle_vm_is_reserved(dst, (uint32_t)span)))
        bink_fail("_BinkCopyToBuffer@28: the destination 0x%08X (pitch %u, %u rows) is not all guest memory",
                  dst, pitch, height);
    int r = bk_copy(e->m, hle_ptr(dst), pitch, height, x, y, flags);
    if (r == -2) bink_fail("_BinkCopyToBuffer@28 before any _BinkDoFrame@4 decoded a frame, handle 0x%08X", e->h);
    if (r != 0) bink_fail("_BinkCopyToBuffer@28: copy type %u not done (%d), handle 0x%08X", flags, r, e->h);
    if (e->shown == e->frames && !e->last_said) {
        e->last_said = 1;
        if (log_on()) fprintf(stderr, "[bw2] bink last frame: %u\n", e->frames);
        bw2_script_movie(1, e->frames);
    }
    if (g_quit.on && !g_quit.armed && e->frames == g_quit.frames && e->shown >= g_quit.frame) {
        g_quit.armed = 1, g_quit.left = g_quit.presents;
        if (!g_quit.presents) quit_now();
    }
    RET(0, 7);
}

/* binkw32 1.8's _BinkDX9SurfaceType@4 (0x1800EB50) mapping from a D3DFORMAT; 0xFFFFFFFF for any other. */
uint32_t bw2_bink_surface_type(uint32_t fmt) {
    switch (fmt) {
    case 20: return 1;                   /* R8G8B8: BINKSURFACE24R */
    case 21: return 5;                   /* A8R8G8B8: BINKSURFACE32A */
    case 22: return 3;                   /* X8R8G8B8: BINKSURFACE32 */
    case 23: return 10;                  /* R5G6B5: BINKSURFACE565 */
    case 24: return 9;                   /* X1R5G5B5: BINKSURFACE555 */
    case 25: return 8;                   /* A1R5G5B5: BINKSURFACE5551 */
    case 26: return 7;                   /* A4R4G4B4: BINKSURFACE4444 */
    case 30: return 7;                   /* X4R4G4B4 */
    case 0x32595559u: return 13;         /* 'YUY2' */
    case 0x59565955u: return 14;         /* 'UYVY' */
    default: return 0xFFFFFFFFu;
    }
}
/* _BinkDX9SurfaceType@4(surface): the surface's format mapped; NULL gives 0xFFFFFFFF, as binkw32 does (Ruling 7). */
static void b_BinkDX9SurfaceType(void) {
    quit_conf();
    uint32_t s = A32(0);
    RET(s ? bw2_bink_surface_type(d3d_surf(s)->fmt) : 0xFFFFFFFFu, 1);
}

/* _BinkClose@4(h): the decoder closed, the block freed, the handle forgotten. */
static void b_BinkClose(void) {
    quit_conf();
    struct movie *e = movie_of(A32(0), "_BinkClose@4");
    if (log_on())
        fprintf(stderr, "[bw2] bink close: %u frames decoded, largest goto %u\n", bk_decoded(e->m), e->largest);
    bk_close(e->m);
    hle_free(hle_ptr(e->h));
    *e = g_movies[--g_nmovies];
    RETV(1);
}

static const win32hle_shim g_bink_shims[] = {
    { "_BinkOpen@8", b_BinkOpen },
    { "_BinkGoto@12", b_BinkGoto },
    { "_BinkDoFrame@4", b_BinkDoFrame },
    { "_BinkCopyToBuffer@28", b_BinkCopyToBuffer },
    { "_BinkDX9SurfaceType@4", b_BinkDX9SurfaceType },
    { "_BinkClose@4", b_BinkClose },
    { NULL, NULL } };

void bw2_video_register(void) { win32hle_register(g_bink_shims); }
