/*
 * video_selftest.c - the Bink decoder core (plan 10 Task 5), driven directly, and
 * the binkw32 imports (Task 6), called as lifted code calls them on a headless
 * device. Built with the engine (minus main.c) by tests/test_video.py;
 * `video_selftest <group> [args]` runs one group and prints "<group>: <n> failed".
 * The movies come from the game tree (paths, and an offset and length into the
 * archive, as arguments); nothing decoded is written anywhere.
 */
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include "win32hle.h"
#include "bw2.h"
#include "bink_dec.h"
#include "d3d_com.h"
#include "d3d9_device.h"

const recomp_dispatch_entry_t recomp_dispatch_table[1] = { { 0, 0 } };
const uint32_t recomp_dispatch_count = 0;

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)
#define CHECKEQ(a, b) do { uint64_t _a = (uint64_t)(a), _b = (uint64_t)(b); if (_a != _b) { \
    printf("FAIL %s:%d %s == %s: %llu != %llu\n", __FILE__, __LINE__, #a, #b, \
           (unsigned long long)_a, (unsigned long long)_b); fails++; } } while (0)

static double now_s(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec + (double)t.tv_nsec * 1e-9;
}

/* A file read into memory that ends exactly at an inaccessible page, so any read past
 * the movie's last byte faults. */
