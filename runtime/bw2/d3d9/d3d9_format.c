/*
 * d3d9_format.c - the format table of d3d9_format.h.
 */
#include "d3d9_format.h"
#include "d3d_com.h"

#define TEXALL (FU_TEX | FU_CUBE | FU_VOL | FU_FILTER)
#define COLRT  (FU_RT | FU_BLEND | FU_AUTOGEN)
static const struct d3d_format g_formats[] = {
    { D3DFMT_A8R8G8B8, ST_BGRA8, "", 4, 1, CV_NONE, TEXALL | COLRT | FU_SRGB, "A8R8G8B8" },
    { D3DFMT_X8R8G8B8, ST_BGRA8, "rgb1", 4, 1, CV_NONE, TEXALL | COLRT | FU_SRGB, "X8R8G8B8" },
    { D3DFMT_A8B8G8R8, ST_RGBA8, "", 4, 1, CV_NONE, TEXALL | FU_AUTOGEN | FU_SRGB, "A8B8G8R8" },
    { D3DFMT_X8B8G8R8, ST_RGBA8, "rgb1", 4, 1, CV_NONE, TEXALL | FU_AUTOGEN | FU_SRGB, "X8B8G8R8" },
    { D3DFMT_R5G6B5, ST_BGRA8, "rgb1", 2, 1, CV_565, TEXALL | COLRT, "R5G6B5" },
    { D3DFMT_X1R5G5B5, ST_BGRA8, "rgb1", 2, 1, CV_X1555, TEXALL | FU_AUTOGEN, "X1R5G5B5" },
    { D3DFMT_A1R5G5B5, ST_BGRA8, "", 2, 1, CV_A1555, TEXALL | FU_AUTOGEN, "A1R5G5B5" },
    { D3DFMT_A4R4G4B4, ST_BGRA8, "", 2, 1, CV_A4444, TEXALL | FU_AUTOGEN, "A4R4G4B4" },
    { D3DFMT_X4R4G4B4, ST_BGRA8, "rgb1", 2, 1, CV_X4444, TEXALL | FU_AUTOGEN, "X4R4G4B4" },
    { D3DFMT_L8, ST_R8, "rrr1", 1, 1, CV_NONE, TEXALL | FU_AUTOGEN, "L8" },
    { D3DFMT_A8, ST_A8, "", 1, 1, CV_NONE, TEXALL, "A8" },
    { D3DFMT_A8L8, ST_RG8, "rrrg", 2, 1, CV_NONE, TEXALL | FU_AUTOGEN, "A8L8" },
    { D3DFMT_L16, ST_R16, "rrr1", 2, 1, CV_NONE, TEXALL | FU_AUTOGEN, "L16" },
    { D3DFMT_V8U8, ST_RG8S, "rg11", 2, 1, CV_NONE, TEXALL | FU_BUMP, "V8U8" },
    { D3DFMT_Q8W8V8U8, ST_RGBA8S, "", 4, 1, CV_NONE, TEXALL | FU_BUMP, "Q8W8V8U8" },
    { D3DFMT_G16R16, ST_RG16, "rg11", 4, 1, CV_NONE, TEXALL | FU_AUTOGEN, "G16R16" },
    { D3DFMT_A16B16G16R16, ST_RGBA16, "", 8, 1, CV_NONE, TEXALL | FU_AUTOGEN, "A16B16G16R16" },
    { D3DFMT_R16F, ST_R16F, "r111", 2, 1, CV_NONE, TEXALL | COLRT, "R16F" },
    { D3DFMT_G16R16F, ST_RG16F, "rg11", 4, 1, CV_NONE, TEXALL | COLRT, "G16R16F" },
    { D3DFMT_A16B16G16R16F, ST_RGBA16F, "", 8, 1, CV_NONE, TEXALL | COLRT, "A16B16G16R16F" },
    { D3DFMT_R32F, ST_R32F, "r111", 4, 1, CV_NONE, TEXALL | COLRT | FU_VTX, "R32F" },
    { D3DFMT_G32R32F, ST_RG32F, "rg11", 8, 1, CV_NONE, TEXALL, "G32R32F" },
    { D3DFMT_A32B32G32R32F, ST_RGBA32F, "", 16, 1, CV_NONE, TEXALL | COLRT | FU_VTX, "A32B32G32R32F" },
    { D3DFMT_DXT1, ST_BC1, "", 8, 4, CV_NONE, TEXALL | FU_SRGB, "DXT1" },
    { D3DFMT_DXT2, ST_BC2, "", 16, 4, CV_NONE, TEXALL, "DXT2" },
    { D3DFMT_DXT3, ST_BC2, "", 16, 4, CV_NONE, TEXALL | FU_SRGB, "DXT3" },
    { D3DFMT_DXT4, ST_BC3, "", 16, 4, CV_NONE, TEXALL, "DXT4" },
    { D3DFMT_DXT5, ST_BC3, "", 16, 4, CV_NONE, TEXALL | FU_SRGB, "DXT5" },
    { D3DFMT_D16, ST_D16, "", 2, 1, CV_NONE, FU_DS, "D16" },
    { D3DFMT_D24S8, ST_D32S8, "", 4, 1, CV_NONE, FU_DS, "D24S8" },
    { D3DFMT_D24X8, ST_D32S8, "", 4, 1, CV_NONE, FU_DS, "D24X8" },
    { D3DFMT_D24X4S4, ST_D32S8, "", 4, 1, CV_NONE, FU_DS, "D24X4S4" },
    { D3DFMT_D32, ST_D32, "", 4, 1, CV_NONE, FU_DS, "D32" },
};

