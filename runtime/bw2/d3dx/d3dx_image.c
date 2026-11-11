/*
 * d3dx_image.c - image files for D3DX (plan 4 Task 9): DDS (2D, cube and
 * volume, with mips), TGA (types 2, 3, 10, 11; 8/16/24/32-bit; either
 * origin) and uncompressed BMP decoded here; PNG, JPEG and other BMPs through
 * ImageIO. Writers for BMP, TGA and DDS.
 */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "d3d9_format.h"
#include "d3dx.h"

#define FMT_R8G8B8 20u
#define RT_TEXTURE 3u
#define RT_VOLUME 4u
#define RT_CUBE 5u
/* Caps on what a file may claim, so no size arithmetic below can wrap (sizes are summed in 64 bits). */
#define MAX_DIM 16384u
#define MAX_DEPTH 2048u
#define MAX_LEVELS 16u
#define MAX_BYTES (1ull << 30)

static uint32_t dim(uint32_t v, uint32_t l) { return v >> l ? v >> l : 1; }
uint32_t d3dx_image_level_w(const struct d3dx_image *im, uint32_t l) { return dim(im->w, l); }
uint32_t d3dx_image_level_h(const struct d3dx_image *im, uint32_t l) { return dim(im->h, l); }
uint32_t d3dx_image_level_d(const struct d3dx_image *im, uint32_t l) { return dim(im->d, l); }

void d3dx_image_free(struct d3dx_image *im) { free(im->data); free(im->off); memset(im, 0, sizeof *im); }

static uint32_t rd32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }

/* Lay out the subresources (the data is copied in by the caller); 0 for sizes past the caps. */
static int layout(struct d3dx_image *im) {
    if (!im->w || !im->h || !im->d || im->w > MAX_DIM || im->h > MAX_DIM || im->d > MAX_DEPTH ||
        !im->levels || im->levels > MAX_LEVELS || (im->faces != 1 && im->faces != 6))
        return 0;
    uint32_t n = im->faces * im->levels;
    uint64_t off = 0;
    im->off = calloc(n, 4);
    if (!im->off) return 0;
    for (uint32_t f = 0; f < im->faces; f++)
        for (uint32_t l = 0; l < im->levels; l++) {
            im->off[f * im->levels + l] = (uint32_t)off;
            off += (uint64_t)d3dx_pitch(im->fmt, dim(im->w, l)) * d3dx_rows(im->fmt, dim(im->h, l)) * dim(im->d, l);
            if (off > MAX_BYTES) { free(im->off); im->off = NULL; return 0; }
        }
    im->size = (uint32_t)off;
    im->data = calloc(1, off ? (size_t)off : 1);
    if (!im->data) { free(im->off); im->off = NULL; return 0; }
    return 1;
}