struct held { uint8_t *data; uint32_t len; void *map; size_t map_len; };
static struct held load_guarded(const char *path) {
    struct held h = { 0 };
    int fd = open(path, O_RDONLY);
    struct stat st;
    if (fd < 0 || fstat(fd, &st) != 0) { fprintf(stderr, "cannot open %s\n", path); exit(2); }
    size_t page = (size_t)getpagesize(), body = ((size_t)st.st_size + page - 1) / page * page;
    h.map_len = body + page;
    h.map = mmap(NULL, h.map_len, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (h.map == MAP_FAILED) { perror("mmap"); exit(2); }
    h.len = (uint32_t)st.st_size;
    h.data = (uint8_t *)h.map + body - h.len;
    for (size_t got = 0; got < h.len;) {
        ssize_t r = read(fd, h.data + got, h.len - got);
        if (r <= 0) { fprintf(stderr, "cannot read %s\n", path); exit(2); }
        got += (size_t)r;
    }
    close(fd);
    mprotect((uint8_t *)h.map + body, page, PROT_NONE);
    mprotect(h.map, body, PROT_READ);
    return h;
}
/* A movie inside the archive, mapped read-only as the game's view is. */
static struct held map_part(const char *path, const char *off_s, const char *len_s) {
    struct held h = { 0 };
    uint64_t off = strtoull(off_s, NULL, 0);
    h.len = (uint32_t)strtoul(len_s, NULL, 0);
    int fd = open(path, O_RDONLY);
    if (fd < 0) { fprintf(stderr, "cannot open %s\n", path); exit(2); }
    uint64_t page = (uint64_t)getpagesize(), base = off / page * page;
    h.map_len = (size_t)(off - base) + h.len;
    h.map = mmap(NULL, h.map_len, PROT_READ, MAP_PRIVATE, fd, (off_t)base);
    if (h.map == MAP_FAILED) { perror("mmap"); exit(2); }
    close(fd);
    h.data = (uint8_t *)h.map + (off - base);
    return h;
}
static void release(struct held *h) { munmap(h->map, h->map_len); }

static struct bk_movie *open_ok(const struct held *h) {
    const char *err = "";
    struct bk_movie *m = bk_open(h->data, h->len, &err);
    if (!m) { printf("FAIL bk_open: %s\n", err); fails++; }
    return m;
}
/* The current frame, as a whole-movie 32-bit copy (malloc'd). */
static uint8_t *copy_of(struct bk_movie *m) {
    const struct bk_info *in = bk_info_of(m);
    uint8_t *buf = calloc((size_t)in->width * in->height, 4);
    CHECKEQ(bk_copy(m, buf, in->width * 4, in->height, 0, 0, 3), 0);
    return buf;
}
static int same(const struct bk_movie *m, const uint8_t *a, const uint8_t *b) {
    const struct bk_info *in = bk_info_of(m);
    return memcmp(a, b, (size_t)in->width * in->height * 4) == 0;
}

/* ---- bink_header: hand-made headers, no game files ---- */
static void header(uint8_t h[44], const char *magic, uint32_t size, uint32_t frames, uint32_t w, uint32_t hh,
                   uint32_t tracks) {
    uint32_t f[11] = { 0, size, frames, 1000, frames, w, hh, 30, 1, 0, tracks };
    memcpy(f, magic, 4);
    memcpy(h, f, 44);
}
static void expect_err(const uint8_t *h, uint32_t len, const char *want) {
    const char *err = NULL;
    struct bk_movie *m = bk_open(h, len, &err);
    CHECK(m == NULL);
    if (m) bk_close(m);
    if (!err || !strstr(err, want)) { printf("FAIL error \"%s\", wanted \"%s\"\n", err ? err : "(none)", want); fails++; }
}
static void group_bink_header(void) {
    uint8_t h[44];
    header(h, "XYZi", 36, 10, 64, 64, 0);
    expect_err(h, 44, "not a Bink movie");
    CHECKEQ(bk_peek_tracks(h, 44), (uint64_t)-1);
    header(h, "BIK\x01", 36, 10, 64, 64, 0);
    CHECKEQ(bk_peek_tracks(h, 44), (uint64_t)-1);
    expect_err(h, 44, "not a Bink movie");
    header(h, "BIKi", 36, 10, 64, 64, 0);
    CHECKEQ(bk_peek_tracks(h, 43), (uint64_t)-1);
    expect_err(h, 43, "truncated");
    header(h, "BIKi", 1000, 10, 64, 64, 0);
    expect_err(h, 44, "truncated");
    expect_err(h, 1007, "truncated");
    header(h, "BIKi", 36, 10, 64, 64, 1);
    CHECKEQ(bk_peek_tracks(h, 44), 1);
    header(h, "BIKi", 36, 10, 64, 64, 0);
    CHECKEQ(bk_peek_tracks(h, 44), 0);
    header(h, "BIKi", 36, 0, 64, 64, 0);
    expect_err(h, 44, "no frames");
    header(h, "BIKi", 36, 10, 0, 64, 0);
    expect_err(h, 44, "zero width or height");
    header(h, "BIKi", 36, 10, 64, 0, 0);
    expect_err(h, 44, "zero width or height");
}

/* ---- bink_ealogo: the loose movie, its goto rules ---- */
static void group_bink_ealogo(const char *path) {
    struct held h = load_guarded(path);
    CHECKEQ(bk_peek_tracks(h.data, h.len), 1);
    struct bk_movie *m = open_ok(&h);
    if (!m) return;
    const struct bk_info *in = bk_info_of(m);
    CHECKEQ(in->width, 720); CHECKEQ(in->height, 540); CHECKEQ(in->frames, 120);
    CHECKEQ(in->rate, 30); CHECKEQ(in->rate_div, 1); CHECKEQ(in->tracks, 1); CHECKEQ(in->bytes, 644220);
    CHECKEQ(bk_decoded(m), 0);
    CHECKEQ(bk_current(m), 1);
    bk_goto(m, 1);
    CHECK(bk_frame(m));
    CHECKEQ(bk_decoded(m), 1);
    bk_goto(m, 10);
    CHECKEQ(bk_current(m), 10);
    CHECK(bk_frame(m));
    CHECKEQ(bk_decoded(m), 10);
    uint8_t *first10 = copy_of(m);
    bk_goto(m, 60);
    CHECK(bk_frame(m));
    CHECKEQ(bk_decoded(m), 60);
    uint8_t *at60 = copy_of(m);
    CHECK(!same(m, first10, at60));
    bk_goto(m, 60);
    CHECK(bk_frame(m));
    CHECKEQ(bk_decoded(m), 60);
    bk_goto(m, 10);
    CHECK(bk_frame(m));
    CHECKEQ(bk_decoded(m), 70);
    CHECKEQ(bk_current(m), 10);
    uint8_t *again10 = copy_of(m);
    CHECK(same(m, first10, again10));
    free(first10), free(at60), free(again10);
    bk_close(m);
    release(&h);
}

/* ---- bink_last_frame: a movie held at its end ---- */
static void group_bink_last_frame(const char *path) {
    struct held h = load_guarded(path);
    struct bk_movie *m = open_ok(&h);
    if (!m) return;
    uint32_t last = bk_info_of(m)->frames;
    bk_goto(m, last);
    CHECK(bk_frame(m));
    uint32_t once = bk_decoded(m);
    CHECKEQ(once, last);
    uint8_t *first = copy_of(m);
    int differ = 0;
    double t0 = now_s();
    for (int i = 0; i < 1000; i++) {
        bk_goto(m, last);
        if (!bk_frame(m)) { fails++; printf("FAIL bk_frame at round %d\n", i); break; }
        uint8_t *c = copy_of(m);
        differ += !same(m, first, c);
        free(c);
    }
    CHECKEQ(differ, 0);
    CHECKEQ(bk_decoded(m), once);
    printf("bink_last_frame: 1000 rounds in %.2f s\n", now_s() - t0);
    free(first);
    bk_close(m);
    release(&h);
}

/* ---- bink_copy_clip: a smaller, offset destination ---- */
static void clip_case(struct bk_movie *m, const uint8_t *full, uint32_t x, uint32_t y) {
    const struct bk_info *in = bk_info_of(m);
    uint32_t w = in->width, pitch = w * 4 + 16, dst_h = in->height / 2, guard = 64;
    size_t body = (size_t)pitch * dst_h;
    uint8_t *buf = malloc(body + 2 * guard), *dst = buf + guard;
    memset(buf, 0xA5, body + 2 * guard);
    CHECKEQ(bk_copy(m, dst, pitch, dst_h, x, y, 3), 0);
    uint32_t cols = pitch / 4, c_end = x + w < cols ? x + w : cols, r_end = y + in->height < dst_h ? y + in->height : dst_h;
    int outside = 0, alpha = 0, wrong = 0, guards = 0;
    for (size_t i = 0; i < guard; i++) guards += buf[i] != 0xA5 || buf[guard + body + i] != 0xA5;
    for (uint32_t r = 0; r < dst_h; r++)
        for (uint32_t b = 0; b < pitch; b++) {
            uint32_t c = b / 4;
            int in_rect = r >= y && r < r_end && c >= x && c < c_end && b < cols * 4;
            uint8_t v = dst[(size_t)r * pitch + b];
            if (!in_rect) { outside += v != 0xA5; continue; }
            if (b % 4 == 3) alpha += v != 0xFF;
            wrong += v != full[((size_t)(r - y) * w + (c - x)) * 4 + b % 4];
        }
    CHECKEQ(guards, 0); CHECKEQ(outside, 0); CHECKEQ(alpha, 0); CHECKEQ(wrong, 0);
    free(buf);
}
static void group_bink_copy_clip(const char *path) {
    struct held h = load_guarded(path);
    struct bk_movie *m = open_ok(&h);
    if (!m) return;
    const struct bk_info *in = bk_info_of(m);
    uint8_t probe[64];
    CHECKEQ(bk_copy(m, probe, 16, 4, 0, 0, 3), (uint64_t)-2);  /* nothing decoded yet */
    bk_goto(m, 30);
    CHECK(bk_frame(m));
    uint8_t *full = copy_of(m);
    clip_case(m, full, 2, 3);
    clip_case(m, full, 2, 0);
    clip_case(m, full, 10, 7);                        /* the right edge clips at pitch / 4 */
    clip_case(m, full, in->width + 4, 0);             /* entirely right of the buffer */
    clip_case(m, full, 0, in->height);                /* entirely below it */
    /* type 5 without an alpha plane: the type-3 bytes */
    uint8_t *a = calloc((size_t)in->width * in->height, 4);
    CHECKEQ(bk_copy(m, a, in->width * 4, in->height, 0, 0, 5), 0);
    CHECK(memcmp(a, full, (size_t)in->width * in->height * 4) == 0);
    memset(probe, 0x5A, sizeof probe);
    CHECKEQ(bk_copy(m, probe, 16, 4, 0, 0, 7), (uint64_t)-1);
    int touched = 0;
    for (size_t i = 0; i < sizeof probe; i++) touched += probe[i] != 0x5A;
    CHECKEQ(touched, 0);
    free(a), free(full);
    bk_close(m);
    release(&h);
}

/* ---- bink_archive: movies inside the archive, every frame ---- */
static void group_bink_archive(char **a) {
    struct held h = map_part(a[0], a[1], a[2]);
    uint32_t want[6];
    for (int i = 0; i < 6; i++) want[i] = (uint32_t)strtoul(a[3 + i], NULL, 0);
    CHECKEQ(bk_peek_tracks(h.data, h.len), want[5]);
    struct bk_movie *m = open_ok(&h);
    if (!m) return;
    const struct bk_info *in = bk_info_of(m);
    CHECKEQ(in->width, want[0]); CHECKEQ(in->height, want[1]); CHECKEQ(in->frames, want[2]);
    CHECKEQ(in->rate, want[3]); CHECKEQ(in->rate_div, want[4]); CHECKEQ(in->tracks, want[5]);
    CHECKEQ(in->bytes, h.len);
    double t0 = now_s();
    uint32_t bad = 0;
    for (uint32_t f = 1; f <= in->frames; f++) {
        bk_goto(m, f);
        if (!bk_frame(m)) { if (!bad) printf("FAIL decoder error at frame %u\n", f); bad++; }
    }
    CHECKEQ(bad, 0);
    CHECKEQ(bk_decoded(m), in->frames);
    printf("bink_archive: %u frames in %.2f s\n", in->frames, now_s() - t0);
    bk_close(m);
    release(&h);
}

/* ---- bink_two_movies: two handles, each on its own state ---- */
static uint8_t *alone_at(const struct held *h, uint32_t frame) {
    struct bk_movie *m = open_ok(h);
    if (!m) exit(1);
    bk_goto(m, frame);
    CHECK(bk_frame(m));
    uint8_t *c = copy_of(m);
    bk_close(m);
    return c;
}
static void group_bink_two_movies(char **a) {
    struct held at = map_part(a[0], a[1], a[2]);
    struct held ea = load_guarded(a[3]);
    uint8_t *at30 = alone_at(&at, 30), *ea30 = alone_at(&ea, 30);
    struct bk_movie *m1 = open_ok(&at), *m2 = open_ok(&ea);
    if (!m1 || !m2) return;
    uint8_t *c1 = NULL, *c2 = NULL;
    for (uint32_t f = 1; f <= 30; f++) {
        bk_goto(m1, f);
        CHECK(bk_frame(m1));
        free(c1);
        c1 = copy_of(m1);
        bk_goto(m2, f < 15 ? f * 2 : f);              /* the second goes ahead, then back */
        CHECK(bk_frame(m2));
        free(c2);
        c2 = copy_of(m2);
    }
    CHECK(same(m1, at30, c1));
    CHECK(same(m2, ea30, c2));
    CHECKEQ(bk_decoded(m1), 30);
    bk_close(m1);
    bk_goto(m2, 30);                                  /* the other survives the close */
    CHECK(bk_frame(m2));
    free(c2);
    c2 = copy_of(m2);
    CHECK(same(m2, ea30, c2));
    bk_close(m2);
    free(c1), free(c2), free(at30), free(ea30);
    release(&at);
    release(&ea);
}

/* ==== Task 6: the binkw32 imports, called as lifted code calls them ==== */

/* binkw32.c's own: the decoder behind a handle (NULL if unknown), and binkw32 1.8's format mapping. */
struct bk_movie *bw2_bink_movie_of(uint32_t h);
uint32_t bw2_bink_surface_type(uint32_t d3dfmt);
int hle_vm_is_reserved(uint32_t va, uint32_t size);   /* guest_mem.c (based build) */

#define BINKFROMMEMORY 0x04000000u
#define BINKSURFACE32 3u
#define FMT_A8R8G8B8 21u
#define FMT_X8R8G8B8 22u
#define FMT_DXT1 0x31545844u
#define POOL_MANAGED 1u
#define POOL_SYSTEMMEM 2u
#define LOCK_READONLY 0x10u

/* An import by name, stdcall: arguments pushed right to left, a return address, the shim; the pop is checked. */
static uint32_t call(const char *name, int n, ...) {
    uint32_t va = hle_resolve(name);
    recomp_func_t f = va ? recomp_lookup_import(va) : NULL;
    if (!f) { printf("FAIL %s: not answered\n", name); fails++; return 0xDEADBEEFu; }
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
/* A COM method by interface and name, through the object's guest vtable. */
static uint32_t com(uint32_t obj, const char *iface, const char *method, uint32_t n, ...) {
    uint32_t a[16];
    va_list ap;
    va_start(ap, n);
    for (uint32_t i = 0; i < n; i++) a[i] = va_arg(ap, uint32_t);
    va_end(ap);
    return d3d_call(obj, iface, method, n, a);
}
static uint32_t gmem(uint32_t n) { return hle_va(hle_alloc(n)); }

static const char *g_stuff;
static uint64_t g_att_off, g_intro_off;
static uint32_t g_att_len, g_intro_len;
static const char *g_ealogo;

/* len bytes of the archive at off into dst (host memory). */
static void read_into(const char *path, uint64_t off, uint32_t len, uint8_t *dst) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) { fprintf(stderr, "cannot open %s\n", path); exit(2); }
    for (uint32_t got = 0; got < len;) {
        ssize_t r = pread(fd, dst + got, len - got, (off_t)(off + got));
        if (r <= 0) { fprintf(stderr, "cannot read %s\n", path); exit(2); }
        got += (uint32_t)r;
    }
    close(fd);
}
/* A movie copied into a guest heap block, as the game's view holds it: its guest address. */
static uint32_t guest_movie(uint64_t off, uint32_t len) {
    uint32_t va = gmem(len);
    if (!va) { fprintf(stderr, "no guest memory for %u bytes\n", len); exit(2); }
    read_into(g_stuff, off, len, hle_ptr(va));
    return va;
}
static uint32_t guest_attract(void) { return guest_movie(g_att_off, g_att_len); }
static uint32_t guest_file(const char *path, uint32_t *len) {
    struct stat st;
    if (stat(path, &st) != 0) { fprintf(stderr, "cannot stat %s\n", path); exit(2); }
    *len = (uint32_t)st.st_size;
    uint32_t va = gmem(*len);
    read_into(path, 0, *len, hle_ptr(va));
    return va;
}

