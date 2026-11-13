/*
 * d3dx_tex.c - D3DX's texture functions (plan 4 Task 9): create, load from
 * files and memory, convert, filter, fill, and save. Textures are made through
 * the device's own methods; their contents are written through the levels'
 * staging copies and uploaded in command order.
 */
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "d3d9_device.h"
#include "d3dx.h"

#define FMT_R8G8B8 20u
#define RT_TEXTURE 3u
#define RT_VOLUME 4u
#define RT_CUBE 5u

_Noreturn static void dx_fail(const char *what) { d3d_fail("D3DX: %s", what); }

/* ---- the guest's own calls: the device's methods, files ---- */
static uint32_t gcall(uint32_t va, int n, const uint32_t *a) { return hle_call_guest(va, n, a); }
static uint32_t method(uint32_t obj, int slot, int n, const uint32_t *args) {
    uint32_t a[16] = { obj };
    memcpy(a + 1, args, 4 * (size_t)n);
    return gcall(MEM32(MEM32(obj) + 4u * (uint32_t)slot), n + 1, a);
}
static uint32_t imp(const char *name) {
    uint32_t va = hle_resolve(name);
    if (!va) d3d_fail("D3DX needs %s", name);
    return va;
}
/* A file through CreateFileA/ReadFile: malloc'd bytes, or NULL. */
static uint8_t *read_guest_file(uint32_t path, uint32_t *n) {
    uint32_t a[7] = { path, 0x80000000u, 1, 0, 3, 0x80, 0 };
    uint32_t h = gcall(imp("CreateFileA"), 7, a);
    if (h == 0xFFFFFFFFu) return NULL;
    uint32_t b[2] = { h, 0 };
    uint32_t size = gcall(imp("GetFileSize"), 2, b), mark = hle_scratch_mark(), got = hle_scratch(4);
    uint32_t mem = hle_va(hle_alloc(size ? size : 1));
    uint32_t c[5] = { h, mem, size, got, 0 };
    uint32_t ok = gcall(imp("ReadFile"), 5, c);
    uint32_t d[1] = { h };
    gcall(imp("CloseHandle"), 1, d);
    uint8_t *r = NULL;
    if (ok && MEM32(got) == size) { r = malloc(size ? size : 1); memcpy(r, hle_ptr(mem), size); *n = size; }
    hle_scratch_release(mark);
    hle_free(hle_ptr(mem));
    return r;
}
static int write_guest_file(uint32_t path, const uint8_t *data, uint32_t n) {
    uint32_t a[7] = { path, 0x40000000u, 0, 0, 2, 0x80, 0 };
    uint32_t h = gcall(imp("CreateFileA"), 7, a);
    if (h == 0xFFFFFFFFu) return 0;
    uint32_t mem = hle_va(hle_alloc(n ? n : 1)), mark = hle_scratch_mark(), put = hle_scratch(4);
    memcpy(hle_ptr(mem), data, n);
    uint32_t c[5] = { h, mem, n, put, 0 };
    uint32_t ok = gcall(imp("WriteFile"), 5, c) && MEM32(put) == n;
    uint32_t d[1] = { h };
    gcall(imp("CloseHandle"), 1, d);
    hle_scratch_release(mark);
    hle_free(hle_ptr(mem));
    return (int)ok;
}

/* ---- pixels: resample, colour key, sRGB ---- */
struct box2 { uint32_t l, t, r, b; };

static float srgb_to_lin(float c) { return c <= 0.04045f ? c / 12.92f : powf((c + 0.055f) / 1.055f, 2.4f); }
static float lin_to_srgb(float c) { return c <= 0.0031308f ? c * 12.92f : 1.055f * powf(c, 1 / 2.4f) - 0.055f; }