/* ---- DDS ---- */
static uint32_t dds_format(const uint8_t *pf) {
    uint32_t flags = rd32(pf + 4), cc = rd32(pf + 8), bits = rd32(pf + 12);
    uint32_t r = rd32(pf + 16), g = rd32(pf + 20), b = rd32(pf + 24), a = rd32(pf + 28);
    if (flags & 4) {                                       /* DDPF_FOURCC: DXTn, or a D3DFORMAT number */
        if (cc == D3DFMT_DXT1 || cc == D3DFMT_DXT2 || cc == D3DFMT_DXT3 || cc == D3DFMT_DXT4 || cc == D3DFMT_DXT5) return cc;
        if (d3d_format(cc)) return cc;
        return 0;
    }
    if (flags & 0x40) {                                    /* DDPF_RGB */
        if (bits == 32 && r == 0xFF0000 && g == 0xFF00 && b == 0xFF) return (flags & 1) && a ? D3DFMT_A8R8G8B8 : D3DFMT_X8R8G8B8;
        if (bits == 32 && r == 0xFF && g == 0xFF00 && b == 0xFF0000) return (flags & 1) && a ? D3DFMT_A8B8G8R8 : D3DFMT_X8B8G8R8;
        if (bits == 32 && r == 0xFFFF && g == 0xFFFF0000) return D3DFMT_G16R16;
        if (bits == 24 && r == 0xFF0000 && g == 0xFF00 && b == 0xFF) return FMT_R8G8B8;
        if (bits == 16 && r == 0xF800 && g == 0x7E0 && b == 0x1F) return D3DFMT_R5G6B5;
        if (bits == 16 && r == 0x7C00 && g == 0x3E0 && b == 0x1F) return (flags & 1) && a ? D3DFMT_A1R5G5B5 : D3DFMT_X1R5G5B5;
        if (bits == 16 && r == 0xF00 && g == 0xF0 && b == 0xF) return (flags & 1) && a ? D3DFMT_A4R4G4B4 : D3DFMT_X4R4G4B4;
        return 0;
    }
    if (flags & 0x20000) {                                 /* DDPF_LUMINANCE */
        if (bits == 8) return D3DFMT_L8;
        if (bits == 16 && (flags & 1)) return D3DFMT_A8L8;
        if (bits == 16) return D3DFMT_L16;
        return 0;
    }
    if ((flags & 2) && bits == 8) return D3DFMT_A8;         /* DDPF_ALPHA */
    if ((flags & 0x80000) && bits == 16) return D3DFMT_V8U8;    /* DDPF_BUMPDUDV */
    if ((flags & 0x80000) && bits == 32) return D3DFMT_Q8W8V8U8;
    return 0;
}
static uint32_t decode_dds(const uint8_t *buf, uint32_t n, struct d3dx_image *im) {
    if (n < 128 || rd32(buf + 4) != 124) return D3DXERR_INVALIDDATA;
    const uint8_t *h = buf + 4;
    uint32_t caps2 = rd32(h + 108);
    im->iff = IFF_DDS;
    im->h = rd32(h + 8), im->w = rd32(h + 12);
    im->d = (caps2 & 0x200000) ? rd32(h + 20) : 1;
    if (!im->d) im->d = 1;
    im->levels = (rd32(h + 4) & 0x20000) && rd32(h + 24) ? rd32(h + 24) : 1;
    im->faces = (caps2 & 0x200) ? 6 : 1;
    im->rtype = im->faces == 6 ? RT_CUBE : im->d > 1 ? RT_VOLUME : RT_TEXTURE;
    im->fmt = dds_format(h + 72);
    if (!im->fmt || !im->w || !im->h || im->levels > 16) return D3DXERR_INVALIDDATA;
    if (im->faces == 6 && (caps2 & 0xFC00) != 0xFC00) return D3DXERR_INVALIDDATA;   /* partial cubes: not here */
    if (!layout(im)) return D3DXERR_INVALIDDATA;
    if ((uint64_t)n - 128 < im->size) { d3dx_image_free(im); return D3DXERR_INVALIDDATA; }
    memcpy(im->data, buf + 128, im->size);
    return 0;
}

/* ---- TGA ---- */
static uint32_t decode_tga(const uint8_t *buf, uint32_t n, struct d3dx_image *im) {
    if (n < 18) return D3DXERR_INVALIDDATA;
    uint32_t idlen = buf[0], cmap = buf[1], type = buf[2], w = rd16(buf + 12), h = rd16(buf + 14), bpp = buf[16];
    uint32_t desc = buf[17];
    if (cmap || (type != 2 && type != 3 && type != 10 && type != 11) || !w || !h) return D3DXERR_INVALIDDATA;
    int gray = type == 3 || type == 11, rle = type >= 9;
    uint32_t fmt = gray ? (bpp == 8 ? D3DFMT_L8 : 0)
                 : bpp == 32 ? D3DFMT_A8R8G8B8 : bpp == 24 ? FMT_R8G8B8
                 : bpp == 16 ? ((desc & 15) ? D3DFMT_A1R5G5B5 : D3DFMT_X1R5G5B5) : 0;
    if (!fmt) return D3DXERR_INVALIDDATA;
    uint32_t px = bpp / 8;
    uint64_t pos = 18 + idlen;
    im->iff = IFF_TGA, im->rtype = RT_TEXTURE, im->w = w, im->h = h, im->d = 1, im->levels = 1, im->faces = 1, im->fmt = fmt;
    if (!layout(im)) return D3DXERR_INVALIDDATA;           /* w and h within MAX_DIM from here */
    uint64_t total = (uint64_t)w * h, k = 0;
    uint8_t *tmp = malloc((size_t)(total * px));
    if (!tmp) { d3dx_image_free(im); return D3DXERR_INVALIDDATA; }
    while (k < total) {
        if (!rle) {
            if (pos + total * px > n) goto bad;
            memcpy(tmp, buf + pos, (size_t)(total * px));
            k = total;
            break;
        }
        if (pos >= n) goto bad;
        uint32_t hdr = buf[pos++], cnt = (hdr & 0x7F) + 1;
        if (k + cnt > total) goto bad;
        if (hdr & 0x80) {
            if (pos + px > n) goto bad;
            for (uint32_t i = 0; i < cnt; i++) memcpy(tmp + (k + i) * px, buf + pos, px);
            pos += px;
        } else {
            if (pos + (uint64_t)cnt * px > n) goto bad;
            memcpy(tmp + k * px, buf + pos, (size_t)cnt * px);
            pos += (uint64_t)cnt * px;
        }
        k += cnt;
    }
    int top = (desc & 0x20) != 0, right = (desc & 0x10) != 0;
    for (uint32_t y = 0; y < h; y++)
        for (uint32_t x = 0; x < w; x++) {
            uint32_t sy = top ? y : h - 1 - y, sx = right ? w - 1 - x : x;
            memcpy(im->data + (y * w + x) * px, tmp + (sy * w + sx) * px, px);
        }
    free(tmp);
    return 0;
bad:
    free(tmp);
    d3dx_image_free(im);
    return D3DXERR_INVALIDDATA;
}