/* A host decoder on the same bytes, at frame f: the reference copy (malloc'd, width*height*4). */
static uint8_t *ref_frame(uint32_t movie, uint32_t len, uint32_t f) {
    const char *err = "";
    struct bk_movie *m = bk_open(hle_ptr(movie), len, &err);
    if (!m) { printf("FAIL reference bk_open: %s\n", err); fails++; exit(1); }
    bk_goto(m, f);
    CHECK(bk_frame(m));
    uint8_t *c = copy_of(m);
    bk_close(m);
    return c;
}

/* ---- the headless device ---- */
static uint32_t g_dev;
static uint32_t device(void) {
    if (g_dev) return g_dev;
    uint32_t d3d9 = call("Direct3DCreate9", 1, 32u);
    uint32_t pp = gmem(56), out = gmem(4);
    MEM32(pp + 0) = 1024, MEM32(pp + 4) = 768, MEM32(pp + 8) = FMT_X8R8G8B8, MEM32(pp + 12) = 1;
    MEM32(pp + 24) = 1;                                     /* D3DSWAPEFFECT_DISCARD */
    MEM32(pp + 28) = 0x10001u;                              /* hDeviceWindow */
    MEM32(pp + 32) = 1;                                     /* Windowed */
    CHECKEQ(com(d3d9, "IDirect3D9", "CreateDevice", 6, 0u, 1u, 0x10001u, 0x40u, pp, out), D3D_OK);
    return g_dev = MEM32(out);
}
static void present(void) { com(device(), "IDirect3DDevice9", "Present", 4, 0u, 0u, 0u, 0u); }
/* Level 0 of a new managed texture: the surface (the texture is released; the surface holds it). */
static uint32_t tex_surface(uint32_t w, uint32_t h, uint32_t fmt) {
    uint32_t out = gmem(4);
    CHECKEQ(com(device(), "IDirect3DDevice9", "CreateTexture", 8, w, h, 1u, 0u, fmt, POOL_MANAGED, out, 0u), D3D_OK);
    uint32_t tex = MEM32(out);
    CHECKEQ(com(tex, "IDirect3DTexture9", "GetSurfaceLevel", 2, 0u, out), D3D_OK);
    uint32_t s = MEM32(out);
    com(tex, "IDirect3DTexture9", "Release", 0);
    return s;
}
/* LockRect: the bits' guest address, and *pitch. */
static uint32_t lock(uint32_t s, uint32_t flags, uint32_t *pitch) {
    uint32_t lr = gmem(8);
    CHECKEQ(com(s, "IDirect3DSurface9", "LockRect", 3, lr, 0u, flags), D3D_OK);
    *pitch = MEM32(lr);
    return MEM32(lr + 4);
}
static void unlock(uint32_t s) { CHECKEQ(com(s, "IDirect3DSurface9", "UnlockRect", 0), D3D_OK); }