static void sample_lin(const float *s, uint32_t w, uint32_t h, float x, float y, float *o) {
    x -= 0.5f, y -= 0.5f;
    int x0 = (int)floorf(x), y0 = (int)floorf(y);
    float fx = x - x0, fy = y - y0;
    for (int c = 0; c < 4; c++) o[c] = 0;
    for (int j = 0; j < 2; j++)
        for (int i = 0; i < 2; i++) {
            int xi = x0 + i < 0 ? 0 : x0 + i >= (int)w ? (int)w - 1 : x0 + i;
            int yj = y0 + j < 0 ? 0 : y0 + j >= (int)h ? (int)h - 1 : y0 + j;
            float wt = (i ? fx : 1 - fx) * (j ? fy : 1 - fy);
            for (int c = 0; c < 4; c++) o[c] += wt * s[4 * (yj * w + xi) + c];
        }
}
/* src (sw x sh float4s), its rect sr, into dst (dw x dh float4s). */
static void resample(const float *src, uint32_t sw, uint32_t sh, const struct box2 *sr, float *dst, uint32_t dw,
                     uint32_t dh, uint32_t filter) {
    (void)sh;
    uint32_t rw = sr->r - sr->l, rh = sr->b - sr->t, kind = filter & 0xFF;
    if (filter == D3DX_DEFAULT || kind == 0 || kind == D3DX_FILTER_TRIANGLE) kind = D3DX_FILTER_BOX;
    for (uint32_t y = 0; y < dh; y++)
        for (uint32_t x = 0; x < dw; x++) {
            float *o = dst + 4 * (y * dw + x);
            if (kind == D3DX_FILTER_NONE || (rw == dw && rh == dh)) {
                if (x < rw && y < rh) memcpy(o, src + 4 * ((sr->t + y) * sw + sr->l + x), 16);
                else memset(o, 0, 16);                       /* outside the source: transparent black */
            } else if (kind == D3DX_FILTER_POINT) {
                uint32_t sx = sr->l + (uint32_t)((x + 0.5f) * rw / dw), sy = sr->t + (uint32_t)((y + 0.5f) * rh / dh);
                memcpy(o, src + 4 * (sy * sw + sx), 16);
            } else if (kind == D3DX_FILTER_BOX && rw >= dw && rh >= dh) {
                uint32_t x0 = sr->l + x * rw / dw, x1 = sr->l + ((x + 1) * rw + dw - 1) / dw;
                uint32_t y0 = sr->t + y * rh / dh, y1 = sr->t + ((y + 1) * rh + dh - 1) / dh;
                float acc[4] = { 0 };
                uint32_t n = 0;
                for (uint32_t yy = y0; yy < y1; yy++)
                    for (uint32_t xx = x0; xx < x1; xx++, n++)
                        for (int c = 0; c < 4; c++) acc[c] += src[4 * (yy * sw + xx) + c];
                for (int c = 0; c < 4; c++) o[c] = acc[c] / (float)n;
            } else {
                /* bilinear, over the rect, edges clamped */
                float fx = sr->l + (x + 0.5f) * rw / dw, fy = sr->t + (y + 0.5f) * rh / dh;
                sample_lin(src, sw, sr->b, fx, fy, o);
            }
        }
}
static uint32_t argb8(const float *c) {
    uint32_t o = 0;
    for (int i = 0; i < 4; i++) {
        float v = c[i] < 0 ? 0 : c[i] > 1 ? 1 : c[i];
        o |= (uint32_t)(v * 255.0f + 0.5f) << (i == 3 ? 24 : 16 - 8 * i);
    }
    return o;
}
static void colorkey(float *px, uint32_t n, uint32_t key) {
    if (!key) return;
    for (uint32_t i = 0; i < n; i++)
        if (argb8(px + 4 * i) == key) memset(px + 4 * i, 0, 16);
}

/* The block-aligned box of a dst rect in a dw_all x dh_all level, clipped to the level. */
static struct box2 block_box(const struct box2 *dr, uint32_t dw_all, uint32_t dh_all) {
    struct box2 b = { dr->l & ~3u, dr->t & ~3u, (dr->r + 3) & ~3u, (dr->b + 3) & ~3u };
    if (b.r > dw_all) b.r = dw_all;
    if (b.b > dh_all) b.b = dh_all;
    return b;
}
/* Convert src (fmt, D3D layout, its rect) into the dst rect of a surface-shaped buffer of fmt dfmt,
 * dw_all x dh_all. Into DXT: the same blocks unscaled are copied; anything else is decoded, scaled and
 * encoded, and a rect that does not cover whole blocks merges into the blocks it touches. */
static uint32_t convert(uint32_t sfmt, const uint8_t *src, uint32_t spitch, uint32_t sw, uint32_t sh, const struct box2 *sr,
                        uint32_t dfmt, uint8_t *dst, uint32_t dpitch, uint32_t dw_all, uint32_t dh_all,
                        const struct box2 *dr, uint32_t filter, uint32_t key) {
    uint32_t dw = dr->r - dr->l, dh = dr->b - dr->t, bs = (uint32_t)d3dx_block(dfmt);
    if (!d3dx_can_read(sfmt) || !d3dx_can_write(dfmt)) return D3DERR_INVALIDCALL;
    struct box2 bb = bs == 4 ? block_box(dr, dw_all, dh_all) : *dr;
    int whole = bb.l == dr->l && bb.t == dr->t && bb.r == dr->r && bb.b == dr->b;
    if (bs == 4 && sfmt == dfmt && whole && !key && sr->r - sr->l == dw && sr->b - sr->t == dh && !((sr->l | sr->t) & 3)) {
        uint32_t rows = (dh + 3) / 4, bytes = (dw + 3) / 4 * d3dx_bytes(dfmt);
        for (uint32_t y = 0; y < rows; y++)
            memcpy(dst + (dr->t / 4 + y) * dpitch + dr->l / 4 * d3dx_bytes(dfmt),
                   src + (sr->t / 4 + y) * spitch + sr->l / 4 * d3dx_bytes(sfmt), bytes);
        return D3D_OK;
    }
    float *s = malloc(sizeof(float) * 4 * sw * sh), *d = malloc(sizeof(float) * 4 * dw * dh);
    d3dx_read(sfmt, src, spitch, sw, sh, s);
    colorkey(s, sw * sh, key);
    if (filter != D3DX_DEFAULT && (filter & D3DX_FILTER_SRGB_IN))
        for (uint32_t i = 0; i < sw * sh; i++) for (int c = 0; c < 3; c++) s[4 * i + c] = srgb_to_lin(s[4 * i + c]);
    resample(s, sw, sh, sr, d, dw, dh, filter);
    if (filter != D3DX_DEFAULT && (filter & D3DX_FILTER_SRGB_OUT))
        for (uint32_t i = 0; i < dw * dh; i++) for (int c = 0; c < 3; c++) d[4 * i + c] = lin_to_srgb(d[4 * i + c]);
    if (bs == 1 || whole) d3dx_write(dfmt, d, dw, dh, dst + dr->t / bs * dpitch + dr->l / bs * d3dx_bytes(dfmt), dpitch);
    else {                                                 /* decode the touched blocks, merge, encode them again */
        uint32_t mw = bb.r - bb.l, mh = bb.b - bb.t;
        uint8_t *at = dst + bb.t / 4 * dpitch + bb.l / 4 * d3dx_bytes(dfmt);
        float *m = malloc(sizeof(float) * 4 * mw * mh);
        d3dx_read(dfmt, at, dpitch, mw, mh, m);
        for (uint32_t y = 0; y < dh; y++)
            memcpy(m + 4 * ((size_t)(dr->t - bb.t + y) * mw + dr->l - bb.l), d + 4 * (size_t)y * dw, 16 * (size_t)dw);
        d3dx_write(dfmt, m, mw, mh, at, dpitch);
        free(m);
    }
    free(s), free(d);
    return D3D_OK;
}

