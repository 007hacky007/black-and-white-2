/*
 * d3d9_convert.c - between a format's D3D layout (what the guest locks) and
 * its GPU storage (d3d9_format.c): the 16-bit colour formats live as BGRA8 on
 * the GPU; everything else is stored as laid out.
 */
#include <string.h>
#include "d3d9_device.h"

uint32_t d3d_storage_pitch(const struct d3d_format *f, uint32_t w) {
    return f->conv != CV_NONE ? w * 4 : d3d_format_pitch(f, w);
}

static inline uint8_t x5(uint32_t v) { return (uint8_t)((v << 3) | (v >> 2)); }
static inline uint8_t x6(uint32_t v) { return (uint8_t)((v << 2) | (v >> 4)); }

void d3d_to_storage(const struct d3d_format *f, const uint8_t *src, uint32_t spitch, uint8_t *dst, uint32_t dpitch,
                    uint32_t w, uint32_t rows) {
    if (f->conv == CV_NONE) {
        uint32_t n = d3d_format_pitch(f, w);
        for (uint32_t y = 0; y < rows; y++) memcpy(dst + y * dpitch, src + y * spitch, n);
        return;
    }
    for (uint32_t y = 0; y < rows; y++) {
        const uint16_t *s = (const uint16_t *)(src + y * spitch);
        uint8_t *d = dst + y * dpitch;
        for (uint32_t x = 0; x < w; x++, d += 4) {
            uint32_t v = s[x];
            switch (f->conv) {
            case CV_565: d[2] = x5(v >> 11), d[1] = x6((v >> 5) & 63), d[0] = x5(v & 31), d[3] = 255; break;
            case CV_X1555: case CV_A1555:
                d[2] = x5((v >> 10) & 31), d[1] = x5((v >> 5) & 31), d[0] = x5(v & 31);
                d[3] = f->conv == CV_X1555 || (v & 0x8000) ? 255 : 0;
                break;
            case CV_A4444: case CV_X4444:
                d[2] = (uint8_t)(((v >> 8) & 15) * 17), d[1] = (uint8_t)(((v >> 4) & 15) * 17);
                d[0] = (uint8_t)((v & 15) * 17), d[3] = f->conv == CV_X4444 ? 255 : (uint8_t)((v >> 12) * 17);
                break;
            default: break;
            }
        }
    }
}

void d3d_from_storage(const struct d3d_format *f, const uint8_t *src, uint32_t spitch, uint8_t *dst, uint32_t dpitch,
                      uint32_t w, uint32_t rows) {
    if (f->conv == CV_NONE) {
        uint32_t n = d3d_format_pitch(f, w);
        for (uint32_t y = 0; y < rows; y++) {
            memcpy(dst + y * dpitch, src + y * spitch, n);
            if (f->d3d == D3DFMT_X8R8G8B8 || f->d3d == D3DFMT_X8B8G8R8)
                for (uint32_t x = 0; x < w; x++) dst[y * dpitch + 4 * x + 3] = 0xFF;   /* X reads as opaque */
        }
        return;
    }
    for (uint32_t y = 0; y < rows; y++) {
        const uint8_t *s = src + y * spitch;
        uint16_t *d = (uint16_t *)(dst + y * dpitch);
        for (uint32_t x = 0; x < w; x++, s += 4) {
            uint32_t r = s[2], g = s[1], b = s[0], a = s[3];
            switch (f->conv) {
            case CV_565: d[x] = (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)); break;
            case CV_X1555: d[x] = (uint16_t)(0x8000 | ((r >> 3) << 10) | ((g >> 3) << 5) | (b >> 3)); break;
            case CV_A1555: d[x] = (uint16_t)(((a >> 7) << 15) | ((r >> 3) << 10) | ((g >> 3) << 5) | (b >> 3)); break;
            case CV_A4444: d[x] = (uint16_t)(((a >> 4) << 12) | ((r >> 4) << 8) | ((g >> 4) << 4) | (b >> 4)); break;
            case CV_X4444: d[x] = (uint16_t)(0xF000 | ((r >> 4) << 8) | ((g >> 4) << 4) | (b >> 4)); break;
            default: break;
            }
        }
    }
}
