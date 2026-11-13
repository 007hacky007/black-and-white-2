/*
 * video_selftest.c - the Bink decoder core (plan 10 Task 5), driven directly.
 * Built with the engine (minus main.c) by tests/test_video.py;
 * `video_selftest <group> [args]` runs one group and prints "<group>: <n> failed".
 * The movies come from the game tree (paths, and an offset and length into the
 * archive, as arguments); nothing decoded is written anywhere.
 */
#include <fcntl.h>
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

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: video_selftest <group> [args]\n"); return 2; }
    const char *g = argv[1];
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