/* ---- BMP: 24- and 32-bit BI_RGB here, the rest through ImageIO ---- */
static uint32_t decode_bmp(const uint8_t *buf, uint32_t n, struct d3dx_image *im) {
    if (n < 54) return D3DXERR_INVALIDDATA;
    uint32_t off = rd32(buf + 10), hsize = rd32(buf + 14), bpp = rd16(buf + 28), comp = rd32(buf + 30);
    int32_t w = (int32_t)rd32(buf + 18), h = (int32_t)rd32(buf + 22);
    if (hsize < 40 || comp != 0 || (bpp != 24 && bpp != 32) || w <= 0 || h == 0) return 1;   /* 1: try ImageIO */
    int top = h < 0;
    if (h == INT32_MIN) return D3DXERR_INVALIDDATA;
    uint32_t uh = (uint32_t)(top ? -h : h), uw = (uint32_t)w, px = bpp / 8;
    if (uw > MAX_DIM || uh > MAX_DIM) return D3DXERR_INVALIDDATA;
    uint64_t stride = ((uint64_t)uw * px + 3) & ~3ull;
    if (off > n || stride * uh > n - off) return D3DXERR_INVALIDDATA;
    im->iff = IFF_BMP, im->rtype = RT_TEXTURE, im->w = uw, im->h = uh, im->d = 1, im->levels = 1, im->faces = 1;
    im->fmt = bpp == 24 ? FMT_R8G8B8 : D3DFMT_X8R8G8B8;
    if (!layout(im)) return D3DXERR_INVALIDDATA;
    for (uint32_t y = 0; y < uh; y++) memcpy(im->data + y * uw * px, buf + off + (top ? y : uh - 1 - y) * stride, uw * px);
    return 0;
}

static uint32_t decode_imageio(const uint8_t *buf, uint32_t n, uint32_t iff, struct d3dx_image *im) {
    uint32_t w, h;
    int alpha;
    uint8_t *argb;
    if (!d3dx_imageio_decode(buf, n, &w, &h, &alpha, &argb)) return D3DXERR_INVALIDDATA;
    im->iff = iff, im->rtype = RT_TEXTURE, im->w = w, im->h = h, im->d = 1, im->levels = 1, im->faces = 1;
    im->fmt = alpha ? D3DFMT_A8R8G8B8 : D3DFMT_X8R8G8B8;
    im->data = argb, im->size = w * h * 4;
    im->off = calloc(1, 4);
    return 0;
}

uint32_t d3dx_image_decode(const uint8_t *b, uint32_t n, struct d3dx_image *im) {
    memset(im, 0, sizeof *im);
    if (n >= 4 && !memcmp(b, "DDS ", 4)) return decode_dds(b, n, im);
    if (n >= 8 && !memcmp(b, "\x89PNG", 4)) return decode_imageio(b, n, IFF_PNG, im);
    if (n >= 3 && b[0] == 0xFF && b[1] == 0xD8 && b[2] == 0xFF) return decode_imageio(b, n, IFF_JPG, im);
    if (n >= 2 && b[0] == 'B' && b[1] == 'M') {
        uint32_t r = decode_bmp(b, n, im);
        return r == 1 ? decode_imageio(b, n, IFF_BMP, im) : r;
    }
    return decode_tga(b, n, im);                           /* TGA has no magic: last */
}

/* ---- writers ---- */
static uint8_t q8(float f) { f = f < 0 ? 0 : f > 1 ? 1 : f; return (uint8_t)(f * 255.0f + 0.5f); }
static void w32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v, p[1] = (uint8_t)(v >> 8), p[2] = (uint8_t)(v >> 16), p[3] = (uint8_t)(v >> 24); }
static void w16(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v, p[1] = (uint8_t)(v >> 8); }

