/*
 * d3d9_adapter.c - Direct3DCreate9 and IDirect3D9 (plan 4 Task 3). One
 * adapter, the Mac's main display, an SM 3.0 device. CreateDevice is in
 * d3d9_device.c.
 */
#include <CoreGraphics/CoreGraphics.h>
#include <stdlib.h>
#include <string.h>
#include "bw2.h"
#include "d3d_com.h"
#include "d3d9_adapter.h"
#include "d3d9_format.h"

const uint8_t bw2_gpu_guid[16] = { 0xB2, 0xB2, 0xB2, 0xB2, 0x00, 0x00, 0x00, 0x40,
                                   0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01 };

void d3d_display_size(uint32_t *w, uint32_t *h) {
    if (bw2_headless()) { *w = 1920, *h = 1080; return; }
    CGDisplayModeRef m = CGDisplayCopyDisplayMode(CGMainDisplayID());
    *w = m ? (uint32_t)CGDisplayModeGetPixelWidth(m) : 1920;
    *h = m ? (uint32_t)CGDisplayModeGetPixelHeight(m) : 1080;
    if (m) CGDisplayModeRelease(m);
}

/* The full-screen device whose back buffer is the current mode, or none. */
static const void *g_fs_dev;
static uint32_t g_fs_w, g_fs_h;

void d3d_current_mode(uint32_t *w, uint32_t *h) {
    if (g_fs_dev) *w = g_fs_w, *h = g_fs_h;
    else d3d_display_size(w, h);
}
static void publish_mode(void) {
    uint32_t w, h;
    d3d_current_mode(&w, &h);
    hle_set_screen_size((int)w, (int)h);
}
void d3d_mode_device(const void *dev, int fullscreen, uint32_t w, uint32_t h, int gone) {
    if (!gone && fullscreen) g_fs_dev = dev, g_fs_w = w, g_fs_h = h;
    else if (g_fs_dev == dev) g_fs_dev = NULL;
    else return;                                 /* another device's windowed change: the mode stays */
    publish_mode();
}

/* The mode list: the standard sizes that fit, plus the display's own, sorted, no duplicates. */
static const uint16_t g_std[][2] = {
    { 640, 480 }, { 800, 600 }, { 1024, 768 }, { 1152, 864 }, { 1280, 720 }, { 1280, 800 }, { 1280, 960 },
    { 1280, 1024 }, { 1360, 768 }, { 1440, 900 }, { 1600, 900 }, { 1600, 1200 }, { 1680, 1050 },
    { 1920, 1080 }, { 1920, 1200 }, { 2560, 1440 }, { 2560, 1600 },
};
static uint32_t g_modes[sizeof g_std / sizeof *g_std + 1][2], g_nmodes;
static int cmp_mode(const void *a, const void *b) {
    const uint32_t *x = a, *y = b;
    return x[0] != y[0] ? (x[0] < y[0] ? -1 : 1) : (x[1] < y[1] ? -1 : x[1] > y[1]);
}
static void modes(void) {
    if (g_nmodes) return;
    uint32_t dw, dh, n = 0;
    d3d_display_size(&dw, &dh);
    for (unsigned i = 0; i < sizeof g_std / sizeof *g_std; i++)
        if (g_std[i][0] <= dw && g_std[i][1] <= dh) g_modes[n][0] = g_std[i][0], g_modes[n++][1] = g_std[i][1];
    g_modes[n][0] = dw, g_modes[n++][1] = dh;
    qsort(g_modes, n, sizeof g_modes[0], cmp_mode);
    uint32_t k = 0;
    for (uint32_t i = 0; i < n; i++)
        if (!k || cmp_mode(g_modes[i], g_modes[k - 1])) g_modes[k][0] = g_modes[i][0], g_modes[k++][1] = g_modes[i][1];
    g_nmodes = k;
}
static int display_format(uint32_t fmt) { return fmt == D3DFMT_X8R8G8B8 || fmt == D3DFMT_R5G6B5; }
uint32_t d3d_mode_count(uint32_t fmt) { modes(); return display_format(fmt) ? g_nmodes : 0; }
int d3d_mode(uint32_t fmt, uint32_t i, uint32_t *w, uint32_t *h) {
    if (i >= d3d_mode_count(fmt)) return 0;
    *w = g_modes[i][0], *h = g_modes[i][1];
    return 1;
}