/* The game's round (sub_0093C460): Goto, DoFrame, then the copy into the locked surface with the type as flags. */
static uint32_t game_round(uint32_t h, uint32_t frame, uint32_t s) {
    call("_BinkGoto@12", 3, h, frame, 0u);
    CHECKEQ(call("_BinkDoFrame@4", 1, h), 0);
    uint32_t type = call("_BinkDX9SurfaceType@4", 1, s), pitch;
    uint32_t bits = lock(s, 0, &pitch);
    uint32_t r = call("_BinkCopyToBuffer@28", 7, h, bits, pitch, MEM32(h + 4), 0u, 0u, type);
    unlock(s);
    return r;
}

/* ---- binkw32_open: the block's fields ---- */
static void group_binkw32_open(void) {
    uint32_t movie = guest_attract();
    uint32_t h = call("_BinkOpen@8", 2, movie, BINKFROMMEMORY);
    CHECK(h != 0);
    if (!h) return;
    CHECKEQ(MEM32(h + 0x00), 720); CHECKEQ(MEM32(h + 0x04), 576); CHECKEQ(MEM32(h + 0x08), 2033);
    CHECKEQ(MEM32(h + 0x0C), 1); CHECKEQ(MEM32(h + 0x10), 0); CHECKEQ(MEM32(h + 0x14), 25);
    CHECKEQ(MEM32(h + 0x18), 1); CHECKEQ(MEM32(h + 0x20), BINKFROMMEMORY);
    CHECKEQ(MEM32(h + 0x28), g_att_len - 8);
    int other = 0;                                          /* the rest of the 0x400 bytes: zero */
    for (uint32_t o = 0; o < 0x400; o += 4)
        if (o > 0x28 || o == 0x1C || o == 0x24) other += MEM32(h + o) != 0;
    CHECKEQ(other, 0);
    CHECK(bw2_bink_movie_of(h) != NULL);
    CHECKEQ(bk_decoded(bw2_bink_movie_of(h)), 0);
    call("_BinkClose@4", 1, h);
    CHECK(bw2_bink_movie_of(h) == NULL);
}

