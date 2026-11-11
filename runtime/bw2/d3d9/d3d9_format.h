/*
 * d3d9_format.h - the D3D formats this device serves (plan 4 Task 3/5): the
 * Metal storage each one lives in, its swizzle, its block layout, whether the
 * CPU converts it on upload, and what it may be used for.
 */
#ifndef D3D9_FORMAT_H
#define D3D9_FORMAT_H
#include <stdint.h>

/* D3DFORMAT */
enum {
    D3DFMT_UNKNOWN = 0, D3DFMT_R8G8B8 = 20, D3DFMT_A8R8G8B8 = 21, D3DFMT_X8R8G8B8 = 22, D3DFMT_R5G6B5 = 23,
    D3DFMT_X1R5G5B5 = 24, D3DFMT_A1R5G5B5 = 25, D3DFMT_A4R4G4B4 = 26, D3DFMT_A8 = 28, D3DFMT_X4R4G4B4 = 30,
    D3DFMT_A8B8G8R8 = 32, D3DFMT_X8B8G8R8 = 33, D3DFMT_G16R16 = 34, D3DFMT_A16B16G16R16 = 36, D3DFMT_L8 = 50,
    D3DFMT_A8L8 = 51, D3DFMT_V8U8 = 60, D3DFMT_Q8W8V8U8 = 63, D3DFMT_D16_LOCKABLE = 70, D3DFMT_D32 = 71,
    D3DFMT_D24S8 = 75, D3DFMT_D24X8 = 77, D3DFMT_D24X4S4 = 79, D3DFMT_D16 = 80, D3DFMT_L16 = 81,
    D3DFMT_VERTEXDATA = 100, D3DFMT_INDEX16 = 101, D3DFMT_INDEX32 = 102, D3DFMT_R16F = 111, D3DFMT_G16R16F = 112,
    D3DFMT_A16B16G16R16F = 113, D3DFMT_R32F = 114, D3DFMT_G32R32F = 115, D3DFMT_A32B32G32R32F = 116,
    D3DFMT_DXT1 = 0x31545844, D3DFMT_DXT2 = 0x32545844, D3DFMT_DXT3 = 0x33545844, D3DFMT_DXT4 = 0x34545844,
    D3DFMT_DXT5 = 0x35545844,
};

/* D3DRESOURCETYPE */
enum { D3DRTYPE_SURFACE = 1, D3DRTYPE_VOLUME, D3DRTYPE_TEXTURE, D3DRTYPE_VOLUMETEXTURE, D3DRTYPE_CUBETEXTURE,
       D3DRTYPE_VERTEXBUFFER, D3DRTYPE_INDEXBUFFER };

/* D3DUSAGE */
#define D3DUSAGE_RENDERTARGET       0x1u
#define D3DUSAGE_DEPTHSTENCIL       0x2u
#define D3DUSAGE_WRITEONLY          0x8u
#define D3DUSAGE_DYNAMIC            0x200u
#define D3DUSAGE_AUTOGENMIPMAP      0x400u
#define D3DUSAGE_DMAP               0x4000u
#define D3DUSAGE_QUERY_LEGACYBUMPMAP 0x8000u
#define D3DUSAGE_QUERY_SRGBREAD     0x10000u
#define D3DUSAGE_QUERY_FILTER       0x20000u
#define D3DUSAGE_QUERY_SRGBWRITE    0x40000u
#define D3DUSAGE_QUERY_POSTPIXELSHADER_BLENDING 0x80000u
#define D3DUSAGE_QUERY_VERTEXTEXTURE 0x100000u
#define D3DUSAGE_QUERY_WRAPANDMIP   0x200000u

/* Where a format is stored on the GPU (mtl_resource.m maps these to MTLPixelFormat). */
enum mtl_store {
    ST_NONE, ST_BGRA8, ST_RGBA8, ST_R8, ST_A8, ST_RG8, ST_R16, ST_RG8S, ST_RGBA8S, ST_RG16, ST_RGBA16,
    ST_R16F, ST_RG16F, ST_RGBA16F, ST_R32F, ST_RG32F, ST_RGBA32F, ST_BC1, ST_BC2, ST_BC3,
    ST_D16, ST_D32S8, ST_D32,
};
/* CPU conversions on upload (and back on readback): 16-bit colour to BGRA8. */
enum fmt_conv { CV_NONE, CV_565, CV_X1555, CV_A1555, CV_A4444, CV_X4444 };

/* Usage bits of the table */
#define FU_TEX     0x001u
#define FU_CUBE    0x002u
#define FU_VOL     0x004u
#define FU_RT      0x008u
#define FU_DS      0x010u
#define FU_VTX     0x020u   /* vertex texture fetch */
#define FU_FILTER  0x040u
#define FU_BLEND   0x080u   /* post-pixel-shader blending */
#define FU_SRGB    0x100u
#define FU_AUTOGEN 0x200u
#define FU_BUMP    0x400u   /* legacy bump map */

struct d3d_format {
    uint32_t d3d;
    enum mtl_store store;
    char swizzle[5];        /* "rgba" order of the storage channels each D3D channel reads; "" = identity */
    uint8_t bytes;          /* per pixel, or per 4x4 block when block == 4 */
    uint8_t block;          /* 1, or 4 for DXT */
    enum fmt_conv conv;
    uint32_t use;
    const char *name;
};

const struct d3d_format *d3d_format(uint32_t d3dfmt);              /* NULL if not served */
/* IDirect3D9::CheckDeviceFormat's answer (D3D_OK, D3DOK_NOAUTOGEN or D3DERR_NOTAVAILABLE). */
uint32_t d3d_check_format(uint32_t adapter_fmt, uint32_t usage, uint32_t rtype, uint32_t fmt);
/* D3D's own layout of a level: bytes per row (of blocks) and rows (of blocks). */
uint32_t d3d_format_pitch(const struct d3d_format *f, uint32_t width);
uint32_t d3d_format_rows(const struct d3d_format *f, uint32_t height);
int d3d_format_is_depth(const struct d3d_format *f);

#endif
