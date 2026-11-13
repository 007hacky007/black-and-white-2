/*
 * frame_dump.m - BW2_FRAME_DUMP=<dir>[:<every>] (plan 10 Task 7, Ruling 14; spec
 * 7.3's frame capture). Read once, at the first Present: every <every>-th
 * Present (100 if not given) writes the back buffer as <dir>/frame-<n>.png, RGB
 * through ImageIO, and prints "[bw2] frame dump: <n>"; n counts every Present
 * from the first. The frames are game material, so a <dir> inside a git work
 * tree (it or a folder above it holds .git) stops the first Present, as do a
 * <dir> that cannot be made and a malformed value. Nothing is made before the
 * checks pass.
 */
#import <CoreGraphics/CoreGraphics.h>
#import <Foundation/Foundation.h>
#import <ImageIO/ImageIO.h>
#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "d3d9_device.h"

static int g_read, g_on;
static uint32_t g_every;
static uint64_t g_presents;
static char g_dir[PATH_MAX];                 /* the folder, absolute, symbolic links above it resolved */

static int exists(const char *p) { struct stat st; return lstat(p, &st) == 0; }

/* The value split at its last ':' into the folder and the count (decimal, 1 to 0xFFFFFFFF). */
static void parse(const char *v, char *dir) {
    const char *colon = strrchr(v, ':');
    size_t dlen = colon ? (size_t)(colon - v) : strlen(v);
    g_every = 100;
    if (colon) {
        const char *n = colon + 1;
        size_t k = strlen(n);
        unsigned long long e = k >= 1 && k <= 10 && strspn(n, "0123456789") == k ? strtoull(n, NULL, 10) : 0;
        if (e < 1 || e > UINT32_MAX)
            d3d_fail("frame dump: BW2_FRAME_DUMP=%s: \"%s\" is not a Present count from 1 to 4294967295", v, n);
        g_every = (uint32_t)e;
    }
    if (!dlen) d3d_fail("frame dump: BW2_FRAME_DUMP=%s names no folder", v);
    char cwd[PATH_MAX] = "";
    if (v[0] != '/' && !getcwd(cwd, sizeof cwd)) d3d_fail("frame dump: no current folder for BW2_FRAME_DUMP=%s", v);
    int len = snprintf(dir, PATH_MAX, "%s%s%.*s", cwd, cwd[0] ? "/" : "", (int)dlen, v);
    if (len < 0 || len >= PATH_MAX) d3d_fail("frame dump: BW2_FRAME_DUMP=%s: the folder name is too long", v);
}

/* dir with "." and ".." resolved by name and repeated or trailing slashes dropped, in place. */
static void normalise(char *dir) {
    char out[PATH_MAX] = "";
    size_t n = 0;
    for (char *save = NULL, *c = strtok_r(dir, "/", &save); c; c = strtok_r(NULL, "/", &save)) {
        if (!strcmp(c, ".")) continue;
        if (!strcmp(c, "..")) {
            while (n > 0 && out[n - 1] != '/') n--;
            if (n > 0) n--;
            out[n] = 0;
            continue;
        }
        n += (size_t)snprintf(out + n, sizeof out - n, "/%s", c);
    }
    strcpy(dir, n ? out : "/");
}