/* ---- binkw32_frame: one round into an X8R8G8B8 texture ---- */
static void group_binkw32_frame(void) {
    uint32_t movie = guest_attract();
    uint32_t h = call("_BinkOpen@8", 2, movie, BINKFROMMEMORY);
    uint32_t s = tex_surface(720, 576, FMT_X8R8G8B8);
    CHECKEQ(call("_BinkDX9SurfaceType@4", 1, s), BINKSURFACE32);
    CHECKEQ(game_round(h, 5, s), 0);
    CHECKEQ(MEM32(h + 0x0C), 5);
    CHECKEQ(MEM32(h + 0x10), 5);
    /* read back from the GPU: the staging copy is cleared, then replaced by the texture's own bytes */
    struct d3d_surface *hs = d3d_surf(s);
    memset(hle_ptr(d3d_surface_mem(hs)), 0, (size_t)hs->pitch * hs->h);
    d3d_surface_download(hs);
    uint32_t pitch, bits = lock(s, LOCK_READONLY, &pitch);
    uint8_t *want = ref_frame(movie, g_att_len, 5);
    int differ = 0;
    uint64_t lit = 0;
    for (uint32_t y = 0; y < 576; y++) {
        const uint8_t *row = (const uint8_t *)hle_ptr(bits) + (size_t)y * pitch;
        for (uint32_t x = 0; x < 720 * 4; x++) {
            if (x % 4 == 3) continue;                       /* X8: the alpha byte is not kept */
            differ += row[x] != want[(size_t)y * 720 * 4 + x];
            lit += row[x] != 0;
        }
    }
    unlock(s);
    CHECKEQ(differ, 0);
    CHECK(lit > 0);
    free(want);
    /* a format binkw32 does not map */
    uint32_t dxt = tex_surface(720, 576, FMT_DXT1);
    CHECKEQ(call("_BinkDX9SurfaceType@4", 1, dxt), 0xFFFFFFFFu);
    com(dxt, "IDirect3DSurface9", "Release", 0);
    com(s, "IDirect3DSurface9", "Release", 0);
    call("_BinkClose@4", 1, h);
}