/* ---- textures ---- */
static uint32_t dim(uint32_t v, uint32_t l) { return v >> l ? v >> l : 1; }

/* A texture subresource in D3D layout, current (a render target's comes back from the GPU). */
static uint8_t *sub_bytes(struct d3d_texture *t, uint32_t sub) {
    uint32_t l = sub % t->levels, face = sub / t->levels;
    uint8_t *mem = hle_ptr(d3d_tex_mem(t, sub));
    if ((t->usage & D3DUSAGE_RENDERTARGET) && t->tex) {
        struct d3d_device *d = d3d_host(t->device, D3DK_DEVICE);
        uint32_t w = dim(t->w, l), h = dim(t->h, l), sp = d3d_storage_pitch(t->f, w);
        uint8_t *tmp = malloc((size_t)sp * h);
        mtl_tex_download(d->mtl, t->tex, face, l, 0, 0, w, h, tmp, sp);
        d3d_from_storage(t->f, tmp, sp, mem, d3d_format_pitch(t->f, w), w, h);
        free(tmp);
    }
    return mem;
}
static void sub_upload(struct d3d_texture *t, uint32_t sub) {
    uint32_t l = sub % t->levels;
    struct d3d_box b = { 0, 0, dim(t->w, l), dim(t->h, l), 0, dim(t->d, l) };
    d3d_tex_upload(t, sub, &b);
}

/* Level l (all its slices) from level l-1, box filtered. */
static void make_mip(struct d3d_texture *t, uint32_t face, uint32_t l, uint32_t filter) {
    uint32_t sw = dim(t->w, l - 1), sh = dim(t->h, l - 1), sd = dim(t->d, l - 1);
    uint32_t dw = dim(t->w, l), dh = dim(t->h, l), dd = dim(t->d, l), fmt = t->fmt;
    const uint8_t *src = sub_bytes(t, face * t->levels + l - 1);
    uint8_t *dst = hle_ptr(d3d_tex_mem(t, face * t->levels + l));
    uint32_t sp = d3dx_pitch(fmt, sw), dp = d3dx_pitch(fmt, dw), ss = sp * d3dx_rows(fmt, sh), ds = dp * d3dx_rows(fmt, dh);
    float *s = malloc(sizeof(float) * 4 * sw * sh * sd), *d = malloc(sizeof(float) * 4 * dw * dh);
    for (uint32_t z = 0; z < sd; z++) d3dx_read(fmt, src + z * ss, sp, sw, sh, s + 4 * (size_t)z * sw * sh);
    struct box2 r = { 0, 0, sw, sh };
    for (uint32_t z = 0; z < dd; z++) {
        float *slice = s + 4 * (size_t)(sd > 1 ? 2 * z : 0) * sw * sh;
        if (sd > 1 && 2 * z + 1 < sd)                      /* a volume: average the two slices first */
            for (uint32_t i = 0; i < 4 * sw * sh; i++) slice[i] = (slice[i] + slice[i + 4 * sw * sh]) / 2;
        resample(slice, sw, sh, &r, d, dw, dh, filter == D3DX_DEFAULT ? D3DX_FILTER_BOX : filter);
        d3dx_write(fmt, d, dw, dh, dst + z * ds, dp);
    }
    free(s), free(d);
    sub_upload(t, face * t->levels + l);
}