static void putf(uint32_t va, float f) { memcpy(hle_ptr(va), &f, 4); }

void d3d_fill_caps(uint32_t c) {
    memset(hle_ptr(c), 0, 304);
    MEM32(c + 0x00) = 1;                           /* DeviceType HAL */
    MEM32(c + 0x08) = 0x20000;                     /* Caps: READ_SCANLINE */
    MEM32(c + 0x0C) = 0x20000 | 0x10000000 | 0x20000000 | 0x40000000;   /* FULLSCREENGAMMA, CANMANAGERESOURCE, DYNAMICTEXTURES, CANAUTOGENMIPMAP */
    MEM32(c + 0x10) = 0x20 | 0x100 | 0x200;        /* Caps3: ALPHA_FULLSCREEN_FLIP_OR_DISCARD, COPY_TO_VIDMEM/SYSTEMMEM */
    MEM32(c + 0x14) = 0x80000000u | 1;             /* PresentationIntervals: IMMEDIATE, ONE */
    MEM32(c + 0x18) = 1;                           /* CursorCaps: COLOR */
    MEM32(c + 0x1C) = 0x10 | 0x20 | 0x40 | 0x80 | 0x100 | 0x200 | 0x400 | 0x800 | 0x2000 | 0x8000 | 0x10000 |
                      0x80000 | 0x100000;          /* DevCaps incl. HWTRANSFORMANDLIGHT, HWRASTERIZATION, PUREDEVICE */
    MEM32(c + 0x20) = 0x2 | 0x10 | 0x20 | 0x40 | 0x80 | 0x200 | 0x400 | 0x800 | 0x4000 | 0x8000 | 0x10000 |
                      0x20000 | 0x80000;           /* PrimitiveMiscCaps */
    MEM32(c + 0x24) = 0x10 | 0x80 | 0x100 | 0x2000 | 0x10000 | 0x20000 | 0x200000 | 0x400000 | 0x1000000 |
                      0x2000000 | 0x4000000 | 0x100000;   /* RasterCaps: ZTEST, FOGVERTEX/TABLE/RANGE, LODBIAS, ANISOTROPY, ZFOG, WFOG, SCISSOR, DEPTHBIAS */
    MEM32(c + 0x28) = 0xFF;                        /* ZCmpCaps */
    MEM32(c + 0x2C) = 0x3FFF;                      /* SrcBlendCaps */
    MEM32(c + 0x30) = 0x3FFF;                      /* DestBlendCaps */
    MEM32(c + 0x34) = 0xFF;                        /* AlphaCmpCaps */
    MEM32(c + 0x38) = 0x8 | 0x200 | 0x4000 | 0x80000;   /* ShadeCaps */
    MEM32(c + 0x3C) = 0x1 | 0x4 | 0x400 | 0x800 | 0x2000 | 0x4000 | 0x8000 | 0x10000;   /* TextureCaps: no POW2 */
    MEM32(c + 0x40) = 0x07030700;                  /* TextureFilterCaps: MIN/MAG point, linear, aniso; MIP point, linear */
    MEM32(c + 0x44) = 0x07030700;                  /* CubeTextureFilterCaps */
    MEM32(c + 0x48) = 0x03030300;                  /* VolumeTextureFilterCaps */
    MEM32(c + 0x4C) = 0x3F;                        /* TextureAddressCaps */
    MEM32(c + 0x50) = 0x3F;                        /* VolumeTextureAddressCaps */
    MEM32(c + 0x54) = 0x1F;                        /* LineCaps */
    MEM32(c + 0x58) = 4096;                        /* MaxTextureWidth */
    MEM32(c + 0x5C) = 4096;                        /* MaxTextureHeight */
    MEM32(c + 0x60) = 512;                         /* MaxVolumeExtent */
    MEM32(c + 0x64) = 8192;                        /* MaxTextureRepeat */
    MEM32(c + 0x68) = 4096;                        /* MaxTextureAspectRatio */
    MEM32(c + 0x6C) = 16;                          /* MaxAnisotropy */
    putf(c + 0x70, 1e10f);                         /* MaxVertexW */
    putf(c + 0x74, -32768.0f), putf(c + 0x78, -32768.0f), putf(c + 0x7C, 32768.0f), putf(c + 0x80, 32768.0f);
    MEM32(c + 0x88) = 0x1FF;                       /* StencilCaps incl. TWOSIDED */
    MEM32(c + 0x8C) = 8 | 0x80000;                 /* FVFCaps: 8 texcoords, DONOTSTRIPELEMENTS */
    MEM32(c + 0x90) = 0x3FFFFFF;                   /* TextureOpCaps: every D3DTOP */
    MEM32(c + 0x94) = 8;                           /* MaxTextureBlendStages */
    MEM32(c + 0x98) = 8;                           /* MaxSimultaneousTextures */
    MEM32(c + 0x9C) = 0x1 | 0x2 | 0x8 | 0x10 | 0x20 | 0x100;   /* VertexProcessingCaps */
    MEM32(c + 0xA0) = 8;                           /* MaxActiveLights */
    MEM32(c + 0xA4) = 6;                           /* MaxUserClipPlanes */
    MEM32(c + 0xA8) = 0;                           /* MaxVertexBlendMatrices */
    putf(c + 0xB0, 1.0f);                          /* MaxPointSize */
    MEM32(c + 0xB4) = 0xFFFFF;                     /* MaxPrimitiveCount */
    MEM32(c + 0xB8) = 0xFFFFFF;                    /* MaxVertexIndex */
    MEM32(c + 0xBC) = 16;                          /* MaxStreams */
    MEM32(c + 0xC0) = 2048;                        /* MaxStreamStride */
    MEM32(c + 0xC4) = 0xFFFE0200u;                 /* VertexShaderVersion vs_2_0 (plan 5: what the game ships) */
    MEM32(c + 0xC8) = 256;                         /* MaxVertexShaderConst */
    MEM32(c + 0xCC) = 0xFFFF0200u;                 /* PixelShaderVersion ps_2_0 */
    putf(c + 0xD0, 65504.0f);                      /* PixelShader1xMaxValue */
    MEM32(c + 0xD4) = 0x1 | 0x10 | 0x40;           /* DevCaps2: STREAMOFFSET, STRETCHRECT_FROM_TEXTURES, VERTEXELEMENTSCANSHARESTREAMOFFSET */
    MEM32(c + 0xE8) = 1;                           /* NumberOfAdaptersInGroup */
    MEM32(c + 0xEC) = 0x33F;                       /* DeclTypes */
    MEM32(c + 0xF0) = 4;                           /* NumSimultaneousRTs */
    MEM32(c + 0xF4) = 0x03000300;                  /* StretchRectFilterCaps */
    /* VS20Caps {Caps, DynamicFlowControlDepth, NumTemps, StaticFlowControlDepth} */
    MEM32(c + 0xF8) = 0, MEM32(c + 0xFC) = 0, MEM32(c + 0x100) = 12, MEM32(c + 0x104) = 1;
    /* PS20Caps {Caps, DynamicFlowControlDepth, NumTemps, StaticFlowControlDepth, NumInstructionSlots} */
    MEM32(c + 0x108) = 0, MEM32(c + 0x10C) = 0, MEM32(c + 0x110) = 12, MEM32(c + 0x114) = 0;
    MEM32(c + 0x118) = 96;
    MEM32(c + 0x11C) = 0x01000100;                 /* VertexTextureFilterCaps: point */
    MEM32(c + 0x120) = 65535;                      /* MaxVShaderInstructionsExecuted */
    MEM32(c + 0x124) = 96;                         /* MaxPShaderInstructionsExecuted */
    MEM32(c + 0x128) = 0;                          /* MaxVertexShader30InstructionSlots: no 3.0 */
    MEM32(c + 0x12C) = 0;                          /* MaxPixelShader30InstructionSlots */
}