/* ---- binkw32_goto_clamp: frame 0 and frames past the end, at the edge of guest memory ---- */
static void group_binkw32_goto_clamp(void) {
    uint32_t len = g_att_len, size = (len + 0xFFFFu) & ~0xFFFFu;
    /* a reservation whose next 64 KB stay unreserved: reserve one more unit, release, reserve again */
    uint32_t va = call("VirtualAlloc", 4, 0u, size + 0x10000u, 0x3000u, 4u);
    CHECK(va != 0);
    CHECK(call("VirtualFree", 3, va, 0u, 0x8000u) != 0);
    CHECKEQ(call("VirtualAlloc", 4, va, size, 0x3000u, 4u), va);
    CHECK(hle_vm_is_reserved(va, size));
    CHECK(!hle_vm_is_reserved(va + size, 1));                /* a read past the movie faults */
    uint32_t movie = va + size - len;
    read_into(g_stuff, g_att_off, len, hle_ptr(movie));
    uint32_t h = call("_BinkOpen@8", 2, movie, BINKFROMMEMORY);
    struct bk_movie *m = bw2_bink_movie_of(h);
    if (!m) { printf("FAIL no movie behind the handle\n"); fails++; return; }
    uint32_t s = tex_surface(720, 576, FMT_X8R8G8B8), pitch;
    uint8_t *first = ref_frame(movie, len, 1), *last = ref_frame(movie, len, 2033);
    uint8_t *got = malloc((size_t)720 * 576 * 4);
    #define GOT_EQUALS(ref) do { uint32_t _b = lock(s, LOCK_READONLY, &pitch); int _d = 0; \
        for (uint32_t y = 0; y < 576; y++) for (uint32_t x = 0; x < 720 * 4; x++) \
            if (x % 4 != 3) _d += ((const uint8_t *)hle_ptr(_b))[(size_t)y * pitch + x] != (ref)[(size_t)y * 2880 + x]; \
        unlock(s); CHECKEQ(_d, 0); } while (0)
    CHECKEQ(game_round(h, 0, s), 0);                         /* 0 becomes 1 */
    CHECKEQ(MEM32(h + 0x0C), 1);
    CHECKEQ(MEM32(h + 0x10), 1);
    GOT_EQUALS(first);
    CHECKEQ(bk_decoded(m), 1);
    CHECKEQ(game_round(h, 2034, s), 0);                      /* past the end becomes the last */
    CHECKEQ(MEM32(h + 0x0C), 2033);
    CHECKEQ(MEM32(h + 0x10), 2033);
    GOT_EQUALS(last);
    CHECKEQ(bk_decoded(m), 2033);
    CHECKEQ(game_round(h, 0x7FFFFFFFu, s), 0);
    CHECKEQ(MEM32(h + 0x0C), 2033);
    GOT_EQUALS(last);
    CHECKEQ(game_round(h, 2033, s), 0);                      /* nothing new to decode */
    CHECKEQ(bk_decoded(m), 2033);
    GOT_EQUALS(last);
    #undef GOT_EQUALS
    free(first), free(last), free(got);
    com(s, "IDirect3DSurface9", "Release", 0);
    call("_BinkClose@4", 1, h);
}