static uint32_t create(uint32_t dev, uint32_t rtype, uint32_t w, uint32_t h, uint32_t d, uint32_t levels,
                       uint32_t usage, uint32_t fmt, uint32_t pool, uint32_t *out) {
    uint32_t mark = hle_scratch_mark(), o = hle_scratch(4), hr;
    if (rtype == RT_CUBE) {
        uint32_t a[7] = { w, levels, usage, fmt, pool, o, 0 };
        hr = method(dev, 25, 7, a);
    } else if (rtype == RT_VOLUME) {
        uint32_t a[9] = { w, h, d, levels, usage, fmt, pool, o, 0 };
        hr = method(dev, 24, 9, a);
    } else {
        uint32_t a[8] = { w, h, levels, usage, fmt, pool, o, 0 };
        hr = method(dev, 23, 8, a);
    }
    *out = hr ? 0 : MEM32(o);
    hle_scratch_release(mark);
    return hr;
}

/* A supported format nearest the one wanted (the D3DXCheckTextureRequirements rule, simply). */
static uint32_t pick_format(uint32_t want, uint32_t file_fmt, uint32_t rtype, uint32_t usage) {
    uint32_t f = want == D3DX_DEFAULT || want == 0 || want == D3DFMT_FROM_FILE ? file_fmt : want;
    if (!f) f = D3DFMT_A8R8G8B8;
    if (f == FMT_R8G8B8) f = D3DFMT_X8R8G8B8;
    uint32_t r = d3d_check_format(D3DFMT_X8R8G8B8, usage & D3DUSAGE_RENDERTARGET, rtype, f);
    if (r != D3D_OK && r != D3DOK_NOAUTOGEN) f = D3DFMT_A8R8G8B8;
    return f;
}
static uint32_t size_or(uint32_t v, uint32_t file) {
    return v == 0 || v == D3DX_DEFAULT || v == D3DX_DEFAULT_NONPOW2 || v == D3DX_FROM_FILE ? file : v;
}

static void put_info(uint32_t info, const struct d3dx_image *im) {
    if (!info) return;
    MEM32(info) = im->w, MEM32(info + 4) = im->h, MEM32(info + 8) = im->d, MEM32(info + 12) = im->levels;
    MEM32(info + 16) = im->fmt, MEM32(info + 20) = im->rtype, MEM32(info + 24) = im->iff;
}

static uint32_t from_image(uint32_t dev, const struct d3dx_image *im, uint32_t rtype, uint32_t w, uint32_t h,
                           uint32_t dpt, uint32_t mips, uint32_t usage, uint32_t fmt, uint32_t pool, uint32_t filter,
                           uint32_t mipfilter, uint32_t key, uint32_t info, uint32_t *out) {
    w = size_or(w, im->w), h = rtype == RT_CUBE ? w : size_or(h, im->h), dpt = size_or(dpt, im->d);
    uint32_t levels = mips == D3DX_FROM_FILE ? im->levels : mips == D3DX_DEFAULT ? 0 : mips;
    fmt = pick_format(fmt, im->fmt, rtype, usage);
    uint32_t tex, hr = create(dev, rtype, w, h, dpt, levels, usage, fmt, pool, &tex);
    if (hr) return hr;
    struct d3d_texture *t = d3d_tex(tex);
    uint32_t faces = rtype == RT_CUBE ? 6 : 1;
    if (rtype == RT_CUBE && im->faces != 6) dx_fail("a cube texture from an image that is not a cube");
    for (uint32_t f = 0; f < faces; f++)
        for (uint32_t l = 0; l < t->levels; l++) {
            uint32_t lw = dim(w, l), lh = dim(h, l), ld = dim(dpt, l), sub = f * t->levels + l;
            int have = l < im->levels && d3dx_image_level_w(im, l) == lw && d3dx_image_level_h(im, l) == lh &&
                       d3dx_image_level_d(im, l) == ld;
            if (have || l == 0) {
                uint32_t il = have ? l : 0, iw = d3dx_image_level_w(im, il), ih = d3dx_image_level_h(im, il);
                const uint8_t *src = im->data + im->off[f * im->levels + il];
                uint8_t *dst = hle_ptr(d3d_tex_mem(t, sub));
                uint32_t sp = d3dx_pitch(im->fmt, iw), dp = d3d_format_pitch(t->f, lw);
                uint32_t slices = have ? ld : 1;
                for (uint32_t z = 0; z < slices; z++) {
                    struct box2 sr = { 0, 0, iw, ih }, dr = { 0, 0, lw, lh };
                    hr = convert(im->fmt, src + z * sp * d3dx_rows(im->fmt, ih), sp, iw, ih, &sr, fmt,
                                 dst + z * dp * d3d_format_rows(t->f, lh), dp, lw, lh, &dr,
                                 have && iw == lw ? D3DX_FILTER_NONE : filter, key);
                    if (hr) { d3d_release(tex); return hr; }
                }
                sub_upload(t, sub);
            } else make_mip(t, f, l, mipfilter);
        }
    put_info(info, im);
    *out = tex;
    return D3D_OK;
}