/* ---- IDirect3D9 ---- */
struct d3d9 { struct d3d_obj base; };

static void destroy_d3d9(struct d3d_obj *o) { free(o); }

uint32_t d3d9_new(void) {
    struct d3d9 *d = calloc(1, sizeof *d);
    d->base.destroy = destroy_d3d9;
    return d3d_new(D3DK_D3D9, "IDirect3D9", &d->base);
}

static void x_Direct3DCreate9(void) {                 /* (SDKVersion) */
    uint32_t v = A32(0) & 0x7FFFFFFFu;
    RET(v == 31 || v == 32 ? d3d9_new() : 0, 1);
}

#define ADAPTER_OK(a) ((a) == 0)
#define DEVTYPE_OK(t) ((t) == 1)

static void a_RegisterSoftwareDevice(void) { RET(D3DERR_INVALIDCALL, 2); }
static void a_GetAdapterCount(void) { RET(1, 1); }
static void a_GetAdapterIdentifier(void) {            /* (this, adapter, flags, D3DADAPTER_IDENTIFIER9*) */
    uint32_t id = A32(3);
    if (!ADAPTER_OK(A32(1)) || !id) RET(D3DERR_INVALIDCALL, 4);
    memset(hle_ptr(id), 0, 1100);
    strcpy(hle_ptr(id), BW2_GPU_DRIVER);
    strcpy(hle_ptr(id + 512), BW2_GPU_DESCRIPTION);
    strcpy(hle_ptr(id + 1024), "\\\\.\\DISPLAY1");
    MEM32(id + 1056) = BW2_GPU_VERSION_LO, MEM32(id + 1060) = BW2_GPU_VERSION_HI;
    MEM32(id + 1064) = BW2_GPU_VENDOR, MEM32(id + 1068) = BW2_GPU_DEVICE;
    memcpy(hle_ptr(id + 1080), bw2_gpu_guid, 16);
    MEM32(id + 1096) = 1;                                /* WHQLLevel */
    RET(D3D_OK, 4);
}
static void a_GetAdapterModeCount(void) {             /* (this, adapter, format) */
    RET(ADAPTER_OK(A32(1)) ? d3d_mode_count(A32(2)) : 0, 3);
}
static void a_EnumAdapterModes(void) {                /* (this, adapter, format, mode, D3DDISPLAYMODE*) */
    uint32_t w, h, m = A32(4);
    if (!ADAPTER_OK(A32(1)) || !m || !d3d_mode(A32(2), A32(3), &w, &h)) RET(D3DERR_INVALIDCALL, 5);
    MEM32(m) = w, MEM32(m + 4) = h, MEM32(m + 8) = 60, MEM32(m + 12) = A32(2);
    RET(D3D_OK, 5);
}
static void a_GetAdapterDisplayMode(void) {           /* (this, adapter, D3DDISPLAYMODE*) */
    uint32_t w, h, m = A32(2);
    if (!ADAPTER_OK(A32(1)) || !m) RET(D3DERR_INVALIDCALL, 3);
    d3d_current_mode(&w, &h);
    MEM32(m) = w, MEM32(m + 4) = h, MEM32(m + 8) = 60, MEM32(m + 12) = D3DFMT_X8R8G8B8;
    RET(D3D_OK, 3);
}
static void a_CheckDeviceType(void) {                 /* (this, adapter, type, display fmt, backbuffer fmt, windowed) */
    uint32_t disp = A32(3), bb = A32(4), windowed = A32(5);
    if (!ADAPTER_OK(A32(1))) RET(D3DERR_INVALIDCALL, 6);
    if (!DEVTYPE_OK(A32(2)) || !display_format(disp)) RET(D3DERR_NOTAVAILABLE, 6);
    int ok = bb == disp || (disp == D3DFMT_X8R8G8B8 && bb == D3DFMT_A8R8G8B8) || (windowed && bb == D3DFMT_UNKNOWN);
    RET(ok ? D3D_OK : D3DERR_NOTAVAILABLE, 6);
}
static void a_CheckDeviceFormat(void) {               /* (this, adapter, type, adapter fmt, usage, rtype, fmt) */
    if (!ADAPTER_OK(A32(1))) RET(D3DERR_INVALIDCALL, 7);
    if (!DEVTYPE_OK(A32(2))) RET(D3DERR_NOTAVAILABLE, 7);
    RET(d3d_check_format(A32(3), A32(4), A32(5), A32(6)), 7);
}
static void a_CheckDeviceMultiSampleType(void) {      /* (this, adapter, type, fmt, windowed, msaa, DWORD* quality) */
    if (!ADAPTER_OK(A32(1))) RET(D3DERR_INVALIDCALL, 7);
    if (!DEVTYPE_OK(A32(2)) || !d3d_format(A32(3)) || A32(5) != 0) {
        if (A32(6)) MEM32(A32(6)) = 0;
        RET(D3DERR_NOTAVAILABLE, 7);
    }
    if (A32(6)) MEM32(A32(6)) = 1;
    RET(D3D_OK, 7);
}
static void a_CheckDepthStencilMatch(void) {          /* (this, adapter, type, adapter fmt, rt fmt, ds fmt) */
    if (!ADAPTER_OK(A32(1))) RET(D3DERR_INVALIDCALL, 6);
    const struct d3d_format *rt = d3d_format(A32(4)), *ds = d3d_format(A32(5));
    int ok = DEVTYPE_OK(A32(2)) && rt && (rt->use & FU_RT) && ds && (ds->use & FU_DS);
    RET(ok ? D3D_OK : D3DERR_NOTAVAILABLE, 6);
}
static void a_CheckDeviceFormatConversion(void) {     /* (this, adapter, type, src fmt, dst fmt) */
    if (!ADAPTER_OK(A32(1))) RET(D3DERR_INVALIDCALL, 5);
    uint32_t s = A32(3), d = A32(4);
    int ok = DEVTYPE_OK(A32(2)) && (s == D3DFMT_X8R8G8B8 || s == D3DFMT_A8R8G8B8) &&
             (d == D3DFMT_X8R8G8B8 || d == D3DFMT_A8R8G8B8);
    RET(ok ? D3D_OK : D3DERR_NOTAVAILABLE, 5);
}
static void a_GetDeviceCaps(void) {                   /* (this, adapter, type, D3DCAPS9*) */
    if (!ADAPTER_OK(A32(1)) || !A32(3)) RET(D3DERR_INVALIDCALL, 4);
    if (!DEVTYPE_OK(A32(2))) RET(D3DERR_NOTAVAILABLE, 4);
    d3d_fill_caps(A32(3));
    RET(D3D_OK, 4);
}
static void a_GetAdapterMonitor(void) { RET(ADAPTER_OK(A32(1)) ? 0x4D4F4E31u : 0, 2); }