/* ---- binkw32_surface_type: binkw32 1.8's mapping ---- */
static uint32_t sysmem_surface(uint32_t fmt) {
    uint32_t out = gmem(4);
    CHECKEQ(com(device(), "IDirect3DDevice9", "CreateOffscreenPlainSurface", 6, 16u, 16u, fmt, POOL_SYSTEMMEM,
                out, 0u), D3D_OK);
    return MEM32(out);
}
static void group_binkw32_surface_type(void) {
    static const uint32_t fmts[] = { 21, 22, 23, 24, 25, 26, 30 }, want[] = { 5, 3, 10, 9, 8, 7, 7 };
    for (int i = 0; i < 7; i++) {
        uint32_t s = sysmem_surface(fmts[i]);
        uint32_t t = call("_BinkDX9SurfaceType@4", 1, s);
        if (t != want[i]) printf("FAIL format %u: type %u, want %u\n", fmts[i], t, want[i]), fails++;
        com(s, "IDirect3DSurface9", "Release", 0);
    }
    /* R8G8B8 (20), YUY2 and UYVY: our device makes no such surface, so the mapping itself */
    CHECKEQ(bw2_bink_surface_type(20), 1);
    CHECKEQ(bw2_bink_surface_type(0x32595559u), 13);         /* 'YUY2' */
    CHECKEQ(bw2_bink_surface_type(0x59565955u), 14);         /* 'UYVY' */
    for (int i = 0; i < 7; i++) CHECKEQ(bw2_bink_surface_type(fmts[i]), want[i]);
    CHECKEQ(bw2_bink_surface_type(0), 0xFFFFFFFFu);
    CHECKEQ(bw2_bink_surface_type(27), 0xFFFFFFFFu);
    CHECKEQ(call("_BinkDX9SurfaceType@4", 1, 0u), 0xFFFFFFFFu);
    uint32_t dxt = tex_surface(64, 64, FMT_DXT1);
    CHECKEQ(call("_BinkDX9SurfaceType@4", 1, dxt), 0xFFFFFFFFu);
    com(dxt, "IDirect3DSurface9", "Release", 0);
}

/* ---- binkw32_reopen: close, then open again from a new copy ---- */
static void group_binkw32_reopen(void) {
    uint32_t movie = guest_attract();
    uint32_t s = tex_surface(720, 576, FMT_X8R8G8B8);
    uint32_t h = call("_BinkOpen@8", 2, movie, BINKFROMMEMORY);
    CHECKEQ(game_round(h, 10, s), 0);
    CHECKEQ(MEM32(h + 0x0C), 10);
    call("_BinkClose@4", 1, h);
    CHECK(bw2_bink_movie_of(h) == NULL);
    hle_free(hle_ptr(movie));
    uint32_t again = guest_attract();
    uint32_t h2 = call("_BinkOpen@8", 2, again, BINKFROMMEMORY);
    CHECK(h2 != 0);
    struct bk_movie *m = bw2_bink_movie_of(h2);
    if (!m) { printf("FAIL no movie behind the second handle\n"); fails++; return; }
    CHECKEQ(MEM32(h2 + 0x0C), 1);
    CHECKEQ(MEM32(h2 + 0x10), 0);
    CHECKEQ(bk_decoded(m), 0);
    CHECKEQ(game_round(h2, 1, s), 0);
    CHECKEQ(bk_decoded(m), 1);
    uint8_t *want = ref_frame(again, g_att_len, 1);
    uint32_t pitch, bits = lock(s, LOCK_READONLY, &pitch);
    int differ = 0;
    for (uint32_t y = 0; y < 576; y++)
        for (uint32_t x = 0; x < 2880; x++)
            if (x % 4 != 3) differ += ((const uint8_t *)hle_ptr(bits))[(size_t)y * pitch + x] != want[(size_t)y * 2880 + x];
    unlock(s);
    CHECKEQ(differ, 0);
    free(want);
    call("_BinkClose@4", 1, h2);
    com(s, "IDirect3DSurface9", "Release", 0);
}

/* ---- binkw32_loud <case>: each stops (the test reads the stop line) ---- */
static void group_binkw32_loud(const char *c) {
    uint32_t never = gmem(0x400);                            /* a block no open handed out */
    if (!strcmp(c, "audio_track")) {
        uint32_t len, ea = guest_file(g_ealogo, &len);
        call("_BinkOpen@8", 2, ea, BINKFROMMEMORY);
        return;
    }
    if (!strcmp(c, "not_bink")) {
        uint32_t junk = gmem(4096);
        memset(hle_ptr(junk), 'Q', 4096);
        call("_BinkOpen@8", 2, junk, BINKFROMMEMORY);
        return;
    }
    if (!strcmp(c, "null_movie")) { call("_BinkOpen@8", 2, 0u, BINKFROMMEMORY); return; }
    if (!strcmp(c, "unknown_goto")) { call("_BinkGoto@12", 3, never, 1u, 0u); return; }
    if (!strcmp(c, "unknown_doframe")) { call("_BinkDoFrame@4", 1, never); return; }
    if (!strcmp(c, "unknown_close")) { call("_BinkClose@4", 1, never); return; }
    if (!strcmp(c, "unknown_copy")) {
        uint32_t buf = gmem(64);
        call("_BinkCopyToBuffer@28", 7, never, buf, 16u, 4u, 0u, 0u, BINKSURFACE32);
        return;
    }
    uint32_t movie = guest_attract();
    if (!strcmp(c, "open_flags")) { call("_BinkOpen@8", 2, movie, 0x00800000u); return; }
    uint32_t h = call("_BinkOpen@8", 2, movie, BINKFROMMEMORY);
    if (!strcmp(c, "goto_flags")) { call("_BinkGoto@12", 3, h, 1u, 1u); return; }
    uint32_t s = tex_surface(720, 576, FMT_X8R8G8B8), pitch, bits = lock(s, 0, &pitch);
    if (!strcmp(c, "copy_before_frame")) {
        call("_BinkCopyToBuffer@28", 7, h, bits, pitch, 576u, 0u, 0u, BINKSURFACE32);
        return;
    }
    call("_BinkGoto@12", 3, h, 1u, 0u);
    call("_BinkDoFrame@4", 1, h);
    if (!strcmp(c, "copy_flags")) {
        call("_BinkCopyToBuffer@28", 7, h, bits, pitch, 576u, 0u, 0u, 0x80000003u);
        return;
    }
    if (!strcmp(c, "copy_unmapped")) {                       /* the game passes the type as the flags */
        unlock(s);
        uint32_t dxt = tex_surface(720, 576, FMT_DXT1);
        uint32_t type = call("_BinkDX9SurfaceType@4", 1, dxt), dbits = lock(dxt, 0, &pitch);
        call("_BinkCopyToBuffer@28", 7, h, dbits, pitch, 576u, 0u, 0u, type);
        return;
    }
    printf("binkw32_loud: no case %s\n", c);
    fails++;
}