static uint32_t from_memory(uint32_t dev, uint32_t data, uint32_t n, uint32_t rtype, uint32_t w, uint32_t h,
                            uint32_t d, uint32_t mips, uint32_t usage, uint32_t fmt, uint32_t pool, uint32_t filter,
                            uint32_t mipfilter, uint32_t key, uint32_t info, uint32_t out) {
    if (!dev || !data || !n || !out) return D3DERR_INVALIDCALL;
    struct d3dx_image im;
    uint32_t hr = d3dx_image_decode(hle_ptr(data), n, &im);
    if (hr) return hr;
    uint32_t tex = 0;
    hr = from_image(dev, &im, rtype, w, h, d, mips, usage, fmt, pool, filter, mipfilter, key, info, &tex);
    d3dx_image_free(&im);
    MEM32(out) = tex;
    return hr;
}

static void x_CreateTexture(void) {        /* (device, w, h, mips, usage, fmt, pool, IDirect3DTexture9**) */
    uint32_t w = A32(1), h = A32(2), mips = A32(3), fmt = A32(5), tex;
    if (!A32(0) || !A32(7)) RET(D3DERR_INVALIDCALL, 8);
    if (w == 0 || w == D3DX_DEFAULT) w = h && h != D3DX_DEFAULT ? h : 256;
    if (h == 0 || h == D3DX_DEFAULT) h = w;
    if (mips == D3DX_DEFAULT) mips = 0;
    fmt = pick_format(fmt, D3DFMT_A8R8G8B8, RT_TEXTURE, A32(4));
    uint32_t hr = create(A32(0), RT_TEXTURE, w, h, 1, mips, A32(4), fmt, A32(6), &tex);
    MEM32(A32(7)) = tex;
    RET(hr, 8);
}
static void x_CreateCubeTexture(void) {    /* (device, size, mips, usage, fmt, pool, IDirect3DCubeTexture9**) */
    uint32_t s = A32(1), mips = A32(2), tex;
    if (!A32(0) || !A32(6)) RET(D3DERR_INVALIDCALL, 7);
    if (s == 0 || s == D3DX_DEFAULT) s = 256;
    if (mips == D3DX_DEFAULT) mips = 0;
    uint32_t hr = create(A32(0), RT_CUBE, s, s, 1, mips, A32(3), pick_format(A32(4), D3DFMT_A8R8G8B8, RT_CUBE, A32(3)),
                         A32(5), &tex);
    MEM32(A32(6)) = tex;
    RET(hr, 7);
}
static void x_GetImageInfoFromFileInMemory(void) {   /* (data, size, D3DXIMAGE_INFO*) */
    if (!A32(0) || !A32(1)) RET(D3DERR_INVALIDCALL, 3);
    struct d3dx_image im;
    uint32_t hr = d3dx_image_decode(APTR(0), A32(1), &im);
    if (hr) RET(hr, 3);
    put_info(A32(2), &im);
    d3dx_image_free(&im);
    RET(D3D_OK, 3);
}
static void x_CreateTextureFromFileInMemory(void) {  /* (device, data, size, IDirect3DTexture9**) */
    RET(from_memory(A32(0), A32(1), A32(2), RT_TEXTURE, D3DX_DEFAULT, D3DX_DEFAULT, 1, D3DX_DEFAULT, 0, 0,
                    D3DPOOL_MANAGED, D3DX_DEFAULT, D3DX_DEFAULT, 0, 0, A32(3)), 4);
}
static void x_CreateTextureFromFileInMemoryEx(void) {   /* (dev, data, size, w, h, mips, usage, fmt, pool, filter, mipfilter, key, info, pal, out) */
    RET(from_memory(A32(0), A32(1), A32(2), RT_TEXTURE, A32(3), A32(4), 1, A32(5), A32(6), A32(7), A32(8), A32(9),
                    A32(10), A32(11), A32(12), A32(14)), 15);
}
static void x_CreateCubeTextureFromFileInMemoryEx(void) {   /* (dev, data, size, size, mips, usage, fmt, pool, filter, mipfilter, key, info, pal, out) */
    RET(from_memory(A32(0), A32(1), A32(2), RT_CUBE, A32(3), A32(3), 1, A32(4), A32(5), A32(6), A32(7), A32(8),
                    A32(9), A32(10), A32(11), A32(13)), 14);
}
static void x_CreateVolumeTextureFromFileInMemoryEx(void) { /* (dev, data, size, w, h, d, mips, usage, fmt, pool, filter, mipfilter, key, info, pal, out) */
    RET(from_memory(A32(0), A32(1), A32(2), RT_VOLUME, A32(3), A32(4), A32(5), A32(6), A32(7), A32(8), A32(9), A32(10),
                    A32(11), A32(12), A32(13), A32(15)), 16);
}
static void x_CreateTextureFromFileA(void) {         /* (device, path, IDirect3DTexture9**) */
    uint32_t n;
    if (!A32(1) || !A32(2)) RET(D3DERR_INVALIDCALL, 3);
    uint8_t *buf = read_guest_file(A32(1), &n);
    if (!buf) RET(D3DERR_NOTFOUND, 3);
    uint32_t mem = hle_va(hle_alloc(n));
    memcpy(hle_ptr(mem), buf, n);
    free(buf);
    uint32_t hr = from_memory(A32(0), mem, n, RT_TEXTURE, D3DX_DEFAULT, D3DX_DEFAULT, 1, D3DX_DEFAULT, 0, 0,
                              D3DPOOL_MANAGED, D3DX_DEFAULT, D3DX_DEFAULT, 0, 0, A32(2));
    hle_free(hle_ptr(mem));
    RET(hr, 3);
}

