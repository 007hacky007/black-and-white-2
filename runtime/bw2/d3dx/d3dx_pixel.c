/*
 * d3dx_pixel.c - pixels in D3D formats as float RGBA (plan 4 Task 9). Every
 * format of the device's table is read and written, plus R8G8B8 (files);
 * DXT1-5 are read (decoded), and DXT1, DXT3 and DXT5 written through
 * d3dx_dxt.c (plan 10 Task 3; DXT2 and DXT4 stop loudly there).
 * Channels a format lacks read as D3D gives them (0 for colour, 1 for alpha
 * and the missing ones of L, R and RG formats).
 */
#include <math.h>
#include <string.h>
#include "d3d9_format.h"
#include "d3dx.h"

#define FMT_R8G8B8 20u

int d3dx_block(uint32_t fmt) { return fmt >= D3DFMT_DXT1 && fmt <= D3DFMT_DXT5 ? 4 : 1; }
uint32_t d3dx_bytes(uint32_t fmt) {
    if (fmt == FMT_R8G8B8) return 3;
    const struct d3d_format *f = d3d_format(fmt);
    return f ? f->bytes : 0;
}
int d3dx_can_read(uint32_t fmt) {
    const struct d3d_format *f = d3d_format(fmt);
    return fmt == FMT_R8G8B8 || (f && !(f->use & FU_DS));
}
int d3dx_can_write(uint32_t fmt) { return d3dx_can_read(fmt); }   /* DXT2 and DXT4 reach the encoder's stop */
uint32_t d3dx_pitch(uint32_t fmt, uint32_t w) {
    return d3dx_block(fmt) == 4 ? (w + 3) / 4 * d3dx_bytes(fmt) : w * d3dx_bytes(fmt);
}
uint32_t d3dx_rows(uint32_t fmt, uint32_t h) { return d3dx_block(fmt) == 4 ? (h + 3) / 4 : h; }

static float half_f(uint16_t h) {
    uint32_t s = (uint32_t)(h & 0x8000) << 16, e = (h >> 10) & 0x1F, m = h & 0x3FF, u;
    if (!e) { float f = ldexpf((float)m, -24); memcpy(&u, &f, 4); u |= s; }
    else if (e == 31) u = s | 0x7F800000u | (m << 13);
    else u = s | ((e + 112) << 23) | (m << 13);
    float f;
    memcpy(&f, &u, 4);
    return f;
}
static uint16_t f_half(float f) {
    uint32_t u;
    memcpy(&u, &f, 4);
    uint32_t s = (u >> 16) & 0x8000;
    int e = (int)((u >> 23) & 0xFF) - 112;
    uint32_t m = u & 0x7FFFFF;
    if (((u >> 23) & 0xFF) == 0xFF) return (uint16_t)(s | 0x7C00 | (m ? 0x200 : 0));
    if (e >= 31) return (uint16_t)(s | 0x7C00);
    if (e <= 0) {
        if (e < -10) return (uint16_t)s;
        m |= 0x800000;
        uint32_t sh = (uint32_t)(14 - e), hm = m >> sh;
        if ((m >> (sh - 1)) & 1) hm++;
        return (uint16_t)(s | hm);
    }
    uint32_t h = s | ((uint32_t)e << 10) | (m >> 13);
    if (m & 0x1000) h++;
    return (uint16_t)h;
}

/* 4-, 5- and 6-bit channels widen by bit replication to 8 bits, as the GPU path does (d3d9_convert.c);
 * 16-bit channels scale. */
static float un(uint32_t v, uint32_t bits) {
    if (bits == 5) return (float)((v << 3) | (v >> 2)) / 255.0f;
    if (bits == 6) return (float)((v << 2) | (v >> 4)) / 255.0f;
    if (bits == 4) return (float)(v * 17) / 255.0f;
    return (float)v / (float)((1u << bits) - 1);
}
static uint32_t nu(float f, uint32_t bits) {
    float m = (float)((1u << bits) - 1);
    f = f < 0 ? 0 : f > 1 ? 1 : f;
    return (uint32_t)(f * m + 0.5f);
}
static float sn8(uint8_t v) { float f = (float)(int8_t)v / 127.0f; return f < -1 ? -1 : f; }
static uint8_t ns8(float f) { f = f < -1 ? -1 : f > 1 ? 1 : f; return (uint8_t)(int8_t)lrintf(f * 127.0f); }