static const win32hle_shim g_impls[] = {
    { "IDirect3D9::RegisterSoftwareDevice", a_RegisterSoftwareDevice },
    { "IDirect3D9::GetAdapterCount", a_GetAdapterCount },
    { "IDirect3D9::GetAdapterIdentifier", a_GetAdapterIdentifier },
    { "IDirect3D9::GetAdapterModeCount", a_GetAdapterModeCount },
    { "IDirect3D9::EnumAdapterModes", a_EnumAdapterModes },
    { "IDirect3D9::GetAdapterDisplayMode", a_GetAdapterDisplayMode },
    { "IDirect3D9::CheckDeviceType", a_CheckDeviceType },
    { "IDirect3D9::CheckDeviceFormat", a_CheckDeviceFormat },
    { "IDirect3D9::CheckDeviceMultiSampleType", a_CheckDeviceMultiSampleType },
    { "IDirect3D9::CheckDepthStencilMatch", a_CheckDepthStencilMatch },
    { "IDirect3D9::CheckDeviceFormatConversion", a_CheckDeviceFormatConversion },
    { "IDirect3D9::GetDeviceCaps", a_GetDeviceCaps },
    { "IDirect3D9::GetAdapterMonitor", a_GetAdapterMonitor },
    { 0, 0 } };
static const win32hle_shim g_exports[] = { { "Direct3DCreate9", x_Direct3DCreate9 }, { 0, 0 } };

void d3d9_adapter_register(void) {
    publish_mode();                              /* the desktop, until a full-screen device */
    d3d_impls(g_impls);
    win32hle_register(g_exports);
}