/* ---- surfaces ---- */
static int rect_of(uint32_t va, uint32_t w, uint32_t h, struct box2 *r) {
    if (!va) { *r = (struct box2){ 0, 0, w, h }; return 1; }
    int32_t q[4];
    memcpy(q, hle_ptr(va), 16);
    if (q[0] < 0 || q[1] < 0 || q[2] > (int32_t)w || q[3] > (int32_t)h || q[0] >= q[2] || q[1] >= q[3]) return 0;
    *r = (struct box2){ (uint32_t)q[0], (uint32_t)q[1], (uint32_t)q[2], (uint32_t)q[3] };
    return 1;
}
/* Load src (fmt, its whole width/height known through sw/sh) into a rect of a surface. */
static uint32_t load_surface(uint32_t dsurf, uint32_t drect, uint32_t sfmt, const uint8_t *src, uint32_t spitch,
                             uint32_t sw, uint32_t sh, const struct box2 *sr, uint32_t filter, uint32_t key) {
    struct d3d_surface *s = d3d_surf(dsurf);
    struct box2 dr;
    if (!rect_of(drect, s->w, s->h, &dr)) return D3DERR_INVALIDCALL;
    int partial = dr.l || dr.t || dr.r != s->w || dr.b != s->h;
    if (partial && s->tex && !s->parent && (s->type_usage & D3DUSAGE_RENDERTARGET)) d3d_surface_download(s);
    uint32_t hr = convert(sfmt, src, spitch, sw, sh, sr, s->fmt, hle_ptr(d3d_surface_mem(s)), s->pitch, s->w, s->h, &dr,
                          filter, key);
    if (hr) return hr;
    if (s->f->block == 4) dr = block_box(&dr, s->w, s->h);  /* the blocks written, whole (a GPU copy of BC is per block) */
    struct d3d_rect r = { (int32_t)dr.l, (int32_t)dr.t, (int32_t)dr.r, (int32_t)dr.b };
    d3d_surface_upload(s, &r);
    return D3D_OK;
}
static void x_LoadSurfaceFromMemory(void) {   /* (dst, dstpal, dstrect, src, srcfmt, srcpitch, srcpal, srcrect, filter, key) */
    if (!A32(0) || !A32(3) || !A32(7)) RET(D3DERR_INVALIDCALL, 10);
    int32_t q[4];
    memcpy(q, APTR(7), 16);
    if (q[0] < 0 || q[1] < 0 || q[0] >= q[2] || q[1] >= q[3]) RET(D3DERR_INVALIDCALL, 10);
    /* the source is addressed from its rect's corner: read just the rect, from (0, 0) of that region */
    uint32_t sfmt = A32(4), bs = (uint32_t)d3dx_block(sfmt), w = (uint32_t)(q[2] - q[0]), h = (uint32_t)(q[3] - q[1]);
    if (bs == 4 && ((q[0] | q[1]) & 3)) RET(D3DERR_INVALIDCALL, 10);
    const uint8_t *src = (const uint8_t *)APTR(3) + ((uint32_t)q[1] / bs) * A32(5) + ((uint32_t)q[0] / bs) * d3dx_bytes(sfmt);
    struct box2 sr = { 0, 0, w, h };
    RET(load_surface(A32(0), A32(2), sfmt, src, A32(5), w, h, &sr, A32(8), A32(9)), 10);
}
static void x_LoadSurfaceFromSurface(void) {  /* (dst, dstpal, dstrect, src, srcpal, srcrect, filter, key) */
    if (!A32(0) || !A32(3)) RET(D3DERR_INVALIDCALL, 8);
    struct d3d_surface *s = d3d_surf(A32(3));
    if (s->tex && !s->parent && (s->type_usage & D3DUSAGE_RENDERTARGET)) d3d_surface_download(s);
    else if (s->parent) (void)sub_bytes(s->parent, s->sub);
    struct box2 sr;
    if (!rect_of(A32(5), s->w, s->h, &sr)) RET(D3DERR_INVALIDCALL, 8);
    RET(load_surface(A32(0), A32(2), s->fmt, hle_ptr(d3d_surface_mem(s)), s->pitch, s->w, s->h, &sr, A32(6), A32(7)), 8);
}
static void x_LoadSurfaceFromFileInMemory(void) {   /* (dst, dstpal, dstrect, data, size, srcrect, filter, key, info) */
    if (!A32(0) || !A32(3) || !A32(4)) RET(D3DERR_INVALIDCALL, 9);
    struct d3dx_image im;
    uint32_t hr = d3dx_image_decode(APTR(3), A32(4), &im);
    if (hr) RET(hr, 9);
    struct box2 sr;
    if (!rect_of(A32(5), im.w, im.h, &sr)) { d3dx_image_free(&im); RET(D3DERR_INVALIDCALL, 9); }
    hr = load_surface(A32(0), A32(2), im.fmt, im.data, d3dx_pitch(im.fmt, im.w), im.w, im.h, &sr, A32(6), A32(7));
    put_info(A32(8), &im);
    d3dx_image_free(&im);
    RET(hr, 9);
}