const struct d3d_format *d3d_format(uint32_t d3dfmt) {
    for (unsigned i = 0; i < sizeof g_formats / sizeof *g_formats; i++)
        if (g_formats[i].d3d == d3dfmt) return &g_formats[i];
    return 0;
}
int d3d_format_is_depth(const struct d3d_format *f) { return f && (f->use & FU_DS); }

uint32_t d3d_format_pitch(const struct d3d_format *f, uint32_t width) {
    if (f->block == 4) return ((width + 3) / 4) * f->bytes;
    return width * f->bytes;
}
uint32_t d3d_format_rows(const struct d3d_format *f, uint32_t height) {
    return f->block == 4 ? (height + 3) / 4 : height;
}

uint32_t d3d_check_format(uint32_t adapter_fmt, uint32_t usage, uint32_t rtype, uint32_t fmt) {
    if (adapter_fmt != D3DFMT_X8R8G8B8 && adapter_fmt != D3DFMT_R5G6B5) return D3DERR_NOTAVAILABLE;
    if (rtype == D3DRTYPE_VERTEXBUFFER) return fmt == D3DFMT_VERTEXDATA ? D3D_OK : D3DERR_NOTAVAILABLE;
    if (rtype == D3DRTYPE_INDEXBUFFER)
        return fmt == D3DFMT_INDEX16 || fmt == D3DFMT_INDEX32 ? D3D_OK : D3DERR_NOTAVAILABLE;
    const struct d3d_format *f = d3d_format(fmt);
    if (!f) return D3DERR_NOTAVAILABLE;
    uint32_t need = 0;
    switch (rtype) {
    case D3DRTYPE_SURFACE:
        if (usage & D3DUSAGE_DEPTHSTENCIL) need |= FU_DS;
        else if (f->use & FU_DS) return D3DERR_NOTAVAILABLE;
        break;
    case D3DRTYPE_TEXTURE: need |= FU_TEX; break;
    case D3DRTYPE_CUBETEXTURE: need |= FU_CUBE; break;
    case D3DRTYPE_VOLUMETEXTURE: case D3DRTYPE_VOLUME:
        need |= FU_VOL;
        if (usage & (D3DUSAGE_RENDERTARGET | D3DUSAGE_DEPTHSTENCIL)) return D3DERR_NOTAVAILABLE;
        break;
    default: return D3DERR_NOTAVAILABLE;
    }
    if (rtype != D3DRTYPE_SURFACE && (usage & D3DUSAGE_DEPTHSTENCIL)) return D3DERR_NOTAVAILABLE;
    if (usage & D3DUSAGE_RENDERTARGET) need |= FU_RT;
    if (usage & D3DUSAGE_QUERY_VERTEXTEXTURE) need |= FU_VTX;
    if (usage & D3DUSAGE_QUERY_FILTER) need |= FU_FILTER;
    if (usage & D3DUSAGE_QUERY_POSTPIXELSHADER_BLENDING) need |= FU_BLEND;
    if (usage & D3DUSAGE_QUERY_SRGBREAD) need |= FU_SRGB;
    if (usage & D3DUSAGE_QUERY_SRGBWRITE) need |= FU_SRGB | FU_RT;
    if (usage & D3DUSAGE_QUERY_LEGACYBUMPMAP) need |= FU_BUMP;
    if (usage & D3DUSAGE_DMAP) return D3DERR_NOTAVAILABLE;
    if ((f->use & need) != need) return D3DERR_NOTAVAILABLE;
    if ((usage & D3DUSAGE_AUTOGENMIPMAP) && !(f->use & FU_AUTOGEN)) return D3DOK_NOAUTOGEN;
    return D3D_OK;
}