/* The folder's longest existing part with its links resolved, then the rest; refuse a git work tree; make it. */
static void prepare(const char *v) {
    char given[PATH_MAX], dir[PATH_MAX], probe[PATH_MAX];
    parse(v, given);
    strcpy(dir, given);
    normalise(dir);
    size_t keep = strlen(dir);                  /* dir[0..keep) exists */
    while (keep > 1) {
        char c = dir[keep];
        dir[keep] = 0;
        int ok = exists(dir);
        dir[keep] = c;
        if (ok) break;
        while (keep > 1 && dir[keep - 1] != '/') keep--;
        if (keep > 1) keep--;
    }
    char head[PATH_MAX], real[PATH_MAX];
    memcpy(head, dir, keep), head[keep] = 0;
    if (!realpath(keep ? head : "/", real)) d3d_fail("frame dump: cannot make %s: %s", given, strerror(errno));
    const char *tail = dir + keep + (dir[keep] == '/');  /* the part that does not exist yet, if any */
    int len = snprintf(g_dir, sizeof g_dir, "%s%s%s", real, *tail && strcmp(real, "/") ? "/" : "", tail);
    if (len < 0 || len >= (int)sizeof g_dir) d3d_fail("frame dump: BW2_FRAME_DUMP=%s: the folder name is too long", v);
    /* every existing folder from the deepest up to the root: none may hold .git (a folder, or a worktree's file) */
    for (size_t n = strlen(real);;) {
        snprintf(probe, sizeof probe, "%.*s/.git", (int)n, real);
        if (exists(probe))
            d3d_fail("frame dump: %s is inside a git work tree (%.*s holds .git); frames are game material",
                     given, n ? (int)n : 1, n ? real : "/");
        if (n == 0) break;
        while (n > 0 && real[n - 1] != '/') n--;
        if (n > 0) n--;
    }
    for (char *p = g_dir + 1;; p++) {             /* mkdir -p, one level at a time */
        if (*p && *p != '/') continue;
        char c = *p;
        *p = 0;
        if (mkdir(g_dir, 0755) != 0 && errno != EEXIST) d3d_fail("frame dump: cannot make %s: %s", given, strerror(errno));
        *p = c;
        if (!c) break;
    }
    struct stat st;
    if (stat(g_dir, &st) != 0 || !S_ISDIR(st.st_mode)) d3d_fail("frame dump: cannot make %s: not a folder", given);
    if (access(g_dir, W_OK | X_OK) != 0) d3d_fail("frame dump: cannot write into %s: %s", given, strerror(errno));
}

static void write_png(const char *path, const uint8_t *rgb, uint32_t w, uint32_t h) {
    CGColorSpaceRef cs = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
    CGDataProviderRef dp = CGDataProviderCreateWithData(NULL, rgb, (size_t)w * 3 * h, NULL);
    CGImageRef img = CGImageCreate(w, h, 8, 24, (size_t)w * 3, cs, kCGBitmapByteOrderDefault | kCGImageAlphaNone, dp,
                                   NULL, false, kCGRenderingIntentDefault);
    CFURLRef url = CFURLCreateFromFileSystemRepresentation(NULL, (const UInt8 *)path, (CFIndex)strlen(path), false);
    CGImageDestinationRef dst = url && img ? CGImageDestinationCreateWithURL(url, CFSTR("public.png"), 1, NULL) : NULL;
    int ok = dst != NULL;
    if (dst) {
        CGImageDestinationAddImage(dst, img, NULL);
        ok = CGImageDestinationFinalize(dst);
        CFRelease(dst);
    }
    if (url) CFRelease(url);
    if (img) CGImageRelease(img);
    CGDataProviderRelease(dp);
    CGColorSpaceRelease(cs);
    if (!ok) d3d_fail("frame dump: cannot write %s", path);
}

/* Called by every Present after the frame is sent (d3d9_device.c present_bb). */
void bw2_frame_dump(struct d3d_device *d, struct d3d_surface *bb) {
    if (!g_read) {
        g_read = 1;
        const char *v = getenv("BW2_FRAME_DUMP");
        if (v) prepare(v), g_on = 1;
    }
    if (!g_on || ++g_presents % g_every) return;
    if (bb->f->store != ST_BGRA8) d3d_fail("frame dump: back buffer format %s is not read back", bb->f->name);
    uint32_t w = bb->w, h = bb->h;
    uint8_t *px = malloc((size_t)w * 4 * h), *rgb = malloc((size_t)w * 3 * h);
    if (!px || !rgb) d3d_fail("frame dump: no memory for a %ux%u frame", w, h);
    mtl_tex_download(d->mtl, bb->tex, bb->slice, bb->level, 0, 0, w, h, px, w * 4);
    for (size_t i = 0, end = (size_t)w * h; i < end; i++)        /* B, G, R, X to R, G, B */
        rgb[3 * i] = px[4 * i + 2], rgb[3 * i + 1] = px[4 * i + 1], rgb[3 * i + 2] = px[4 * i];
    free(px);
    char path[PATH_MAX];
    snprintf(path, sizeof path, "%s/frame-%llu.png", g_dir, (unsigned long long)g_presents);
    write_png(path, rgb, w, h);
    free(rgb);
    fprintf(stderr, "[bw2] frame dump: %llu\n", (unsigned long long)g_presents);
}