static void rgb565(uint16_t c, float *o) { o[0] = un(c >> 11, 5), o[1] = un((c >> 5) & 63, 6), o[2] = un(c & 31, 5); }

/* One 4x4 BC block: 16 float4s, row-major. */
static void dxt_block(uint32_t fmt, const uint8_t *b, float *out) {
    const uint8_t *cb = fmt == D3DFMT_DXT1 ? b : b + 8;
    uint16_t c0 = (uint16_t)(cb[0] | cb[1] << 8), c1 = (uint16_t)(cb[2] | cb[3] << 8);
    float pal[4][4];
    rgb565(c0, pal[0]), rgb565(c1, pal[1]);
    pal[0][3] = pal[1][3] = 1;
    if (c0 > c1 || fmt != D3DFMT_DXT1) {
        for (int k = 0; k < 3; k++) pal[2][k] = (2 * pal[0][k] + pal[1][k]) / 3, pal[3][k] = (pal[0][k] + 2 * pal[1][k]) / 3;
        pal[2][3] = pal[3][3] = 1;
    } else {
        for (int k = 0; k < 3; k++) pal[2][k] = (pal[0][k] + pal[1][k]) / 2, pal[3][k] = 0;
        pal[2][3] = 1, pal[3][3] = 0;
    }
    uint32_t idx = cb[4] | cb[5] << 8 | cb[6] << 16 | (uint32_t)cb[7] << 24;
    for (int i = 0; i < 16; i++) memcpy(out + 4 * i, pal[(idx >> (2 * i)) & 3], 16);
    if (fmt == D3DFMT_DXT2 || fmt == D3DFMT_DXT3)
        for (int i = 0; i < 16; i++) out[4 * i + 3] = un((b[i / 2] >> (4 * (i & 1))) & 15, 4);
    else if (fmt == D3DFMT_DXT4 || fmt == D3DFMT_DXT5) {
        float a[8], a0 = b[0] / 255.0f, a1 = b[1] / 255.0f;
        a[0] = a0, a[1] = a1;
        if (b[0] > b[1]) for (int k = 1; k < 7; k++) a[k + 1] = ((7 - k) * a0 + k * a1) / 7;
        else {
            for (int k = 1; k < 5; k++) a[k + 1] = ((5 - k) * a0 + k * a1) / 5;
            a[6] = 0, a[7] = 1;
        }
        uint64_t bits = 0;
        for (int k = 0; k < 6; k++) bits |= (uint64_t)b[2 + k] << (8 * k);
        for (int i = 0; i < 16; i++) out[4 * i + 3] = a[(bits >> (3 * i)) & 7];
    }
}