/* ---- filter and fill ---- */
static void x_FilterTexture(void) {        /* (texture, palette, src level, filter) */
    if (!A32(0)) RET(D3DERR_INVALIDCALL, 4);
    struct d3d_texture *t = d3d_tex(A32(0));
    uint32_t src = A32(2) == D3DX_DEFAULT ? 0 : A32(2), filter = A32(3) == D3DX_DEFAULT ? D3DX_FILTER_BOX : A32(3);
    if (src >= t->levels) RET(D3DERR_INVALIDCALL, 4);
    if (t->autogen) {
        mtl_tex_mipgen(((struct d3d_device *)d3d_host(t->device, D3DK_DEVICE))->mtl, t->tex);
        RET(D3D_OK, 4);
    }
    for (uint32_t f = 0; f < t->faces; f++)
        for (uint32_t l = src + 1; l < t->levels; l++) make_mip(t, f, l, filter);
    RET(D3D_OK, 4);
}
/* One face's texels through the callback, every level: 2D texture coordinates, or a cube direction. */
static void fill(struct d3d_texture *t, uint32_t face, uint32_t fn, uint32_t data, int cube) {
    uint32_t mark = hle_scratch_mark(), out = hle_scratch(16), tc = hle_scratch(16), ts = hle_scratch(16);
    for (uint32_t l = 0; l < t->levels; l++) {
        uint32_t w = dim(t->w, l), h = dim(t->h, l), sub = face * t->levels + l;
        float *px = malloc(sizeof(float) * 4 * w * h);
        for (uint32_t y = 0; y < h; y++)
            for (uint32_t x = 0; x < w; x++) {
                float c[3], sz[3];
                if (!cube) c[0] = (x + 0.5f) / w, c[1] = (y + 0.5f) / h, c[2] = 0, sz[0] = 1.0f / w, sz[1] = 1.0f / h, sz[2] = 0;
                else {                                     /* the face's direction, s across and u down, in -1..1 */
                    float s = 2 * (x + 0.5f) / w - 1, u = 2 * (y + 0.5f) / h - 1;
                    const float d[6][3] = { { 1, -u, -s }, { -1, -u, s }, { s, 1, u }, { s, -1, -u }, { s, -u, 1 },
                                            { -s, -u, -1 } };
                    memcpy(c, d[face], sizeof c);
                    sz[0] = 2.0f / w, sz[1] = 2.0f / h, sz[2] = 0;
                }
                memcpy(hle_ptr(tc), c, 12), memcpy(hle_ptr(ts), sz, 12);
                memset(hle_ptr(out), 0, 16);
                uint32_t a[4] = { out, tc, ts, data };
                gcall(fn, 4, a);
                memcpy(px + 4 * (y * w + x), hle_ptr(out), 16);
            }
        d3dx_write(t->fmt, px, w, h, hle_ptr(d3d_tex_mem(t, sub)), d3d_format_pitch(t->f, w));
        free(px);
        sub_upload(t, sub);
    }
    hle_scratch_release(mark);
}
static void x_FillTexture(void) {          /* (texture, LPD3DXFILL2D, data) */
    if (!A32(0) || !A32(1)) RET(D3DERR_INVALIDCALL, 3);
    struct d3d_texture *t = d3d_tex(A32(0));
    if (t->base.kind != D3DK_TEXTURE) RET(D3DERR_INVALIDCALL, 3);
    fill(t, 0, A32(1), A32(2), 0);
    RET(D3D_OK, 3);
}
static void x_FillCubeTexture(void) {      /* (cube texture, LPD3DXFILL3D, data) */
    if (!A32(0) || !A32(1)) RET(D3DERR_INVALIDCALL, 3);
    struct d3d_texture *t = d3d_tex(A32(0));
    if (t->base.kind != D3DK_CUBETEXTURE) RET(D3DERR_INVALIDCALL, 3);
    for (uint32_t f = 0; f < 6; f++) fill(t, f, A32(1), A32(2), 1);
    RET(D3D_OK, 3);
}

