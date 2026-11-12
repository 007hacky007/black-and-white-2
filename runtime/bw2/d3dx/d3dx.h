/*
 * d3dx.h - D3DX's texture side (plan 4 Task 9): images in files, pixels in
 * every format the device has, and ID3DXBuffer.
 */
#ifndef D3DX_H
#define D3DX_H
#include <stdint.h>

#define D3DX_DEFAULT          0xFFFFFFFFu
#define D3DX_DEFAULT_NONPOW2  0xFFFFFFFEu
#define D3DX_FROM_FILE        0xFFFFFFFDu
#define D3DFMT_FROM_FILE      0xFFFFFFFDu
#define D3DXERR_INVALIDDATA   0x88760B59u
#define D3DX_FILTER_NONE      1u
#define D3DX_FILTER_POINT     2u
#define D3DX_FILTER_LINEAR    3u
#define D3DX_FILTER_TRIANGLE  4u
#define D3DX_FILTER_BOX       5u
#define D3DX_FILTER_MIRROR_U  0x10000u
#define D3DX_FILTER_MIRROR_V  0x20000u
#define D3DX_FILTER_SRGB_IN   0x200000u
#define D3DX_FILTER_SRGB_OUT  0x400000u
enum { IFF_BMP, IFF_JPG, IFF_TGA, IFF_PNG, IFF_DDS, IFF_PPM, IFF_DIB, IFF_HDR, IFF_PFM };

/* d3dx_buffer.c */
uint32_t d3dx_buffer_new(uint32_t size);              /* an ID3DXBuffer of size zeroed bytes */
uint32_t d3dx_buffer_ptr(uint32_t buf);

/* d3dx_pixel.c: pixels as float RGBA. Formats are D3DFORMATs (including R8G8B8 and DXT1-5 to read). */
int d3dx_can_read(uint32_t fmt);
int d3dx_can_write(uint32_t fmt);
uint32_t d3dx_bytes(uint32_t fmt);                     /* per pixel, or per 4x4 block */
int d3dx_block(uint32_t fmt);                          /* 4 for DXT */
uint32_t d3dx_pitch(uint32_t fmt, uint32_t w);
uint32_t d3dx_rows(uint32_t fmt, uint32_t h);
/* Read a w x h image (D3D layout) into w*h float4s; write one back. */
void d3dx_read(uint32_t fmt, const uint8_t *src, uint32_t pitch, uint32_t w, uint32_t h, float *out);
void d3dx_write(uint32_t fmt, const float *in, uint32_t w, uint32_t h, uint8_t *dst, uint32_t pitch);

/* d3dx_image.c: a decoded file, its data in its own format, D3D layout */
struct d3dx_image {
    uint32_t iff, rtype, w, h, d, levels, faces, fmt;
    uint8_t *data;
    uint32_t size;
    uint32_t *off;                                     /* per subresource: face * levels + level */
};
uint32_t d3dx_image_decode(const uint8_t *buf, uint32_t n, struct d3dx_image *img);   /* 0 or an HRESULT */
void d3dx_image_free(struct d3dx_image *img);
uint32_t d3dx_image_level_w(const struct d3dx_image *img, uint32_t l);
uint32_t d3dx_image_level_h(const struct d3dx_image *img, uint32_t l);
uint32_t d3dx_image_level_d(const struct d3dx_image *img, uint32_t l);
/* Encoders: malloc'd file bytes. rgba is w*h float4s; dds takes raw subresources. */
uint8_t *d3dx_encode_bmp(const float *rgba, uint32_t w, uint32_t h, uint32_t *n);
uint8_t *d3dx_encode_tga(const float *rgba, uint32_t w, uint32_t h, uint32_t *n);
uint8_t *d3dx_encode_dds(uint32_t fmt, uint32_t w, uint32_t h, uint32_t levels, uint32_t faces,
                         const uint8_t *const *subs, uint32_t *n);

/* d3dx_lifted.c: the disc's d3dx9_25.dll, lifted (plan 9). An export's guest address; the first call
 * loads the DLL from the engine binary's folder, and a missing or wrong file stops loudly. */
uint32_t d3dx_lifted_proc(const char *export_name);

/* d3dx_imageio.m: PNG and JPEG (and other BMPs) through ImageIO */
int d3dx_imageio_decode(const uint8_t *buf, uint32_t n, uint32_t *w, uint32_t *h, int *has_alpha, uint8_t **argb);
uint8_t *d3dx_imageio_encode(const float *rgba, uint32_t w, uint32_t h, int jpeg, uint32_t *n);

#endif