static void read_px(uint32_t fmt, const uint8_t *p, float *o) {
    uint32_t v;
    o[0] = o[1] = o[2] = 0, o[3] = 1;
    switch (fmt) {
    case D3DFMT_A8R8G8B8: o[3] = p[3] / 255.0f; /* fall through */
    case D3DFMT_X8R8G8B8: o[0] = p[2] / 255.0f, o[1] = p[1] / 255.0f, o[2] = p[0] / 255.0f; break;
    case FMT_R8G8B8: o[0] = p[2] / 255.0f, o[1] = p[1] / 255.0f, o[2] = p[0] / 255.0f; break;
    case D3DFMT_A8B8G8R8: o[3] = p[3] / 255.0f; /* fall through */
    case D3DFMT_X8B8G8R8: o[0] = p[0] / 255.0f, o[1] = p[1] / 255.0f, o[2] = p[2] / 255.0f; break;
    case D3DFMT_R5G6B5: rgb565((uint16_t)(p[0] | p[1] << 8), o); break;
    case D3DFMT_A1R5G5B5: case D3DFMT_X1R5G5B5:
        v = p[0] | p[1] << 8;
        o[0] = un((v >> 10) & 31, 5), o[1] = un((v >> 5) & 31, 5), o[2] = un(v & 31, 5);
        if (fmt == D3DFMT_A1R5G5B5) o[3] = (float)(v >> 15);
        break;
    case D3DFMT_A4R4G4B4: case D3DFMT_X4R4G4B4:
        v = p[0] | p[1] << 8;
        o[0] = un((v >> 8) & 15, 4), o[1] = un((v >> 4) & 15, 4), o[2] = un(v & 15, 4);
        if (fmt == D3DFMT_A4R4G4B4) o[3] = un(v >> 12, 4);
        break;
    case D3DFMT_L8: o[0] = o[1] = o[2] = p[0] / 255.0f; break;
    case D3DFMT_A8: o[3] = p[0] / 255.0f; break;
    case D3DFMT_A8L8: o[0] = o[1] = o[2] = p[0] / 255.0f, o[3] = p[1] / 255.0f; break;
    case D3DFMT_L16: o[0] = o[1] = o[2] = un((uint32_t)(p[0] | p[1] << 8), 16); break;
    case D3DFMT_V8U8: o[0] = sn8(p[0]), o[1] = sn8(p[1]), o[2] = 1; break;
    case D3DFMT_Q8W8V8U8: o[0] = sn8(p[0]), o[1] = sn8(p[1]), o[2] = sn8(p[2]), o[3] = sn8(p[3]); break;
    case D3DFMT_G16R16: o[0] = un((uint32_t)(p[0] | p[1] << 8), 16), o[1] = un((uint32_t)(p[2] | p[3] << 8), 16), o[2] = 1; break;
    case D3DFMT_A16B16G16R16:
        for (int i = 0; i < 4; i++) o[i] = un((uint32_t)(p[2 * i] | p[2 * i + 1] << 8), 16);
        break;
    case D3DFMT_R16F: o[0] = half_f((uint16_t)(p[0] | p[1] << 8)), o[1] = o[2] = 1; break;
    case D3DFMT_G16R16F: o[0] = half_f((uint16_t)(p[0] | p[1] << 8)), o[1] = half_f((uint16_t)(p[2] | p[3] << 8)), o[2] = 1; break;
    case D3DFMT_A16B16G16R16F: for (int i = 0; i < 4; i++) o[i] = half_f((uint16_t)(p[2 * i] | p[2 * i + 1] << 8)); break;
    case D3DFMT_R32F: memcpy(o, p, 4), o[1] = o[2] = 1; break;
    case D3DFMT_G32R32F: memcpy(o, p, 8), o[2] = 1; break;
    case D3DFMT_A32B32G32R32F: memcpy(o, p, 16); break;
    default: break;
    }
}

static void write_px(uint32_t fmt, const float *c, uint8_t *p) {
    uint32_t v;
    switch (fmt) {
    case D3DFMT_A8R8G8B8: case D3DFMT_X8R8G8B8:
        p[2] = (uint8_t)nu(c[0], 8), p[1] = (uint8_t)nu(c[1], 8), p[0] = (uint8_t)nu(c[2], 8);
        p[3] = fmt == D3DFMT_A8R8G8B8 ? (uint8_t)nu(c[3], 8) : 0xFF;
        break;
    case FMT_R8G8B8: p[2] = (uint8_t)nu(c[0], 8), p[1] = (uint8_t)nu(c[1], 8), p[0] = (uint8_t)nu(c[2], 8); break;
    case D3DFMT_A8B8G8R8: case D3DFMT_X8B8G8R8:
        p[0] = (uint8_t)nu(c[0], 8), p[1] = (uint8_t)nu(c[1], 8), p[2] = (uint8_t)nu(c[2], 8);
        p[3] = fmt == D3DFMT_A8B8G8R8 ? (uint8_t)nu(c[3], 8) : 0xFF;
        break;
    case D3DFMT_R5G6B5: v = nu(c[0], 5) << 11 | nu(c[1], 6) << 5 | nu(c[2], 5); goto w16;
    case D3DFMT_A1R5G5B5: v = (c[3] >= 0.5f ? 0x8000u : 0) | nu(c[0], 5) << 10 | nu(c[1], 5) << 5 | nu(c[2], 5); goto w16;
    case D3DFMT_X1R5G5B5: v = 0x8000u | nu(c[0], 5) << 10 | nu(c[1], 5) << 5 | nu(c[2], 5); goto w16;
    case D3DFMT_A4R4G4B4: v = nu(c[3], 4) << 12 | nu(c[0], 4) << 8 | nu(c[1], 4) << 4 | nu(c[2], 4); goto w16;
    case D3DFMT_X4R4G4B4: v = 0xF000u | nu(c[0], 4) << 8 | nu(c[1], 4) << 4 | nu(c[2], 4); goto w16;
    case D3DFMT_L16: v = nu(0.2125f * c[0] + 0.7154f * c[1] + 0.0721f * c[2], 16); goto w16;
    case D3DFMT_R16F: v = f_half(c[0]); goto w16;
    w16: p[0] = (uint8_t)v, p[1] = (uint8_t)(v >> 8); break;
    case D3DFMT_L8: p[0] = (uint8_t)nu(0.2125f * c[0] + 0.7154f * c[1] + 0.0721f * c[2], 8); break;
    case D3DFMT_A8: p[0] = (uint8_t)nu(c[3], 8); break;
    case D3DFMT_A8L8: p[0] = (uint8_t)nu(0.2125f * c[0] + 0.7154f * c[1] + 0.0721f * c[2], 8), p[1] = (uint8_t)nu(c[3], 8); break;
    case D3DFMT_V8U8: p[0] = ns8(c[0]), p[1] = ns8(c[1]); break;
    case D3DFMT_Q8W8V8U8: for (int i = 0; i < 4; i++) p[i] = ns8(c[i]); break;
    case D3DFMT_G16R16:
        v = nu(c[0], 16), p[0] = (uint8_t)v, p[1] = (uint8_t)(v >> 8);
        v = nu(c[1], 16), p[2] = (uint8_t)v, p[3] = (uint8_t)(v >> 8);
        break;
    case D3DFMT_A16B16G16R16:
        for (int i = 0; i < 4; i++) v = nu(c[i], 16), p[2 * i] = (uint8_t)v, p[2 * i + 1] = (uint8_t)(v >> 8);
        break;
    case D3DFMT_G16R16F:
        for (int i = 0; i < 2; i++) v = f_half(c[i]), p[2 * i] = (uint8_t)v, p[2 * i + 1] = (uint8_t)(v >> 8);
        break;
    case D3DFMT_A16B16G16R16F:
        for (int i = 0; i < 4; i++) v = f_half(c[i]), p[2 * i] = (uint8_t)v, p[2 * i + 1] = (uint8_t)(v >> 8);
        break;
    case D3DFMT_R32F: memcpy(p, c, 4); break;
    case D3DFMT_G32R32F: memcpy(p, c, 8); break;
    case D3DFMT_A32B32G32R32F: memcpy(p, c, 16); break;
    default: break;
    }
}