uint8_t *d3dx_encode_bmp(const float *c, uint32_t w, uint32_t h, uint32_t *n) {
    uint32_t stride = (w * 3 + 3) & ~3u, size = 54 + stride * h;
    uint8_t *o = calloc(1, size);
    o[0] = 'B', o[1] = 'M', w32(o + 2, size), w32(o + 10, 54), w32(o + 14, 40), w32(o + 18, w), w32(o + 22, h);
    w16(o + 26, 1), w16(o + 28, 24), w32(o + 34, stride * h), w32(o + 38, 2835), w32(o + 42, 2835);
    for (uint32_t y = 0; y < h; y++)
        for (uint32_t x = 0; x < w; x++) {
            const float *p = c + 4 * ((h - 1 - y) * w + x);
            uint8_t *d = o + 54 + y * stride + 3 * x;
            d[0] = q8(p[2]), d[1] = q8(p[1]), d[2] = q8(p[0]);
        }
    *n = size;
    return o;
}
uint8_t *d3dx_encode_tga(const float *c, uint32_t w, uint32_t h, uint32_t *n) {
    uint32_t size = 18 + w * h * 4;
    uint8_t *o = calloc(1, size);
    o[2] = 2, w16(o + 12, w), w16(o + 14, h), o[16] = 32, o[17] = 8 | 0x20;   /* 32-bit, top-left origin */
    for (uint32_t i = 0; i < w * h; i++) {
        uint8_t *d = o + 18 + 4 * i;
        d[0] = q8(c[4 * i + 2]), d[1] = q8(c[4 * i + 1]), d[2] = q8(c[4 * i]), d[3] = q8(c[4 * i + 3]);
    }
    *n = size;
    return o;
}
uint8_t *d3dx_encode_dds(uint32_t fmt, uint32_t w, uint32_t h, uint32_t levels, uint32_t faces,
                         const uint8_t *const *subs, uint32_t *n) {
    uint32_t data = 0;
    for (uint32_t f = 0; f < faces; f++)
        for (uint32_t l = 0; l < levels; l++) data += d3dx_pitch(fmt, dim(w, l)) * d3dx_rows(fmt, dim(h, l));
    uint8_t *o = calloc(1, 128 + data), *hd = o + 4;
    memcpy(o, "DDS ", 4);
    w32(hd, 124), w32(hd + 4, 0x1007 | (levels > 1 ? 0x20000 : 0) | (d3dx_block(fmt) == 4 ? 0x80000 : 0x8));
    w32(hd + 8, h), w32(hd + 12, w), w32(hd + 16, d3dx_block(fmt) == 4 ? d3dx_pitch(fmt, w) * d3dx_rows(fmt, h) : d3dx_pitch(fmt, w));
    w32(hd + 24, levels);
    uint8_t *pf = hd + 72;
    w32(pf, 32);
    switch (fmt) {
    case D3DFMT_A8R8G8B8: w32(pf + 4, 0x41), w32(pf + 12, 32), w32(pf + 16, 0xFF0000), w32(pf + 20, 0xFF00), w32(pf + 24, 0xFF), w32(pf + 28, 0xFF000000u); break;
    case D3DFMT_X8R8G8B8: w32(pf + 4, 0x40), w32(pf + 12, 32), w32(pf + 16, 0xFF0000), w32(pf + 20, 0xFF00), w32(pf + 24, 0xFF); break;
    case D3DFMT_R5G6B5: w32(pf + 4, 0x40), w32(pf + 12, 16), w32(pf + 16, 0xF800), w32(pf + 20, 0x7E0), w32(pf + 24, 0x1F); break;
    case D3DFMT_A1R5G5B5: w32(pf + 4, 0x41), w32(pf + 12, 16), w32(pf + 16, 0x7C00), w32(pf + 20, 0x3E0), w32(pf + 24, 0x1F), w32(pf + 28, 0x8000); break;
    case D3DFMT_A4R4G4B4: w32(pf + 4, 0x41), w32(pf + 12, 16), w32(pf + 16, 0xF00), w32(pf + 20, 0xF0), w32(pf + 24, 0xF), w32(pf + 28, 0xF000); break;
    case D3DFMT_L8: w32(pf + 4, 0x20000), w32(pf + 12, 8), w32(pf + 16, 0xFF); break;
    case D3DFMT_A8: w32(pf + 4, 0x2), w32(pf + 12, 8), w32(pf + 28, 0xFF); break;
    default: w32(pf + 4, 0x4), w32(pf + 8, fmt); break;    /* DXTn and the rest by FOURCC / D3DFORMAT number */
    }
    w32(hd + 104, 0x1000 | (levels > 1 ? 0x400008 : 0) | (faces == 6 ? 8 : 0));
    w32(hd + 108, faces == 6 ? 0xFE00 : 0);
    uint32_t off = 128;
    for (uint32_t f = 0; f < faces; f++)
        for (uint32_t l = 0; l < levels; l++) {
            uint32_t s = d3dx_pitch(fmt, dim(w, l)) * d3dx_rows(fmt, dim(h, l));
            memcpy(o + off, subs[f * levels + l], s);
            off += s;
        }
    *n = 128 + data;
    return o;
}