/* ---- saving ---- */
static uint8_t *encode_rgba(uint32_t iff, uint32_t fmt, const uint8_t *src, uint32_t pitch, uint32_t w, uint32_t h,
                            uint32_t *n) {
    float *px = malloc(sizeof(float) * 4 * w * h);
    d3dx_read(fmt, src, pitch, w, h, px);
    uint8_t *r = NULL;
    switch (iff) {
    case IFF_BMP: case IFF_DIB: r = d3dx_encode_bmp(px, w, h, n); break;
    case IFF_TGA: r = d3dx_encode_tga(px, w, h, n); break;
    case IFF_PNG: r = d3dx_imageio_encode(px, w, h, 0, n); break;
    case IFF_JPG: r = d3dx_imageio_encode(px, w, h, 1, n); break;
    default: free(px); dx_fail("saving to this file format is not supported");
    }
    free(px);
    return r;
}
static uint8_t *encode_texture(uint32_t iff, struct d3d_texture *t, uint32_t *n) {
    if (iff == IFF_DDS) {
        uint32_t ns = t->faces * t->levels;
        const uint8_t **subs = calloc(ns, sizeof *subs);
        for (uint32_t i = 0; i < ns; i++) subs[i] = sub_bytes(t, i);
        uint8_t *r = d3dx_encode_dds(t->fmt, t->w, t->h, t->levels, t->faces, subs, n);
        free(subs);
        return r;
    }
    return encode_rgba(iff, t->fmt, sub_bytes(t, 0), d3d_format_pitch(t->f, t->w), t->w, t->h, n);
}
static void x_SaveTextureToFileA(void) {   /* (path, iff, texture, palette) */
    if (!A32(0) || !A32(2)) RET(D3DERR_INVALIDCALL, 4);
    uint32_t n;
    uint8_t *f = encode_texture(A32(1), d3d_tex(A32(2)), &n);
    int ok = f && write_guest_file(A32(0), f, n);
    free(f);
    RET(ok ? D3D_OK : D3DERR_INVALIDCALL, 4);
}
static void x_SaveTextureToFileInMemory(void) {   /* (ID3DXBuffer**, iff, texture, palette) */
    if (!A32(0) || !A32(2)) RET(D3DERR_INVALIDCALL, 4);
    uint32_t n;
    uint8_t *f = encode_texture(A32(1), d3d_tex(A32(2)), &n);
    if (!f) RET(D3DERR_INVALIDCALL, 4);
    uint32_t b = d3dx_buffer_new(n);
    memcpy(hle_ptr(d3dx_buffer_ptr(b)), f, n);
    free(f);
    MEM32(A32(0)) = b;
    RET(D3D_OK, 4);
}
static void x_SaveSurfaceToFileA(void) {   /* (path, iff, surface, palette, rect) */
    if (!A32(0) || !A32(2)) RET(D3DERR_INVALIDCALL, 5);
    struct d3d_surface *s = d3d_surf(A32(2));
    if (s->tex && !s->parent && (s->type_usage & D3DUSAGE_RENDERTARGET)) d3d_surface_download(s);
    else if (s->parent) (void)sub_bytes(s->parent, s->sub);
    struct box2 r;
    if (!rect_of(A32(4), s->w, s->h, &r)) RET(D3DERR_INVALIDCALL, 5);
    uint32_t w = r.r - r.l, h = r.b - r.t, n, bs = (uint32_t)d3dx_block(s->fmt);
    const uint8_t *src = (const uint8_t *)hle_ptr(d3d_surface_mem(s)) + (r.t / bs) * s->pitch + (r.l / bs) * s->f->bytes;
    uint8_t *f;
    if (A32(1) == IFF_DDS) {
        const uint8_t *subs[1] = { src };
        if (r.l || r.t || w != s->w) dx_fail("saving part of a surface as DDS is not supported");
        f = d3dx_encode_dds(s->fmt, w, h, 1, 1, subs, &n);
    } else f = encode_rgba(A32(1), s->fmt, src, s->pitch, w, h, &n);
    int ok = f && write_guest_file(A32(0), f, n);
    free(f);
    RET(ok ? D3D_OK : D3DERR_INVALIDCALL, 5);
}

static const win32hle_shim g_exports[] = {
    { "D3DXCreateTexture", x_CreateTexture }, { "D3DXCreateCubeTexture", x_CreateCubeTexture },
    { "D3DXGetImageInfoFromFileInMemory", x_GetImageInfoFromFileInMemory },
    { "D3DXCreateTextureFromFileInMemory", x_CreateTextureFromFileInMemory },
    { "D3DXCreateTextureFromFileInMemoryEx", x_CreateTextureFromFileInMemoryEx },
    { "D3DXCreateCubeTextureFromFileInMemoryEx", x_CreateCubeTextureFromFileInMemoryEx },
    { "D3DXCreateVolumeTextureFromFileInMemoryEx", x_CreateVolumeTextureFromFileInMemoryEx },
    { "D3DXCreateTextureFromFileA", x_CreateTextureFromFileA },
    { "D3DXLoadSurfaceFromMemory", x_LoadSurfaceFromMemory },
    { "D3DXLoadSurfaceFromSurface", x_LoadSurfaceFromSurface },
    { "D3DXLoadSurfaceFromFileInMemory", x_LoadSurfaceFromFileInMemory },
    { "D3DXFilterTexture", x_FilterTexture },
    { "D3DXFillTexture", x_FillTexture }, { "D3DXFillCubeTexture", x_FillCubeTexture },
    { "D3DXSaveTextureToFileA", x_SaveTextureToFileA },
    { "D3DXSaveTextureToFileInMemory", x_SaveTextureToFileInMemory },
    { "D3DXSaveSurfaceToFileA", x_SaveSurfaceToFileA },
    { 0, 0 } };
void d3dx_buffer_register(void);
void d3dx_tex_register(void) { d3dx_buffer_register(); win32hle_register(g_exports); }