void d3dx_read(uint32_t fmt, const uint8_t *src, uint32_t pitch, uint32_t w, uint32_t h, float *out) {
    if (d3dx_block(fmt) == 4) {
        float blk[64];
        for (uint32_t by = 0; by < (h + 3) / 4; by++)
            for (uint32_t bx = 0; bx < (w + 3) / 4; bx++) {
                dxt_block(fmt, src + by * pitch + bx * d3dx_bytes(fmt), blk);
                for (uint32_t y = 0; y < 4; y++)
                    for (uint32_t x = 0; x < 4; x++)
                        if (by * 4 + y < h && bx * 4 + x < w)
                            memcpy(out + 4 * ((by * 4 + y) * w + bx * 4 + x), blk + 4 * (y * 4 + x), 16);
            }
        return;
    }
    uint32_t bpp = d3dx_bytes(fmt);
    for (uint32_t y = 0; y < h; y++)
        for (uint32_t x = 0; x < w; x++) read_px(fmt, src + y * pitch + x * bpp, out + 4 * (y * w + x));
}
void d3dx_write(uint32_t fmt, const float *in, uint32_t w, uint32_t h, uint8_t *dst, uint32_t pitch) {
    uint32_t bpp = d3dx_bytes(fmt);
    if (d3dx_block(fmt) == 4) {                            /* a partial edge block repeats the last column and row */
        float blk[64];
        for (uint32_t by = 0; by < (h + 3) / 4; by++)
            for (uint32_t bx = 0; bx < (w + 3) / 4; bx++) {
                for (uint32_t y = 0; y < 4; y++)
                    for (uint32_t x = 0; x < 4; x++) {
                        uint32_t sx = bx * 4 + x < w ? bx * 4 + x : w - 1, sy = by * 4 + y < h ? by * 4 + y : h - 1;
                        memcpy(blk + 4 * (y * 4 + x), in + 4 * ((size_t)sy * w + sx), 16);
                    }
                d3dx_dxt_encode(fmt, blk, dst + by * pitch + bx * bpp);
            }
        return;
    }
    for (uint32_t y = 0; y < h; y++)
        for (uint32_t x = 0; x < w; x++) write_px(fmt, in + 4 * (y * w + x), dst + y * pitch + x * bpp);
}