/* ---- binkw32_quit: BW2_QUIT_AFTER_MOVIE (set by the test); stdout marks how far the run got ---- */
static void mark(const char *what) { printf("mark %s\n", what); fflush(stdout); }
static void group_binkw32_quit(void) {
    uint32_t intro = guest_movie(g_intro_off, g_intro_len);
    uint32_t hi = call("_BinkOpen@8", 2, intro, BINKFROMMEMORY);
    uint32_t si = tex_surface(640, 360, FMT_X8R8G8B8);
    for (uint32_t f = 1; f <= 5; f++) CHECKEQ(game_round(hi, f, si), 0);
    present();
    mark("intro present 1");
    present();
    mark("intro present 2");
    uint32_t att = guest_attract();
    uint32_t ha = call("_BinkOpen@8", 2, att, BINKFROMMEMORY);
    uint32_t sa = tex_surface(720, 576, FMT_X8R8G8B8);
    CHECKEQ(game_round(ha, 1, sa), 0);
    mark("attract 1 copied");
    CHECKEQ(game_round(ha, 2, sa), 0);
    present();
    mark("attract 2 present");
    CHECKEQ(game_round(ha, 3, sa), 0);
    present();
    mark("attract 3 present 1");
    present();
    mark("attract 3 present 2");
    for (int i = 0; i < 5; i++) present();
    printf("FAIL the run did not quit\n");
    fails++;
}

static int binkw32_main(int argc, char **argv, const char *g) {
    if (argc < 6) { fprintf(stderr, "usage: video_selftest %s STUFF ATT_OFF ATT_LEN INTRO_OFF INTRO_LEN EALOGO\n", g); return 2; }
    g_stuff = argv[0];
    g_att_off = strtoull(argv[1], NULL, 0), g_att_len = (uint32_t)strtoul(argv[2], NULL, 0);
    g_intro_off = strtoull(argv[3], NULL, 0), g_intro_len = (uint32_t)strtoul(argv[4], NULL, 0);
    g_ealogo = argv[5];
    hle_mem_init();
    bw2_register_shims();
    mach_enter();
    g_esp = hle_va(hle_alloc(1u << 20)) + (1u << 20) - 64;
    if (!strcmp(g, "binkw32_open")) group_binkw32_open();
    else if (!strcmp(g, "binkw32_frame")) group_binkw32_frame();
    else if (!strcmp(g, "binkw32_goto_clamp")) group_binkw32_goto_clamp();
    else if (!strcmp(g, "binkw32_surface_type")) group_binkw32_surface_type();
    else if (!strcmp(g, "binkw32_reopen")) group_binkw32_reopen();
    else if (!strcmp(g, "binkw32_loud") && argc > 6) group_binkw32_loud(argv[6]);
    else if (!strcmp(g, "binkw32_quit")) group_binkw32_quit();
    else { fprintf(stderr, "video_selftest: no group %s\n", g); return 2; }
    printf("%s: %d failed\n", g, fails);
    fflush(stdout);
    return fails != 0;
}

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: video_selftest <group> [args]\n"); return 2; }
    const char *g = argv[1];
    if (!strncmp(g, "binkw32_", 8)) return binkw32_main(argc - 2, argv + 2, g);
    if (!strcmp(g, "bink_header")) group_bink_header();
    else if (!strcmp(g, "bink_ealogo") && argc > 2) group_bink_ealogo(argv[2]);
    else if (!strcmp(g, "bink_last_frame") && argc > 2) group_bink_last_frame(argv[2]);
    else if (!strcmp(g, "bink_copy_clip") && argc > 2) group_bink_copy_clip(argv[2]);
    else if (!strcmp(g, "bink_archive") && argc > 10) group_bink_archive(argv + 2);
    else if (!strcmp(g, "bink_two_movies") && argc > 5) group_bink_two_movies(argv + 2);
    else { fprintf(stderr, "video_selftest: no group %s\n", g); return 2; }
    printf("%s: %d failed\n", g, fails);
    fflush(stdout);
    return fails != 0;
}
