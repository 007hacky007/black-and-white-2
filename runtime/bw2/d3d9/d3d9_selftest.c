/*
 * d3d9_selftest.c - the D3D9, DirectDraw and D3DX layers driven as lifted
 * code drives them: arguments pushed right to left, a return address, then
 * the shim; COM methods through the guest vtable. Each check also verifies
 * the stdcall pop. Built with the engine (minus main.c) by tests/test_d3d9.py;
 * `d3d9_selftest <group>` runs one group and prints "<group>: <n> failed".
 */
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "win32hle.h"
#include "bw2.h"
#include "d3d_com.h"
#include "d3d9_device.h"

const recomp_dispatch_entry_t recomp_dispatch_table[1] = { { 0, 0 } };
const uint32_t recomp_dispatch_count = 0;

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)
#define CHECKEQ(a, b) do { uint64_t _a = (uint64_t)(a), _b = (uint64_t)(b); if (_a != _b) { \
    printf("FAIL %s:%d %s == %s: 0x%llX != 0x%llX\n", __FILE__, __LINE__, #a, #b, \
           (unsigned long long)_a, (unsigned long long)_b); fails++; } } while (0)

static uint32_t run(recomp_func_t f, const char *name, int n, const uint32_t *a) {
    uint32_t esp0 = g_esp;
    for (int i = n - 1; i >= 0; i--) { g_esp -= 4; MEM32(g_esp) = a[i]; }
    g_esp -= 4;
    MEM32(g_esp) = 0x12345678u;
    f();
    if (g_esp != esp0) {
        printf("FAIL %s: popped %d bytes, want %d\n", name, (int)(g_esp - (esp0 - 4u - 4u * (uint32_t)n)), 4 + 4 * n);
        fails++;
        g_esp = esp0;
    }
    return g_eax;
}

/* An import by name. */
static uint32_t call(const char *name, int n, ...) {
    uint32_t va = hle_resolve(name);
    recomp_func_t f = va ? recomp_lookup_import(va) : NULL;
    if (!f) { printf("FAIL %s: not answered\n", name); fails++; return 0xDEADBEEFu; }
    uint32_t a[16];
    va_list ap;
    va_start(ap, n);
    for (int i = 0; i < n; i++) a[i] = va_arg(ap, uint32_t);
    va_end(ap);
    return run(f, name, n, a);
}

/* A COM method: slot of obj's vtable, with this and n more arguments. */
static const char *g_last_method;
static uint32_t vcall(uint32_t obj, int slot, int n, ...) {
    uint32_t va = MEM32(MEM32(obj) + 4u * (uint32_t)slot);
    recomp_func_t f = recomp_lookup_import(va);
    g_last_method = g_cur_import;
    if (!f) { printf("FAIL slot %d of 0x%08X: no method\n", slot, obj); fails++; return 0xDEADBEEFu; }
    uint32_t a[16] = { obj };
    va_list ap;
    va_start(ap, n);
    for (int i = 0; i < n; i++) a[1 + i] = va_arg(ap, uint32_t);
    va_end(ap);
    return run(f, g_cur_import, n + 1, a);
}
static const char *method_name(uint32_t obj, int slot) {
    recomp_lookup_import(MEM32(MEM32(obj) + 4u * (uint32_t)slot));
    return g_cur_import ? g_cur_import : "";
}

static uint32_t gmem(uint32_t n) { return hle_va(hle_alloc(n)); }
static uint32_t gstr(const char *s) { uint32_t p = gmem((uint32_t)strlen(s) + 1); strcpy(hle_ptr(p), s); return p; }

/* D3D9 numbers the tests use */
#define D3D_SDK_VERSION 32u
#define HAL 1u
#define REF 2u
#define FMT_A8R8G8B8 21u
#define FMT_X8R8G8B8 22u
#define FMT_R5G6B5 23u
#define FMT_D16_LOCKABLE 70u
#define FMT_D16 80u
#define FMT_D24S8 75u
#define FMT_DXT1 0x31545844u
#define RT_SURFACE 1u
#define RT_TEXTURE 3u
#define USAGE_DEPTHSTENCIL 2u

/* IDirect3D9 slots */
enum { D9_GetAdapterCount = 4, D9_GetAdapterIdentifier, D9_GetAdapterModeCount, D9_EnumAdapterModes,
       D9_GetAdapterDisplayMode, D9_CheckDeviceType, D9_CheckDeviceFormat, D9_CheckDeviceMultiSampleType,
       D9_CheckDepthStencilMatch, D9_CheckDeviceFormatConversion, D9_GetDeviceCaps, D9_GetAdapterMonitor,
       D9_CreateDevice };

static uint32_t new_d3d9(void) { return call("Direct3DCreate9", 1, D3D_SDK_VERSION); }

static void group_adapter(void) {
    uint32_t d = new_d3d9();
    CHECK(d && d != 0xDEADBEEFu);
    CHECK(!strcmp(method_name(d, 16), "IDirect3D9::CreateDevice"));
    CHECK(!strcmp(method_name(d, 4), "IDirect3D9::GetAdapterCount"));
    CHECKEQ(vcall(d, D9_GetAdapterCount, 0), 1);

    uint32_t id = gmem(1100);
    CHECKEQ(vcall(d, D9_GetAdapterIdentifier, 3, 0u, 0u, id), D3D_OK);
    CHECK(!strcmp((char *)hle_ptr(id), "bw2metal.dll"));
    CHECK(!strcmp((char *)hle_ptr(id + 512), "BW2 virtual GPU (Metal)"));
    CHECKEQ(MEM32(id + 1056), (10u << 16) | 0u);           /* DriverVersion low: 10.0 */
    CHECKEQ(MEM32(id + 1060), (6u << 16) | 14u);           /* high: 6.14 */
    CHECKEQ(MEM32(id + 1064), 0x106B);
    CHECKEQ(MEM32(id + 1068), 1);
    CHECKEQ(MEM32(id + 1080), 0xB2B2B2B2u);
    CHECKEQ(MEM32(id + 1096), 1);
    CHECKEQ(vcall(d, D9_GetAdapterIdentifier, 3, 1u, 0u, id), D3DERR_INVALIDCALL);

    uint32_t n = vcall(d, D9_GetAdapterModeCount, 2, 0u, FMT_X8R8G8B8);
    CHECK(n >= 10);
    CHECKEQ(vcall(d, D9_GetAdapterModeCount, 2, 0u, FMT_R5G6B5), n);
    CHECKEQ(vcall(d, D9_GetAdapterModeCount, 2, 0u, FMT_A8R8G8B8), 0);
    uint32_t m = gmem(16), pw = 0, ph = 0, saw1080 = 0, saw1200 = 0;
    for (uint32_t i = 0; i < n; i++) {
        CHECKEQ(vcall(d, D9_EnumAdapterModes, 4, 0u, FMT_X8R8G8B8, i, m), D3D_OK);
        uint32_t w = MEM32(m), h = MEM32(m + 4);
        CHECK(w > pw || (w == pw && h > ph));                /* sorted, no duplicates */
        CHECKEQ(MEM32(m + 8), 60);
        CHECKEQ(MEM32(m + 12), FMT_X8R8G8B8);
        saw1080 |= w == 1920 && h == 1080;
        saw1200 |= w == 1920 && h == 1200;
        pw = w, ph = h;
    }
    CHECK(saw1080 && !saw1200);
    CHECK(pw == 1920 && ph == 1080);                         /* the headless display is the largest */
    CHECKEQ(vcall(d, D9_EnumAdapterModes, 4, 0u, FMT_X8R8G8B8, n, m), D3DERR_INVALIDCALL);
    CHECKEQ(vcall(d, D9_GetAdapterDisplayMode, 2, 0u, m), D3D_OK);
    CHECK(MEM32(m) == 1920 && MEM32(m + 4) == 1080 && MEM32(m + 8) == 60 && MEM32(m + 12) == FMT_X8R8G8B8);

    CHECKEQ(vcall(d, D9_CheckDeviceType, 5, 0u, HAL, FMT_X8R8G8B8, FMT_A8R8G8B8, 1u), D3D_OK);
    CHECKEQ(vcall(d, D9_CheckDeviceType, 5, 0u, HAL, FMT_X8R8G8B8, FMT_X8R8G8B8, 0u), D3D_OK);
    CHECKEQ(vcall(d, D9_CheckDeviceType, 5, 0u, REF, FMT_X8R8G8B8, FMT_X8R8G8B8, 1u), D3DERR_NOTAVAILABLE);
    CHECKEQ(vcall(d, D9_CheckDeviceFormat, 6, 0u, HAL, FMT_X8R8G8B8, 0u, RT_TEXTURE, FMT_DXT1), D3D_OK);
    CHECKEQ(vcall(d, D9_CheckDeviceFormat, 6, 0u, HAL, FMT_X8R8G8B8, USAGE_DEPTHSTENCIL, RT_SURFACE, FMT_D16_LOCKABLE),
            D3DERR_NOTAVAILABLE);
    CHECKEQ(vcall(d, D9_CheckDeviceFormat, 6, 0u, HAL, FMT_X8R8G8B8, USAGE_DEPTHSTENCIL, RT_SURFACE, FMT_D24S8), D3D_OK);
    uint32_t q = gmem(4);
    CHECKEQ(vcall(d, D9_CheckDeviceMultiSampleType, 6, 0u, HAL, FMT_X8R8G8B8, 1u, 0u, q), D3D_OK);
    CHECKEQ(MEM32(q), 1);
    CHECKEQ(vcall(d, D9_CheckDeviceMultiSampleType, 6, 0u, HAL, FMT_X8R8G8B8, 1u, 4u, q), D3DERR_NOTAVAILABLE);
    CHECKEQ(vcall(d, D9_CheckDepthStencilMatch, 5, 0u, HAL, FMT_X8R8G8B8, FMT_A8R8G8B8, FMT_D24S8), D3D_OK);
    CHECKEQ(vcall(d, D9_CheckDeviceFormatConversion, 4, 0u, HAL, FMT_A8R8G8B8, FMT_X8R8G8B8), D3D_OK);

    uint32_t caps = gmem(304);
    CHECKEQ(vcall(d, D9_GetDeviceCaps, 3, 0u, HAL, caps), D3D_OK);
    CHECKEQ(MEM32(caps + 0x00), HAL);
    CHECKEQ(MEM32(caps + 0xC4), 0xFFFE0300u);
    CHECKEQ(MEM32(caps + 0xCC), 0xFFFF0300u);
    CHECKEQ(MEM32(caps + 0xC8), 256);
    CHECKEQ(MEM32(caps + 0x58), 4096);
    CHECKEQ(MEM32(caps + 0x94), 8);
    CHECKEQ(MEM32(caps + 0x98), 8);
    CHECKEQ(MEM32(caps + 0xA0), 8);
    CHECKEQ(MEM32(caps + 0xF0), 4);
    CHECKEQ(MEM32(caps + 0x3C) & 0x102, 0);                  /* neither POW2 nor NONPOW2CONDITIONAL */
    CHECKEQ(vcall(d, D9_GetDeviceCaps, 3, 0u, REF, caps), D3DERR_NOTAVAILABLE);
    CHECKEQ(vcall(d, D9_GetAdapterMonitor, 1, 0u), 0x4D4F4E31u);
    CHECKEQ(vcall(d, 3, 1, 0u), D3DERR_INVALIDCALL);         /* RegisterSoftwareDevice */
    CHECKEQ(vcall(d, 2, 0), 0);                              /* Release */
}

/* DirectDrawEnumerateA's callback: (guid, description, name, context) -> TRUE */
static int g_enum_calls;
static uint32_t g_enum_guid, g_enum_ctx;
static char g_enum_desc[64], g_enum_name[64];
static void enum_cb(void) {
    g_enum_calls++;
    g_enum_guid = A32(0), g_enum_ctx = A32(3);
    snprintf(g_enum_desc, sizeof g_enum_desc, "%s", (char *)APTR(1));
    snprintf(g_enum_name, sizeof g_enum_name, "%s", (char *)APTR(2));
    RET(1, 4);
}
static const win32hle_shim g_test_shims[] = { { "selftest_ddenum_cb", enum_cb }, { 0, 0 } };

static const uint8_t CLSID_DirectDraw[16] = { 0xE0, 0x0E, 0xB7, 0xD7, 0x40, 0x43, 0xCF, 0x11,
                                              0xB0, 0x63, 0x00, 0x20, 0xAF, 0xC2, 0xCD, 0x35 };
static const uint8_t IID_IDirectDraw7[16] = { 0xC0, 0x5E, 0xE6, 0x15, 0x9C, 0x3B, 0xD2, 0x11,
                                              0xB9, 0x2F, 0x00, 0x60, 0x97, 0x97, 0xEA, 0x5B };

static void group_ddraw(void) {
    uint32_t clsid = gmem(16), iid = gmem(16), out = gmem(4);
    memcpy(hle_ptr(clsid), CLSID_DirectDraw, 16);
    memcpy(hle_ptr(iid), IID_IDirectDraw7, 16);
    CHECKEQ(call("CoInitialize", 1, 0u), 0);
    CHECKEQ(call("CoCreateInstance", 5, clsid, 0u, 7u, iid, out), 0);
    uint32_t dd = MEM32(out);
    CHECK(dd != 0);
    CHECK(!strcmp(method_name(dd, 23), "IDirectDraw7::GetAvailableVidMem"));
    CHECKEQ(vcall(dd, 18, 1, 0u), 0);                        /* Initialize(NULL) */
    uint32_t caps = gmem(16), total = gmem(4), freem = gmem(4);
    MEM32(caps) = 0x10000000u;                               /* DDSCAPS_LOCALVIDMEM */
    CHECKEQ(vcall(dd, 23, 3, caps, total, freem), 0);
    CHECKEQ(MEM32(total), 512u << 20);
    CHECKEQ(MEM32(freem), 512u << 20);
    CHECKEQ(vcall(dd, 2, 0), 0);

    CHECKEQ(call("DirectDrawCreateEx", 4, 0u, out, iid, 0u), 0);
    dd = MEM32(out);
    uint32_t id = gmem(1068);
    CHECKEQ(vcall(dd, 27, 2, id, 0u), 0);                    /* GetDeviceIdentifier */
    CHECK(!strcmp((char *)hle_ptr(id), "bw2metal.dll"));
    CHECK(!strcmp((char *)hle_ptr(id + 512), "BW2 virtual GPU (Metal)"));
    CHECKEQ(MEM32(id + 1024), (10u << 16) | 0u);
    CHECKEQ(MEM32(id + 1028), (6u << 16) | 14u);
    CHECKEQ(MEM32(id + 1032), 0x106B);
    CHECKEQ(MEM32(id + 1036), 1);
    CHECKEQ(MEM32(id + 1064), 1);
    CHECKEQ(vcall(dd, 2, 0), 0);

    win32hle_register(g_test_shims);
    CHECKEQ(call("DirectDrawEnumerateA", 2, hle_resolve("selftest_ddenum_cb"), 0x1234u), 0);
    CHECKEQ(g_enum_calls, 1);
    CHECKEQ(g_enum_guid, 0);
    CHECKEQ(g_enum_ctx, 0x1234);
    CHECK(!strcmp(g_enum_desc, "Primary Display Driver"));
    CHECK(!strcmp(g_enum_name, "display"));
}

struct test_obj { struct d3d_obj base; uint32_t n; };
static void group_objects(void) {
    enum { N = 200000 };
    uint32_t *objs = malloc(sizeof *objs * N);
    struct test_obj *hosts = calloc(N, sizeof *hosts);
    uint32_t live0 = d3d_live_count();
    for (uint32_t i = 0; i < N; i++) {
        hosts[i].n = i;
        objs[i] = d3d_new(D3DK_D3DXBUFFER, "ID3DXBuffer", &hosts[i].base);
    }
    for (uint32_t i = 0; i < N; i += 2) CHECKEQ(d3d_release(objs[i]), 0);
    CHECKEQ(d3d_live_count() - live0, N / 2);
    int bad = 0;
    for (uint32_t i = 0; i < N; i++) {
        if (i % 2) bad += ((struct test_obj *)d3d_host(objs[i], D3DK_D3DXBUFFER))->n != i;
        else bad += d3d_alive(objs[i]);
    }
    CHECKEQ(bad, 0);
    /* freed ids are reused, and an old handle never resolves to a new object */
    struct test_obj again = { .n = 7 };
    uint32_t o = d3d_new(D3DK_D3DXBUFFER, "ID3DXBuffer", &again.base);
    CHECK(d3d_alive(o));
    CHECKEQ(vcall(o, 1, 0), 2);                              /* AddRef through the vtable */
    CHECKEQ(vcall(o, 2, 0), 1);
}

static void group_unimplemented(void) {
    struct d3d_obj dev = { 0 };
    uint32_t o = d3d_new(D3DK_DEVICE, "IDirect3DDevice9", &dev);
    vcall(o, 115, 5, 0u, 0u, 0u, 0u, 0u);                    /* DrawRectPatch: never implemented */
    printf("unimplemented: returned\n");
}

/* ---- Task 4: devices ---- */

/* IDirect3DDevice9 slots */
enum { DV_TestCooperativeLevel = 3, DV_GetAvailableTextureMem, DV_EvictManagedResources, DV_GetDirect3D,
       DV_GetDeviceCaps, DV_GetDisplayMode, DV_GetCreationParameters, DV_SetCursorProperties, DV_SetCursorPosition,
       DV_ShowCursor, DV_CreateAdditionalSwapChain, DV_GetSwapChain, DV_GetNumberOfSwapChains, DV_Reset, DV_Present,
       DV_GetBackBuffer, DV_GetRasterStatus, DV_SetDialogBoxMode, DV_SetGammaRamp, DV_GetGammaRamp,
       DV_CreateTexture, DV_CreateVolumeTexture, DV_CreateCubeTexture, DV_CreateVertexBuffer, DV_CreateIndexBuffer,
       DV_CreateRenderTarget, DV_CreateDepthStencilSurface, DV_UpdateSurface, DV_UpdateTexture,
       DV_GetRenderTargetData, DV_GetFrontBufferData, DV_StretchRect, DV_ColorFill, DV_CreateOffscreenPlainSurface,
       DV_SetRenderTarget, DV_GetRenderTarget, DV_SetDepthStencilSurface, DV_GetDepthStencilSurface,
       DV_BeginScene, DV_EndScene, DV_Clear, DV_SetTransform, DV_GetTransform, DV_MultiplyTransform,
       DV_SetViewport, DV_GetViewport, DV_SetMaterial, DV_GetMaterial, DV_SetLight, DV_GetLight, DV_LightEnable,
       DV_GetLightEnable, DV_SetClipPlane, DV_GetClipPlane, DV_SetRenderState, DV_GetRenderState,
       DV_CreateStateBlock, DV_BeginStateBlock, DV_EndStateBlock, DV_SetClipStatus, DV_GetClipStatus,
       DV_GetTexture, DV_SetTexture, DV_GetTextureStageState, DV_SetTextureStageState, DV_GetSamplerState,
       DV_SetSamplerState, DV_ValidateDevice, DV_SetPaletteEntries, DV_GetPaletteEntries,
       DV_SetCurrentTexturePalette, DV_GetCurrentTexturePalette, DV_SetScissorRect, DV_GetScissorRect,
       DV_SetSoftwareVertexProcessing, DV_GetSoftwareVertexProcessing, DV_SetNPatchMode, DV_GetNPatchMode,
       DV_DrawPrimitive, DV_DrawIndexedPrimitive, DV_DrawPrimitiveUP, DV_DrawIndexedPrimitiveUP,
       DV_ProcessVertices, DV_CreateVertexDeclaration, DV_SetVertexDeclaration, DV_GetVertexDeclaration,
       DV_SetFVF, DV_GetFVF, DV_CreateVertexShader, DV_SetVertexShader, DV_GetVertexShader,
       DV_SetVertexShaderConstantF, DV_GetVertexShaderConstantF, DV_SetVertexShaderConstantI,
       DV_GetVertexShaderConstantI, DV_SetVertexShaderConstantB, DV_GetVertexShaderConstantB,
       DV_SetStreamSource, DV_GetStreamSource, DV_SetStreamSourceFreq, DV_GetStreamSourceFreq, DV_SetIndices,
       DV_GetIndices, DV_CreatePixelShader, DV_SetPixelShader, DV_GetPixelShader, DV_SetPixelShaderConstantF,
       DV_GetPixelShaderConstantF, DV_SetPixelShaderConstantI, DV_GetPixelShaderConstantI,
       DV_SetPixelShaderConstantB, DV_GetPixelShaderConstantB, DV_DrawRectPatch, DV_DrawTriPatch, DV_DeletePatch,
       DV_CreateQuery };
/* IDirect3DSurface9 slots */
enum { SF_GetDevice = 3, SF_GetType = 10, SF_GetContainer = 11, SF_GetDesc, SF_LockRect, SF_UnlockRect };

#define POOL_DEFAULT 0u
#define POOL_MANAGED 1u
#define POOL_SYSTEMMEM 2u
#define CLEAR_TARGET 1u
#define CLEAR_ZBUFFER 2u
#define CLEAR_STENCIL 4u
#define CREATE_FPU_PRESERVE 0x2u
#define CREATE_HWVP 0x40u

static uint32_t g_d3d9;
static uint32_t g_pp;                                       /* the last device's D3DPRESENT_PARAMETERS */

static uint32_t pp_new(uint32_t w, uint32_t h, uint32_t fmt, int autods) {
    uint32_t pp = gmem(56);
    MEM32(pp + 0) = w, MEM32(pp + 4) = h, MEM32(pp + 8) = fmt, MEM32(pp + 12) = 1;
    MEM32(pp + 24) = 1;                                     /* D3DSWAPEFFECT_DISCARD */
    MEM32(pp + 28) = 0x10001u;                              /* hDeviceWindow */
    MEM32(pp + 32) = 1;                                     /* Windowed */
    MEM32(pp + 36) = (uint32_t)autods, MEM32(pp + 40) = FMT_D24S8;
    return pp;
}
static uint32_t new_device_flags(uint32_t w, uint32_t h, uint32_t flags) {
    if (!g_d3d9) g_d3d9 = new_d3d9();
    g_pp = pp_new(w, h, FMT_X8R8G8B8, 1);
    uint32_t out = gmem(4);
    uint32_t hr = vcall(g_d3d9, D9_CreateDevice, 6, 0u, HAL, 0x10001u, flags, g_pp, out);
    CHECKEQ(hr, D3D_OK);
    return hr ? 0 : MEM32(out);
}
static uint32_t new_device(uint32_t w, uint32_t h) { return new_device_flags(w, h, CREATE_HWVP); }

static uint32_t sysmem_surface(uint32_t dev, uint32_t w, uint32_t h, uint32_t fmt) {
    uint32_t out = gmem(4);
    CHECKEQ(vcall(dev, DV_CreateOffscreenPlainSurface, 6, w, h, fmt, POOL_SYSTEMMEM, out, 0u), D3D_OK);
    return MEM32(out);
}
static uint32_t back_buffer(uint32_t dev) {
    uint32_t out = gmem(4);
    CHECKEQ(vcall(dev, DV_GetBackBuffer, 4, 0u, 0u, 0u, out), D3D_OK);
    return MEM32(out);
}
/* Read a render target back as D3DCOLORs, w*h, into out. */
static void read_rt(uint32_t dev, uint32_t rt, uint32_t w, uint32_t h, uint32_t *out) {
    uint32_t desc = gmem(32);
    CHECKEQ(vcall(rt, SF_GetDesc, 1, desc), D3D_OK);
    uint32_t s = sysmem_surface(dev, w, h, MEM32(desc));
    CHECKEQ(vcall(dev, DV_GetRenderTargetData, 2, rt, s), D3D_OK);
    uint32_t lr = gmem(8);
    CHECKEQ(vcall(s, SF_LockRect, 3, lr, 0u, 0x10u), D3D_OK);   /* D3DLOCK_READONLY */
    uint32_t pitch = MEM32(lr), bits = MEM32(lr + 4);
    for (uint32_t y = 0; y < h; y++) memcpy(out + y * w, hle_ptr(bits + y * pitch), w * 4);
    CHECKEQ(vcall(s, SF_UnlockRect, 0), D3D_OK);
    CHECKEQ(vcall(s, 2, 0), 0);
}
static uint32_t rect(uint32_t l, uint32_t t, uint32_t r, uint32_t b) {
    uint32_t p = gmem(16);
    MEM32(p) = l, MEM32(p + 4) = t, MEM32(p + 8) = r, MEM32(p + 12) = b;
    return p;
}
static uint32_t f32(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }

static void group_clear(void) {
    uint32_t dev = new_device(64, 64), bb = back_buffer(dev);
    static uint32_t px[64 * 64];
    CHECKEQ(vcall(dev, DV_Clear, 6, 0u, 0u, CLEAR_TARGET | CLEAR_ZBUFFER, 0xFF336699u, f32(1.0f), 0u), D3D_OK);
    read_rt(dev, bb, 64, 64, px);
    int bad = 0;
    for (int i = 0; i < 64 * 64; i++) bad += px[i] != 0xFF336699u;
    CHECKEQ(bad, 0);
    /* a rect clear changes only the rect */
    CHECKEQ(vcall(dev, DV_Clear, 6, 1u, rect(8, 8, 16, 12), CLEAR_TARGET, 0xFF00FF00u, f32(1.0f), 0u), D3D_OK);
    read_rt(dev, bb, 64, 64, px);
    bad = 0;
    for (int y = 0; y < 64; y++)
        for (int x = 0; x < 64; x++)
            bad += px[y * 64 + x] != ((x >= 8 && x < 16 && y >= 8 && y < 12) ? 0xFF00FF00u : 0xFF336699u);
    CHECKEQ(bad, 0);
    /* with scissor test on, a full clear is limited to the scissor rect */
    CHECKEQ(vcall(dev, DV_SetScissorRect, 1, rect(0, 0, 4, 4)), D3D_OK);
    CHECKEQ(vcall(dev, DV_SetRenderState, 2, 174u, 1u), D3D_OK);   /* D3DRS_SCISSORTESTENABLE */
    CHECKEQ(vcall(dev, DV_Clear, 6, 0u, 0u, CLEAR_TARGET, 0xFFFF0000u, f32(1.0f), 0u), D3D_OK);
    read_rt(dev, bb, 64, 64, px);
    CHECKEQ(px[0], 0xFFFF0000u);
    CHECKEQ(px[3 * 64 + 3], 0xFFFF0000u);
    CHECKEQ(px[4 * 64 + 4], 0xFF336699u);
    CHECKEQ(px[10 * 64 + 10], 0xFF00FF00u);
    /* and the viewport limits it too */
    CHECKEQ(vcall(dev, DV_SetRenderState, 2, 174u, 0u), D3D_OK);
    uint32_t vp = gmem(24);
    MEM32(vp) = 32, MEM32(vp + 4) = 32, MEM32(vp + 8) = 32, MEM32(vp + 12) = 32, MEM32(vp + 16) = f32(0), MEM32(vp + 20) = f32(1);
    CHECKEQ(vcall(dev, DV_SetViewport, 1, vp), D3D_OK);
    CHECKEQ(vcall(dev, DV_Clear, 6, 0u, 0u, CLEAR_TARGET, 0xFF0000FFu, f32(1.0f), 0u), D3D_OK);
    read_rt(dev, bb, 64, 64, px);
    CHECKEQ(px[40 * 64 + 40], 0xFF0000FFu);
    CHECKEQ(px[31 * 64 + 31], 0xFF336699u);
    CHECK(vcall(bb, 2, 0) >= 1 && d3d_alive(bb));          /* the device still holds it */
}

static void group_persist(void) {
    uint32_t dev = new_device(32, 32), bb = back_buffer(dev);
    static uint32_t px[32 * 32];
    CHECKEQ(vcall(dev, DV_Clear, 6, 0u, 0u, CLEAR_TARGET, 0xFF102030u, f32(1.0f), 0u), D3D_OK);
    CHECKEQ(vcall(dev, DV_Present, 4, 0u, 0u, 0u, 0u), D3D_OK);
    CHECKEQ(vcall(dev, DV_Present, 4, 0u, 0u, 0u, 0u), D3D_OK);
    read_rt(dev, bb, 32, 32, px);
    CHECKEQ(px[0], 0xFF102030u);
    CHECKEQ(px[31 * 32 + 31], 0xFF102030u);
}

void mtl_set_test_drawable(uint32_t w, uint32_t h);
void mtl_read_test_drawable(uint32_t *argb);

static void group_present_scaled(void) {
    uint32_t dev = new_device(320, 200), bb = back_buffer(dev);
    CHECKEQ(vcall(dev, DV_Clear, 6, 0u, 0u, CLEAR_TARGET, 0xFF00FF00u, f32(1.0f), 0u), D3D_OK);
    CHECKEQ(vcall(dev, DV_Clear, 6, 1u, rect(150, 90, 170, 110), CLEAR_TARGET, 0xFFFF0000u, f32(1.0f), 0u), D3D_OK);
    mtl_set_test_drawable(1000, 1000);
    CHECKEQ(vcall(dev, DV_Present, 4, 0u, 0u, 0u, 0u), D3D_OK);
    uint32_t *d = malloc(1000 * 1000 * 4);
    mtl_read_test_drawable(d);
    CHECKEQ(d[500 * 1000 + 500], 0xFFFF0000u);              /* the back buffer's centre */
    CHECKEQ(d[10 * 1000 + 500], 0xFF000000u);               /* the top bar: 320x200 fits as 1000x625 */
    CHECKEQ(d[990 * 1000 + 500], 0xFF000000u);
    CHECKEQ(d[200 * 1000 + 10], 0xFF00FF00u);               /* the picture, at its left edge */
    free(d);
    static uint32_t px[320 * 200];
    read_rt(dev, bb, 320, 200, px);
    CHECKEQ(px[100 * 320 + 160], 0xFFFF0000u);
    CHECKEQ(px[0], 0xFF00FF00u);
}

static void group_reset_rules(void) {
    uint32_t dev = new_device(64, 64), out = gmem(4), v = gmem(4);
    CHECKEQ(vcall(dev, DV_CreateRenderTarget, 8, 16u, 16u, FMT_A8R8G8B8, 0u, 0u, 0u, out, 0u), D3D_OK);
    uint32_t rt = MEM32(out);
    uint32_t pp = pp_new(64, 64, FMT_X8R8G8B8, 1);
    CHECKEQ(vcall(dev, DV_Reset, 1, pp), D3DERR_INVALIDCALL);
    CHECKEQ(vcall(dev, DV_Clear, 6, 0u, 0u, CLEAR_TARGET, 0xFF445566u, f32(1.0f), 0u), D3D_OK);
    static uint32_t px[64 * 64];
    read_rt(dev, back_buffer(dev), 64, 64, px);
    CHECKEQ(px[5], 0xFF445566u);
    CHECKEQ(vcall(rt, 2, 0), 0);
    CHECKEQ(vcall(dev, DV_SetRenderState, 2, 7u, 0u), D3D_OK);     /* ZENABLE off */
    CHECKEQ(vcall(dev, DV_SetRenderState, 2, 22u, 1u), D3D_OK);    /* CULLMODE NONE */
    CHECKEQ(vcall(dev, DV_SetRenderState, 2, 27u, 1u), D3D_OK);    /* ALPHABLENDENABLE */
    CHECKEQ(vcall(dev, DV_Reset, 1, pp), D3D_OK);
    vcall(dev, DV_GetRenderState, 2, 7u, v);  CHECKEQ(MEM32(v), 1);   /* D3DZB_TRUE: auto depth stencil */
    vcall(dev, DV_GetRenderState, 2, 22u, v); CHECKEQ(MEM32(v), 3);   /* D3DCULL_CCW */
    vcall(dev, DV_GetRenderState, 2, 27u, v); CHECKEQ(MEM32(v), 0);
    /* a system-memory surface does not block Reset */
    uint32_t s = sysmem_surface(dev, 8, 8, FMT_A8R8G8B8);
    CHECKEQ(vcall(dev, DV_Reset, 1, pp), D3D_OK);
    CHECKEQ(vcall(s, 2, 0), 0);
}

/* Documented defaults, written from the D3D9 reference (independently of d3d9_state.c). */
static const uint32_t g_rs_defaults[][2] = {
    { 7, 1 }, { 8, 3 }, { 9, 2 }, { 14, 1 }, { 15, 0 }, { 16, 1 }, { 19, 2 }, { 20, 1 }, { 22, 3 }, { 23, 4 },
    { 24, 0 }, { 25, 8 }, { 26, 0 }, { 27, 0 }, { 28, 0 }, { 29, 0 }, { 34, 0 }, { 35, 0 }, { 36, 0x00000000 },
    { 37, 0x3F800000 }, { 38, 0x3F800000 }, { 48, 0 }, { 52, 0 }, { 53, 1 }, { 54, 1 }, { 55, 1 }, { 56, 8 },
    { 57, 0 }, { 58, 0xFFFFFFFF }, { 59, 0xFFFFFFFF }, { 60, 0xFFFFFFFF }, { 128, 0 }, { 135, 0 }, { 136, 1 },
    { 137, 1 }, { 139, 0 }, { 140, 0 }, { 141, 1 }, { 142, 1 }, { 143, 0 }, { 145, 1 }, { 146, 2 }, { 147, 0 },
    { 148, 0 }, { 151, 0 }, { 152, 0 }, { 154, 0x3F800000 }, { 155, 0x3F800000 }, { 156, 0 }, { 157, 0 },
    { 158, 0x3F800000 }, { 159, 0 }, { 160, 0 }, { 161, 1 }, { 162, 0xFFFFFFFF }, { 163, 0 }, { 166, 0x42800000 },
    { 167, 0 }, { 168, 0xF }, { 170, 0 }, { 171, 1 }, { 172, 3 }, { 173, 1 }, { 174, 0 }, { 175, 0 }, { 176, 0 },
    { 178, 0x3F800000 }, { 179, 0x3F800000 }, { 180, 0 }, { 181, 0 }, { 182, 0x3F800000 }, { 183, 0 }, { 184, 0 },
    { 185, 0 }, { 186, 1 }, { 187, 1 }, { 188, 1 }, { 189, 8 }, { 190, 0xF }, { 191, 0xF }, { 192, 0xF },
    { 193, 0xFFFFFFFF }, { 194, 0 }, { 195, 0 }, { 198, 0 }, { 205, 0 }, { 206, 0 }, { 207, 2 }, { 208, 1 }, { 209, 1 },
};
static const uint32_t g_ss_defaults[][2] = {
    { 1, 1 }, { 2, 1 }, { 3, 1 }, { 4, 0 }, { 5, 1 }, { 6, 1 }, { 7, 0 }, { 8, 0 }, { 9, 0 }, { 10, 1 }, { 11, 0 },
    { 12, 0 }, { 13, 0 },
};
static void group_defaults(void) {
    uint32_t dev = new_device(16, 16), v = gmem(4);
    for (unsigned i = 0; i < sizeof g_rs_defaults / sizeof *g_rs_defaults; i++) {
        MEM32(v) = 0xDEADBEEF;
        CHECKEQ(vcall(dev, DV_GetRenderState, 2, g_rs_defaults[i][0], v), D3D_OK);
        if (MEM32(v) != g_rs_defaults[i][1])
            printf("FAIL render state %u = 0x%X, want 0x%X\n", g_rs_defaults[i][0], MEM32(v), g_rs_defaults[i][1]), fails++;
    }
    for (uint32_t smp = 0; smp < 16; smp++)
        for (unsigned i = 0; i < sizeof g_ss_defaults / sizeof *g_ss_defaults; i++) {
            CHECKEQ(vcall(dev, DV_GetSamplerState, 3, smp, g_ss_defaults[i][0], v), D3D_OK);
            if (MEM32(v) != g_ss_defaults[i][1])
                printf("FAIL sampler %u state %u = 0x%X\n", smp, g_ss_defaults[i][0], MEM32(v)), fails++;
        }
    CHECKEQ(vcall(dev, DV_GetSamplerState, 3, 257u, 1u, v), D3D_OK);   /* D3DVERTEXTEXTURESAMPLER0 */
    CHECKEQ(MEM32(v), 1);
    /* stage 0 and stage 1: COLOROP, ARG1, ARG2, ALPHAOP, ARG1, ARG2, TEXCOORDINDEX, TTF, ARG0s, RESULTARG */
    static const uint32_t st0[][2] = { { 1, 4 }, { 2, 2 }, { 3, 1 }, { 4, 2 }, { 5, 2 }, { 6, 1 }, { 7, 0 }, { 11, 0 },
                                       { 22, 0 }, { 23, 0 }, { 24, 0 }, { 26, 1 }, { 27, 1 }, { 28, 1 }, { 32, 0 } };
    static const uint32_t st1[][2] = { { 1, 1 }, { 2, 2 }, { 3, 1 }, { 4, 1 }, { 5, 2 }, { 6, 1 }, { 11, 1 }, { 24, 0 } };
    for (unsigned i = 0; i < sizeof st0 / sizeof *st0; i++) {
        CHECKEQ(vcall(dev, DV_GetTextureStageState, 3, 0u, st0[i][0], v), D3D_OK);
        if (MEM32(v) != st0[i][1]) printf("FAIL stage 0 state %u = 0x%X\n", st0[i][0], MEM32(v)), fails++;
    }
    for (unsigned i = 0; i < sizeof st1 / sizeof *st1; i++) {
        CHECKEQ(vcall(dev, DV_GetTextureStageState, 3, 1u, st1[i][0], v), D3D_OK);
        if (MEM32(v) != st1[i][1]) printf("FAIL stage 1 state %u = 0x%X\n", st1[i][0], MEM32(v)), fails++;
    }
    /* viewport: the whole target, depth 0..1 */
    uint32_t vp = gmem(24);
    CHECKEQ(vcall(dev, DV_GetViewport, 1, vp), D3D_OK);
    CHECK(MEM32(vp) == 0 && MEM32(vp + 4) == 0 && MEM32(vp + 8) == 16 && MEM32(vp + 12) == 16);
    CHECK(MEM32(vp + 16) == f32(0.0f) && MEM32(vp + 20) == f32(1.0f));
    /* a set reads back */
    CHECKEQ(vcall(dev, DV_SetRenderState, 2, 60u, 0x12345678u), D3D_OK);
    vcall(dev, DV_GetRenderState, 2, 60u, v);
    CHECKEQ(MEM32(v), 0x12345678u);
    CHECKEQ(vcall(dev, DV_GetAvailableTextureMem, 0), 512u << 20);
    CHECKEQ(vcall(dev, DV_TestCooperativeLevel, 0), D3D_OK);
    uint32_t caps = gmem(304);
    CHECKEQ(vcall(dev, DV_GetDeviceCaps, 1, caps), D3D_OK);
    CHECKEQ(MEM32(caps + 0xCC), 0xFFFF0300u);
    uint32_t cp = gmem(16);
    CHECKEQ(vcall(dev, DV_GetCreationParameters, 1, cp), D3D_OK);
    CHECK(MEM32(cp) == 0 && MEM32(cp + 4) == HAL && MEM32(cp + 8) == 0x10001u && MEM32(cp + 12) == CREATE_HWVP);
    CHECKEQ(vcall(dev, DV_GetNumberOfSwapChains, 0), 1);
    CHECKEQ(vcall(dev, DV_BeginScene, 0), D3D_OK);
    CHECKEQ(vcall(dev, DV_BeginScene, 0), D3DERR_INVALIDCALL);
    CHECKEQ(vcall(dev, DV_EndScene, 0), D3D_OK);
    CHECKEQ(vcall(dev, DV_EndScene, 0), D3DERR_INVALIDCALL);
    /* CreateDevice wrote back the chosen back buffer */
    CHECKEQ(MEM32(g_pp + 12), 1);
}

static void group_fpu(void) {
    g_fpu_cw = 0x027F;
    new_device_flags(16, 16, CREATE_HWVP);
    CHECKEQ(g_fpu_cw & 0xF00, 0);                           /* 24-bit, round to nearest */
    g_fpu_cw = 0x0E7F;                                      /* PC=64, round toward zero */
    new_device_flags(16, 16, CREATE_HWVP | CREATE_FPU_PRESERVE);
    CHECKEQ(g_fpu_cw, 0x0E7F);
}

static void group_depth_objects(void) {
    uint32_t dev = new_device(32, 32), out = gmem(4);
    CHECKEQ(vcall(dev, DV_CreateDepthStencilSurface, 8, 32u, 32u, FMT_D24S8, 0u, 0u, 0u, out, 0u), D3D_OK);
    uint32_t a = MEM32(out);
    CHECKEQ(vcall(dev, DV_CreateDepthStencilSurface, 8, 32u, 32u, FMT_D24S8, 0u, 0u, 0u, out, 0u), D3D_OK);
    uint32_t b = MEM32(out);
    CHECK(a && b && a != b);
    CHECKEQ(vcall(dev, DV_GetDepthStencilSurface, 1, out), D3D_OK);
    uint32_t autods = MEM32(out);
    CHECK(autods != a && autods != b);
    vcall(autods, 2, 0);
    CHECKEQ(vcall(dev, DV_SetDepthStencilSurface, 1, a), D3D_OK);
    CHECKEQ(vcall(dev, DV_GetDepthStencilSurface, 1, out), D3D_OK);
    CHECKEQ(MEM32(out), a);
    vcall(a, 2, 0);
    CHECKEQ(vcall(dev, DV_Clear, 6, 0u, 0u, CLEAR_ZBUFFER | CLEAR_STENCIL, 0u, f32(0.25f), 7u), D3D_OK);
    CHECKEQ(vcall(dev, DV_SetDepthStencilSurface, 1, 0u), D3D_OK);
    CHECKEQ(vcall(dev, DV_GetDepthStencilSurface, 1, out), D3DERR_NOTFOUND);
    uint32_t desc = gmem(32);
    CHECKEQ(vcall(a, SF_GetDesc, 1, desc), D3D_OK);
    CHECK(MEM32(desc) == FMT_D24S8 && MEM32(desc + 4) == 1 && MEM32(desc + 8) == 2 && MEM32(desc + 24) == 32);
    CHECKEQ(vcall(a, 2, 0), 0);                             /* unbound: our reference was the last */
    CHECKEQ(vcall(b, 2, 0), 0);
}

/* ---- Task 5: textures, surfaces, buffers ---- */

/* IDirect3DTexture9 slots (resource 3-10, base texture 11-16) */
enum { TX_GetDevice = 3, TX_SetPrivateData, TX_GetPrivateData, TX_FreePrivateData, TX_SetPriority, TX_GetPriority,
       TX_PreLoad, TX_GetType, TX_SetLOD, TX_GetLOD, TX_GetLevelCount, TX_SetAutoGenFilterType,
       TX_GetAutoGenFilterType, TX_GenerateMipSubLevels, TX_GetLevelDesc, TX_GetSurfaceLevel, TX_LockRect,
       TX_UnlockRect, TX_AddDirtyRect };
/* IDirect3DVertexBuffer9 / IndexBuffer9 slots */
enum { VB_Lock = 11, VB_Unlock, VB_GetDesc };

#define FMT_X1R5G5B5 24u
#define FMT_A1R5G5B5 25u
#define FMT_A4R4G4B4 26u
#define FMT_L8 50u
#define FMT_A8 28u
#define FMT_R16F 111u
#define FMT_G16R16F 112u
#define FMT_A16B16G16R16F 113u
#define FMT_R32F 114u
#define FMT_A32B32G32R32F 116u
#define FMT_DXT5 0x35545844u
#define FMT_P8 41u
#define FMT_R8G8B8 20u
#define USAGE_RENDERTARGET 1u
#define USAGE_DYNAMIC 0x200u
#define USAGE_WRITEONLY 8u
#define FMT_INDEX16 101u

static uint32_t new_texture(uint32_t dev, uint32_t w, uint32_t h, uint32_t levels, uint32_t usage, uint32_t fmt,
                            uint32_t pool, uint32_t *hr) {
    uint32_t out = gmem(4);
    *hr = vcall(dev, DV_CreateTexture, 8, w, h, levels, usage, fmt, pool, out, 0u);
    return *hr ? 0 : MEM32(out);
}
static uint32_t texture(uint32_t dev, uint32_t w, uint32_t h, uint32_t levels, uint32_t fmt) {
    uint32_t hr, t = new_texture(dev, w, h, levels, 0, fmt, POOL_MANAGED, &hr);
    CHECKEQ(hr, D3D_OK);
    return t;
}
static uint32_t level_surface(uint32_t tex, uint32_t level) {
    uint32_t out = gmem(4);
    CHECKEQ(vcall(tex, TX_GetSurfaceLevel, 2, level, out), D3D_OK);
    return MEM32(out);
}
static uint32_t render_target(uint32_t dev, uint32_t w, uint32_t h) {
    uint32_t out = gmem(4);
    CHECKEQ(vcall(dev, DV_CreateRenderTarget, 8, w, h, FMT_A8R8G8B8, 0u, 0u, 0u, out, 0u), D3D_OK);
    return MEM32(out);
}
static uint16_t half(float f) {                              /* float to IEEE half, round to nearest */
    uint32_t u; memcpy(&u, &f, 4);
    uint32_t s = (u >> 16) & 0x8000, e = (u >> 23) & 0xFF, m = u & 0x7FFFFF;
    if (e < 113) return (uint16_t)s;                         /* the test values are 0 or >= 1/15 */
    uint32_t h = s | ((e - 112) << 10) | (m >> 13);
    if (m & 0x1000) h++;
    return (uint16_t)h;
}

/* Write a 16x1 gradient in fmt: pixel x has every channel at level x of 15. Returns the expected D3DCOLOR. */
static uint32_t gradient_texel(uint32_t fmt, uint32_t x, uint8_t *p) {
    uint32_t v8 = x * 17;                                    /* 0..255 */
    float vf = (float)x / 15.0f;
    switch (fmt) {
    case FMT_A8R8G8B8: p[0] = p[1] = p[2] = p[3] = (uint8_t)v8; return v8 * 0x01010101u;
    case FMT_X8R8G8B8: p[0] = p[1] = p[2] = (uint8_t)v8; p[3] = 0x12; return 0xFF000000u | v8 * 0x010101u;
    case FMT_R5G6B5: {
        uint32_t r = x * 2, g = x * 4, b = x * 2;
        uint16_t v = (uint16_t)((r << 11) | (g << 5) | b);
        memcpy(p, &v, 2);
        uint32_t r8 = (r << 3) | (r >> 2), g8 = (g << 2) | (g >> 4), b8 = (b << 3) | (b >> 2);
        return 0xFF000000u | r8 << 16 | g8 << 8 | b8;
    }
    case FMT_R16F: { uint16_t h = half(vf); memcpy(p, &h, 2); return 0xFF0000FFu | 0xFF00u | ((uint32_t)lroundf(vf * 255) << 16); }
    case FMT_G16R16F: { uint16_t h[2] = { half(vf), half(vf) }; memcpy(p, h, 4);
                        return 0xFF0000FFu | ((uint32_t)lroundf(vf * 255) * 0x10100u); }
    case FMT_A16B16G16R16F: { uint16_t h[4] = { half(vf), half(vf), half(vf), half(vf) }; memcpy(p, h, 8);
                              return (uint32_t)lroundf(vf * 255) * 0x01010101u; }
    case FMT_R32F: memcpy(p, &vf, 4); return 0xFF0000FFu | 0xFF00u | ((uint32_t)lroundf(vf * 255) << 16);
    case FMT_A32B32G32R32F: memcpy(p, &vf, 4); memcpy(p + 4, &vf, 4); memcpy(p + 8, &vf, 4); memcpy(p + 12, &vf, 4);
                            return (uint32_t)lroundf(vf * 255) * 0x01010101u;
    }
    return 0;
}
static int near_color(uint32_t a, uint32_t b, int tol) {
    for (int i = 0; i < 32; i += 8) {
        int x = (int)((a >> i) & 0xFF), y = (int)((b >> i) & 0xFF);
        if (x - y > tol || y - x > tol) return 0;
    }
    return 1;
}

static void group_lock_roundtrip(void) {
    uint32_t dev = new_device(16, 16), rt = render_target(dev, 16, 1);
    static const uint32_t fmts[] = { FMT_A8R8G8B8, FMT_X8R8G8B8, FMT_R5G6B5, FMT_A16B16G16R16F, FMT_R16F,
                                     FMT_G16R16F, FMT_R32F, FMT_A32B32G32R32F };
    for (unsigned i = 0; i < sizeof fmts / sizeof *fmts; i++) {
        uint32_t t = texture(dev, 16, 1, 1, fmts[i]), lr = gmem(8), want[16], px[16];
        CHECKEQ(vcall(t, TX_LockRect, 4, 0u, lr, 0u, 0u), D3D_OK);
        uint8_t *bits = hle_ptr(MEM32(lr + 4));
        uint32_t bpp = MEM32(lr) / 16;
        for (uint32_t x = 0; x < 16; x++) want[x] = gradient_texel(fmts[i], x, bits + x * bpp);
        CHECKEQ(vcall(t, TX_UnlockRect, 1, 0u), D3D_OK);
        uint32_t s = level_surface(t, 0);
        CHECKEQ(vcall(dev, DV_StretchRect, 5, s, 0u, rt, 0u, 1u), D3D_OK);   /* D3DTEXF_POINT */
        read_rt(dev, rt, 16, 1, px);
        for (uint32_t x = 0; x < 16; x++)
            if (!near_color(px[x], want[x], fmts[i] == FMT_A8R8G8B8 || fmts[i] == FMT_X8R8G8B8 || fmts[i] == FMT_R5G6B5 ? 0 : 1))
                printf("FAIL format %u pixel %u: 0x%08X, want 0x%08X\n", fmts[i], x, px[x], want[x]), fails++;
        vcall(s, 2, 0);
        CHECKEQ(vcall(t, 2, 0), 0);
    }
    /* the documented 5-bit expansion: 31 -> 255, 16 -> 132 */
    uint32_t t = texture(dev, 2, 1, 1, FMT_R5G6B5), lr = gmem(8), px[2];
    vcall(t, TX_LockRect, 4, 0u, lr, 0u, 0u);
    uint16_t v[2] = { (uint16_t)(31u << 11), (uint16_t)(16u << 11) };
    memcpy(hle_ptr(MEM32(lr + 4)), v, 4);
    vcall(t, TX_UnlockRect, 1, 0u);
    uint32_t rt2 = render_target(dev, 2, 1), s = level_surface(t, 0);
    CHECKEQ(vcall(dev, DV_StretchRect, 5, s, 0u, rt2, 0u, 1u), D3D_OK);
    read_rt(dev, rt2, 2, 1, px);
    CHECKEQ(px[0], 0xFFFF0000u);
    CHECKEQ(px[1], 0xFF840000u);
}

static void group_pitches(void) {
    uint32_t dev = new_device(16, 16);
    static const uint32_t fmts[] = { FMT_A8R8G8B8, FMT_R5G6B5, FMT_DXT1, FMT_DXT5 };
    static const uint32_t sizes[][2] = { { 256, 256 }, { 100, 60 }, { 3, 3 } };
    for (unsigned f = 0; f < 4; f++)
        for (unsigned z = 0; z < 3; z++) {
            uint32_t w = sizes[z][0], h = sizes[z][1], t = texture(dev, w, h, 0, fmts[f]);
            uint32_t n = vcall(t, TX_GetLevelCount, 0), want_n = 1;
            for (uint32_t m = w > h ? w : h; m > 1; m >>= 1) want_n++;
            CHECKEQ(n, want_n);
            for (uint32_t l = 0; l < n; l++) {
                uint32_t lw = w >> l ? w >> l : 1, lh = h >> l ? h >> l : 1, desc = gmem(32), lr = gmem(8);
                CHECKEQ(vcall(t, TX_GetLevelDesc, 2, l, desc), D3D_OK);
                CHECK(MEM32(desc + 24) == lw && MEM32(desc + 28) == lh && MEM32(desc) == fmts[f]);
                CHECKEQ(vcall(t, TX_LockRect, 4, l, lr, 0u, 0u), D3D_OK);
                uint32_t want = fmts[f] == FMT_A8R8G8B8 ? lw * 4 : fmts[f] == FMT_R5G6B5 ? lw * 2
                              : ((lw + 3) / 4) * (fmts[f] == FMT_DXT1 ? 8u : 16u);
                if (MEM32(lr) != want) printf("FAIL fmt %X %ux%u level %u pitch %u want %u\n", fmts[f], w, h, l, MEM32(lr), want), fails++;
                CHECKEQ(vcall(t, TX_UnlockRect, 1, l), D3D_OK);
            }
            CHECKEQ(vcall(t, TX_LockRect, 4, n, gmem(8), 0u, 0u), D3DERR_INVALIDCALL);
            vcall(t, 2, 0);
        }
    uint32_t t = texture(dev, 256, 256, 0, FMT_DXT1), lr = gmem(8);
    vcall(t, TX_LockRect, 4, 0u, lr, 0u, 0u);
    CHECKEQ(MEM32(lr), 512);
    vcall(t, TX_UnlockRect, 1, 0u);
}

static void group_dxt_small_mips(void) {
    uint32_t dev = new_device(16, 16), t = texture(dev, 8, 8, 4, FMT_DXT1);
    CHECKEQ(vcall(t, TX_GetLevelCount, 0), 4);
    for (uint32_t l = 0; l < 4; l++) {
        uint32_t lr = gmem(8);
        CHECKEQ(vcall(t, TX_LockRect, 4, l, lr, 0u, 0u), D3D_OK);
        CHECKEQ(MEM32(lr), l == 0 ? 16u : 8u);              /* two blocks per row at 8x8, one at 4x4 and below */
        uint8_t *b = hle_ptr(MEM32(lr + 4));
        uint32_t blocks = l == 0 ? 4 : 1;
        for (uint32_t i = 0; i < blocks * 8; i++) b[i] = (uint8_t)(0x40 + i);
        CHECKEQ(vcall(t, TX_UnlockRect, 1, l), D3D_OK);
    }
    /* a lock reads back what was written */
    uint32_t lr = gmem(8);
    vcall(t, TX_LockRect, 4, 3u, lr, 0u, 0x10u);
    CHECKEQ(*(uint8_t *)hle_ptr(MEM32(lr + 4)), 0x40);
    vcall(t, TX_UnlockRect, 1, 3u);
    mtl_flush(((struct d3d_device *)d3d_host(dev, D3DK_DEVICE))->mtl);   /* the uploads reach the GPU cleanly */
}

static void group_zeroed(void) {
    uint32_t dev = new_device(16, 16), t = texture(dev, 8, 8, 1, FMT_A8R8G8B8), lr = gmem(8);
    CHECKEQ(vcall(t, TX_LockRect, 4, 0u, lr, 0u, 0x10u), D3D_OK);
    int nz = 0;
    for (uint32_t i = 0; i < 8 * 8 * 4; i++) nz += ((uint8_t *)hle_ptr(MEM32(lr + 4)))[i] != 0;
    CHECKEQ(nz, 0);
    vcall(t, TX_UnlockRect, 1, 0u);
    /* the GPU copy is zero too */
    uint32_t rt = render_target(dev, 8, 8), s = level_surface(t, 0), px[64];
    CHECKEQ(vcall(dev, DV_Clear, 6, 0u, 0u, CLEAR_TARGET, 0xFFFFFFFFu, f32(1), 0u), D3D_OK);
    CHECKEQ(vcall(dev, DV_StretchRect, 5, s, 0u, rt, 0u, 1u), D3D_OK);
    read_rt(dev, rt, 8, 8, px);
    CHECKEQ(px[0], 0);
    CHECKEQ(px[63], 0);
    uint32_t out = gmem(4), ptr = gmem(4);
    CHECKEQ(vcall(dev, DV_CreateVertexBuffer, 6, 4096u, USAGE_WRITEONLY, 0u, POOL_MANAGED, out, 0u), D3D_OK);
    uint32_t vb = MEM32(out);
    CHECKEQ(vcall(vb, VB_Lock, 4, 0u, 0u, ptr, 0u), D3D_OK);
    nz = 0;
    for (uint32_t i = 0; i < 4096; i++) nz += ((uint8_t *)hle_ptr(MEM32(ptr)))[i] != 0;
    CHECKEQ(nz, 0);
    CHECKEQ(vcall(vb, VB_Unlock, 0), D3D_OK);
    CHECKEQ(vcall(dev, DV_CreateIndexBuffer, 6, 600u, USAGE_DYNAMIC | USAGE_WRITEONLY, FMT_INDEX16, POOL_DEFAULT, out, 0u), D3D_OK);
    uint32_t ib = MEM32(out), desc = gmem(24);
    CHECKEQ(vcall(ib, VB_Lock, 4, 100u, 50u, ptr, 0x2000u), D3D_OK);    /* DISCARD */
    nz = 0;
    for (uint32_t i = 0; i < 50; i++) nz += ((uint8_t *)hle_ptr(MEM32(ptr)))[i] != 0;
    CHECKEQ(nz, 0);
    CHECKEQ(vcall(ib, VB_Unlock, 0), D3D_OK);
    CHECKEQ(vcall(ib, VB_GetDesc, 1, desc), D3D_OK);
    CHECK(MEM32(desc) == FMT_INDEX16 && MEM32(desc + 4) == 7 && MEM32(desc + 12) == POOL_DEFAULT && MEM32(desc + 16) == 600);
    /* a DEFAULT index buffer blocks Reset until released */
    CHECKEQ(vcall(dev, DV_Reset, 1, pp_new(16, 16, FMT_X8R8G8B8, 1)), D3DERR_INVALIDCALL);
    CHECKEQ(vcall(ib, 2, 0), 0);
    vcall(s, 2, 0);
    vcall(rt, 2, 0);
    CHECKEQ(vcall(dev, DV_Reset, 1, pp_new(16, 16, FMT_X8R8G8B8, 1)), D3D_OK);
}

static void group_release_while_bound(void) {
    uint32_t dev = new_device(16, 16), t = texture(dev, 4, 4, 1, FMT_A8R8G8B8), out = gmem(4);
    CHECKEQ(vcall(dev, DV_SetTexture, 2, 0u, t), D3D_OK);
    CHECKEQ(vcall(t, 2, 0), 1);                              /* the device's reference */
    CHECK(d3d_alive(t));
    CHECKEQ(vcall(dev, DV_GetTexture, 2, 0u, out), D3D_OK);
    CHECKEQ(MEM32(out), t);
    CHECKEQ(vcall(t, 2, 0), 1);
    CHECKEQ(vcall(dev, DV_SetTexture, 2, 0u, 0u), D3D_OK);
    CHECK(!d3d_alive(t));
    CHECKEQ(vcall(dev, DV_GetTexture, 2, 0u, out), D3D_OK);
    CHECKEQ(MEM32(out), 0);
    /* streams and indices hold their buffers the same way */
    CHECKEQ(vcall(dev, DV_CreateVertexBuffer, 6, 256u, 0u, 0u, POOL_MANAGED, out, 0u), D3D_OK);
    uint32_t vb = MEM32(out);
    CHECKEQ(vcall(dev, DV_SetStreamSource, 4, 0u, vb, 16u, 32u), D3D_OK);
    CHECKEQ(vcall(vb, 2, 0), 1);
    uint32_t off = gmem(4), stride = gmem(4);
    CHECKEQ(vcall(dev, DV_GetStreamSource, 4, 0u, out, off, stride), D3D_OK);
    CHECK(MEM32(out) == vb && MEM32(off) == 16 && MEM32(stride) == 32);
    vcall(vb, 2, 0);
    CHECKEQ(vcall(dev, DV_SetStreamSource, 4, 0u, 0u, 0u, 0u), D3D_OK);
    CHECK(!d3d_alive(vb));
    /* the volume and cube textures exist */
    CHECKEQ(vcall(dev, DV_CreateCubeTexture, 7, 16u, 0u, 0u, FMT_DXT5, POOL_MANAGED, out, 0u), D3D_OK);
    uint32_t cube = MEM32(out);
    CHECKEQ(vcall(cube, TX_GetLevelCount, 0), 5);
    CHECKEQ(vcall(cube, 2, 0), 0);
    CHECKEQ(vcall(dev, DV_CreateVolumeTexture, 9, 8u, 8u, 4u, 0u, 0u, FMT_A8R8G8B8, POOL_MANAGED, out, 0u), D3D_OK);
    uint32_t vol = MEM32(out), lb = gmem(12);
    CHECKEQ(vcall(vol, TX_GetLevelCount, 0), 4);
    CHECKEQ(vcall(vol, TX_LockRect, 4, 1u, lb, 0u, 0u), D3D_OK);        /* LockBox, level 1: 4x4x2 */
    CHECK(MEM32(lb) == 16 && MEM32(lb + 4) == 64);
    CHECKEQ(vcall(vol, TX_UnlockRect, 1, 1u), D3D_OK);
    CHECKEQ(vcall(vol, 2, 0), 0);
}

static void group_child_refs(void) {
    uint32_t dev = new_device(16, 16), t = texture(dev, 8, 8, 0, FMT_A8R8G8B8);
    uint32_t s = level_surface(t, 1), again = level_surface(t, 1);
    CHECKEQ(s, again);                                       /* one surface per level */
    vcall(again, 2, 0);
    CHECKEQ(vcall(t, 2, 0), 1);                              /* the surface holds its texture */
    CHECK(d3d_alive(t));
    uint32_t desc = gmem(32), out = gmem(4);
    CHECKEQ(vcall(s, SF_GetDesc, 1, desc), D3D_OK);
    CHECK(MEM32(desc + 24) == 4 && MEM32(desc + 28) == 4);
    static const uint8_t IID_IDirect3DTexture9[16] = { 0x27, 0x12, 0xC3, 0x85, 0xE5, 0x3D, 0x00, 0x4F,
                                                       0x9B, 0x3A, 0xF1, 0x1A, 0xC3, 0x8C, 0x18, 0xB5 };
    uint32_t iid = gmem(16);
    memcpy(hle_ptr(iid), IID_IDirect3DTexture9, 16);
    CHECKEQ(vcall(s, SF_GetContainer, 2, iid, out), D3D_OK);
    CHECKEQ(MEM32(out), t);
    vcall(t, 2, 0);
    CHECKEQ(vcall(s, 2, 0), 0);
    CHECK(!d3d_alive(t) && !d3d_alive(s));
    /* private data travels with the object */
    uint32_t t2 = texture(dev, 4, 4, 1, FMT_A8R8G8B8), guid = gmem(16), data = gmem(8), size = gmem(4);
    memset(hle_ptr(guid), 0x5A, 16);
    MEM32(data) = 0xCAFEF00Du;
    CHECKEQ(vcall(t2, TX_SetPrivateData, 4, guid, data, 4u, 0u), D3D_OK);
    MEM32(data) = 0, MEM32(size) = 2;
    CHECKEQ(vcall(t2, TX_GetPrivateData, 3, guid, data, size), D3DERR_MOREDATA);
    MEM32(size) = 4;
    CHECKEQ(vcall(t2, TX_GetPrivateData, 3, guid, data, size), D3D_OK);
    CHECKEQ(MEM32(data), 0xCAFEF00Du);
    CHECKEQ(vcall(t2, TX_FreePrivateData, 1, guid), D3D_OK);
    CHECKEQ(vcall(t2, TX_GetPrivateData, 3, guid, data, size), D3DERR_NOTFOUND);
    CHECKEQ(vcall(t2, TX_GetType, 0), 3);
    vcall(t2, 2, 0);
}

static void group_formats_check(void) {
    uint32_t dev = new_device(16, 16), d = g_d3d9;
    static const uint32_t fmts[] = { FMT_A8R8G8B8, FMT_X8R8G8B8, FMT_R5G6B5, FMT_X1R5G5B5, FMT_A1R5G5B5, FMT_A4R4G4B4,
        FMT_L8, FMT_A8, 51, 60, 63, 34, 36, FMT_R16F, FMT_G16R16F, FMT_A16B16G16R16F, FMT_R32F, 115,
        FMT_A32B32G32R32F, FMT_DXT1, 0x32545844u, 0x33545844u, 0x34545844u, FMT_DXT5, FMT_D16, FMT_D24S8, 77, 79, 71,
        FMT_P8, FMT_R8G8B8, FMT_D16_LOCKABLE, 0x59565955u /* UYVY */ };
    for (unsigned i = 0; i < sizeof fmts / sizeof *fmts; i++)
        for (uint32_t usage = 0; usage <= 1; usage++) {
            uint32_t ok = vcall(d, D9_CheckDeviceFormat, 6, 0u, HAL, FMT_X8R8G8B8, usage, RT_TEXTURE, fmts[i]);
            uint32_t hr, t = new_texture(dev, 8, 8, 1, usage, fmts[i], usage ? POOL_DEFAULT : POOL_MANAGED, &hr);
            if ((ok == D3D_OK) != (hr == D3D_OK))
                printf("FAIL format 0x%X usage %u: CheckDeviceFormat 0x%X, CreateTexture 0x%X\n", fmts[i], usage, ok, hr), fails++;
            if (t) vcall(t, 2, 0);
        }
}

/* ---- Task 6: draws and the fixed-function pipeline ---- */

#define FVF_XYZ 0x002u
#define FVF_XYZRHW 0x004u
#define FVF_NORMAL 0x010u
#define FVF_DIFFUSE 0x040u
#define FVF_SPECULAR 0x080u
#define FVF_TEX1 0x100u
#define PT_POINTLIST 1u
#define PT_LINELIST 2u
#define PT_LINESTRIP 3u
#define PT_TRIANGLELIST 4u
#define PT_TRIANGLESTRIP 5u
#define PT_TRIANGLEFAN 6u
#define RS_ZENABLE 7u
#define RS_ZWRITEENABLE 14u
#define RS_ALPHATESTENABLE 15u
#define RS_SRCBLEND 19u
#define RS_DESTBLEND 20u
#define RS_CULLMODE 22u
#define RS_ZFUNC 23u
#define RS_ALPHAREF 24u
#define RS_ALPHAFUNC 25u
#define RS_ALPHABLENDENABLE 27u
#define RS_FOGENABLE 28u
#define RS_SPECULARENABLE 29u
#define RS_FOGCOLOR 34u
#define RS_FOGTABLEMODE 35u
#define RS_FOGSTART 36u
#define RS_FOGEND 37u
#define RS_STENCILENABLE 52u
#define RS_STENCILPASS 55u
#define RS_STENCILFUNC 56u
#define RS_STENCILREF 57u
#define RS_TEXTUREFACTOR 60u
#define RS_LIGHTING 137u
#define RS_AMBIENT 139u
#define RS_BLENDOP 171u
#define TSS_COLOROP 1u
#define TSS_COLORARG1 2u
#define TSS_COLORARG2 3u
#define TSS_ALPHAOP 4u
#define TSS_ALPHAARG1 5u
#define TSS_ALPHAARG2 6u
#define SS_ADDRESSU 1u
#define SS_ADDRESSV 2u
#define SS_MAGFILTER 5u
#define SS_MINFILTER 6u
#define SS_MIPFILTER 7u
#define SS_MAXMIPLEVEL 9u
#define SS_MAXANISOTROPY 10u

/* pretransformed, coloured: x, y, z, rhw, diffuse */
struct tlv { float x, y, z, rhw; uint32_t c; };
/* pretransformed, coloured, textured */
struct tlvt { float x, y, z, rhw; uint32_t c; float u, v; };

static uint32_t up(const void *v, uint32_t bytes) { uint32_t p = gmem(bytes); memcpy(hle_ptr(p), v, bytes); return p; }
static void draw_up(uint32_t dev, uint32_t type, uint32_t prims, const void *v, uint32_t nverts, uint32_t stride) {
    CHECKEQ(vcall(dev, DV_DrawPrimitiveUP, 4, type, prims, up(v, nverts * stride), stride), D3D_OK);
}
static void quad_tlv(uint32_t dev, float x0, float y0, float x1, float y1, float z, uint32_t c) {
    struct tlv q[4] = { { x0, y0, z, 1, c }, { x1, y0, z, 1, c }, { x0, y1, z, 1, c }, { x1, y1, z, 1, c } };
    CHECKEQ(vcall(dev, DV_SetFVF, 1, FVF_XYZRHW | FVF_DIFFUSE), D3D_OK);
    draw_up(dev, PT_TRIANGLESTRIP, 2, q, 4, sizeof q[0]);
}
static uint32_t count_color(const uint32_t *px, uint32_t n, uint32_t c) {
    uint32_t k = 0;
    for (uint32_t i = 0; i < n; i++) k += px[i] == c;
    return k;
}
static void clear(uint32_t dev, uint32_t c) {
    CHECKEQ(vcall(dev, DV_Clear, 6, 0u, 0u, CLEAR_TARGET | CLEAR_ZBUFFER | CLEAR_STENCIL, c, f32(1.0f), 0u), D3D_OK);
}
static void rs(uint32_t dev, uint32_t s, uint32_t v) { CHECKEQ(vcall(dev, DV_SetRenderState, 2, s, v), D3D_OK); }
static void tss(uint32_t dev, uint32_t st, uint32_t s, uint32_t v) { CHECKEQ(vcall(dev, DV_SetTextureStageState, 3, st, s, v), D3D_OK); }
static void sst(uint32_t dev, uint32_t smp, uint32_t s, uint32_t v) { CHECKEQ(vcall(dev, DV_SetSamplerState, 3, smp, s, v), D3D_OK); }
static uint32_t *frame(uint32_t dev, uint32_t w, uint32_t h) {
    static uint32_t buf[256 * 256];
    read_rt(dev, back_buffer(dev), w, h, buf);
    return buf;
}

static void group_tri_colour(void) {
    uint32_t dev = new_device(64, 64);
    clear(dev, 0xFF000000u);
    struct tlv t[3] = { { 0, 0, 0.5f, 1, 0xFFFF0000u }, { 64, 0, 0.5f, 1, 0xFF00FF00u }, { 0, 64, 0.5f, 1, 0xFF0000FFu } };
    CHECKEQ(vcall(dev, DV_SetFVF, 1, FVF_XYZRHW | FVF_DIFFUSE), D3D_OK);
    draw_up(dev, PT_TRIANGLELIST, 1, t, 3, sizeof t[0]);
    uint32_t *px = frame(dev, 64, 64);
    /* pixel (10, 50): the centre is (10, 50) in D3D9; weights 4/64 red, 10/64 green, 50/64 blue */
    uint32_t want = 0xFF000000u | (uint32_t)(255 * 4 / 64.0 + 0.5) << 16 | (uint32_t)(255 * 10 / 64.0 + 0.5) << 8 |
                    (uint32_t)(255 * 50 / 64.0 + 0.5);
    if (!near_color(px[50 * 64 + 10], want, 2)) printf("FAIL pixel (10,50) 0x%08X want 0x%08X\n", px[50 * 64 + 10], want), fails++;
    CHECKEQ(px[63 * 64 + 63], 0xFF000000u);
    /* a quad from (0,0) to (32,32) covers exactly 32x32 pixels (top-left rule) */
    clear(dev, 0xFF000000u);
    quad_tlv(dev, 0, 0, 32, 32, 0.5f, 0xFFFFFFFFu);
    px = frame(dev, 64, 64);
    CHECKEQ(count_color(px, 64 * 64, 0xFFFFFFFFu), 32 * 32);
    CHECKEQ(px[31 * 64 + 31], 0xFFFFFFFFu);
    CHECKEQ(px[32 * 64 + 31], 0xFF000000u);
}

static uint32_t texture_argb(uint32_t dev, uint32_t w, uint32_t h, const uint32_t *texels) {
    uint32_t t = texture(dev, w, h, 1, FMT_A8R8G8B8), lr = gmem(8);
    CHECKEQ(vcall(t, TX_LockRect, 4, 0u, lr, 0u, 0u), D3D_OK);
    for (uint32_t y = 0; y < h; y++) memcpy((uint8_t *)hle_ptr(MEM32(lr + 4)) + y * MEM32(lr), texels + y * w, w * 4);
    CHECKEQ(vcall(t, TX_UnlockRect, 1, 0u), D3D_OK);
    return t;
}
static void quad_tex(uint32_t dev, float x0, float y0, float x1, float y1, float u0, float v0, float u1, float v1, uint32_t c) {
    struct tlvt q[4] = { { x0, y0, 0.5f, 1, c, u0, v0 }, { x1, y0, 0.5f, 1, c, u1, v0 },
                         { x0, y1, 0.5f, 1, c, u0, v1 }, { x1, y1, 0.5f, 1, c, u1, v1 } };
    CHECKEQ(vcall(dev, DV_SetFVF, 1, FVF_XYZRHW | FVF_DIFFUSE | FVF_TEX1), D3D_OK);
    draw_up(dev, PT_TRIANGLESTRIP, 2, q, 4, sizeof q[0]);
}

static void group_half_pixel(void) {
    uint32_t dev = new_device(4, 4), tx[16];
    for (int i = 0; i < 16; i++) tx[i] = 0xFF000000u | (uint32_t)(i * 16) << 16 | (uint32_t)(255 - i * 16) << 8 | (uint32_t)(i * 5);
    uint32_t t = texture_argb(dev, 4, 4, tx);
    CHECKEQ(vcall(dev, DV_SetTexture, 2, 0u, t), D3D_OK);
    sst(dev, 0, SS_MAGFILTER, 2), sst(dev, 0, SS_MINFILTER, 2);   /* linear: an offset would mix texels */
    clear(dev, 0xFF000000u);
    /* the D3D9 1:1 mapping: a quad from (-0.5, -0.5) puts texel centres on pixel centres */
    quad_tex(dev, -0.5f, -0.5f, 3.5f, 3.5f, 0, 0, 1, 1, 0xFFFFFFFFu);
    uint32_t *px = frame(dev, 4, 4);
    for (int i = 0; i < 16; i++)
        if (!near_color(px[i], tx[i], 1)) printf("FAIL pixel %d 0x%08X want 0x%08X\n", i, px[i], tx[i]), fails++;
}

static void mat_identity(float *m) { memset(m, 0, 64); m[0] = m[5] = m[10] = m[15] = 1; }
static void set_xform(uint32_t dev, uint32_t which, const float *m) {
    uint32_t p = gmem(64);
    memcpy(hle_ptr(p), m, 64);
    CHECKEQ(vcall(dev, DV_SetTransform, 2, which, p), D3D_OK);
}
/* D3DXMatrixPerspectiveFovLH */
static void mat_persp(float *m, float fovy, float aspect, float zn, float zf) {
    memset(m, 0, 64);
    float ys = 1.0f / tanf(fovy / 2), xs = ys / aspect, q = zf / (zf - zn);
    m[0] = xs, m[5] = ys, m[10] = q, m[11] = 1, m[14] = -zn * q;
}
struct xyzc { float x, y, z; uint32_t c; };

static void group_transforms(void) {
    uint32_t dev = new_device(64, 64);
    float w[16], v[16], p[16];
    mat_identity(w), mat_identity(v);
    mat_persp(p, 3.14159265f / 2, 1, 1, 10);
    set_xform(dev, 256, w), set_xform(dev, 2, v), set_xform(dev, 3, p);
    rs(dev, RS_LIGHTING, 0);
    /* a 2x2 square at z = 5: NDC +-0.2, screen 25.6 to 38.4; D3D9 pixel centres 26..38 inside */
    struct xyzc q[4] = { { -1, 1, 5, 0xFFFFFFFFu }, { 1, 1, 5, 0xFFFFFFFFu }, { -1, -1, 5, 0xFFFFFFFFu }, { 1, -1, 5, 0xFFFFFFFFu } };
    CHECKEQ(vcall(dev, DV_SetFVF, 1, FVF_XYZ | FVF_DIFFUSE), D3D_OK);
    clear(dev, 0xFF000000u);
    draw_up(dev, PT_TRIANGLESTRIP, 2, q, 4, sizeof q[0]);
    uint32_t *px = frame(dev, 64, 64);
    CHECKEQ(count_color(px, 64 * 64, 0xFFFFFFFFu), 13 * 13);
    CHECKEQ(px[26 * 64 + 26], 0xFFFFFFFFu);
    CHECKEQ(px[38 * 64 + 38], 0xFFFFFFFFu);
    CHECKEQ(px[25 * 64 + 26], 0xFF000000u);
    /* world moves it right by one unit: 6.4 pixels; the view is applied after the world */
    w[12] = 1.0f;
    set_xform(dev, 256, w);
    v[13] = 1.0f;                                           /* the view moves everything up one unit */
    set_xform(dev, 2, v);
    clear(dev, 0xFF000000u);
    draw_up(dev, PT_TRIANGLESTRIP, 2, q, 4, sizeof q[0]);
    px = frame(dev, 64, 64);
    int minx = 64, maxx = -1, miny = 64, maxy = -1;
    for (int y = 0; y < 64; y++)
        for (int x = 0; x < 64; x++)
            if (px[y * 64 + x] == 0xFFFFFFFFu) {
                if (x < minx) minx = x;
                if (x > maxx) maxx = x;
                if (y < miny) miny = y;
                if (y > maxy) maxy = y;
            }
    /* x 32.0 to 44.8: the left edge is in (top-left rule), centres 32..44; y 19.2 to 32.0: centres 20..31 */
    if (!(minx == 32 && maxx == 44 && miny == 20 && maxy == 31))
        printf("FAIL moved square at x %d..%d y %d..%d, want 32..44 20..31\n", minx, maxx, miny, maxy), fails++;
    /* culling: D3D's default culls counter-clockwise triangles (as seen on screen) */
    mat_identity(w), mat_identity(v);
    set_xform(dev, 256, w), set_xform(dev, 2, v);
    clear(dev, 0xFF000000u);
    struct tlv cw[3] = { { 0, 0, 0.5f, 1, 0xFFFFFFFFu }, { 16, 0, 0.5f, 1, 0xFFFFFFFFu }, { 0, 16, 0.5f, 1, 0xFFFFFFFFu } };
    struct tlv ccw[3] = { { 32, 32, 0.5f, 1, 0xFFFFFFFFu }, { 32, 48, 0.5f, 1, 0xFFFFFFFFu }, { 48, 32, 0.5f, 1, 0xFFFFFFFFu } };
    CHECKEQ(vcall(dev, DV_SetFVF, 1, FVF_XYZRHW | FVF_DIFFUSE), D3D_OK);
    draw_up(dev, PT_TRIANGLELIST, 1, cw, 3, sizeof cw[0]);
    draw_up(dev, PT_TRIANGLELIST, 1, ccw, 3, sizeof ccw[0]);
    px = frame(dev, 64, 64);
    CHECKEQ(px[2 * 64 + 2], 0xFFFFFFFFu);
    CHECKEQ(px[34 * 64 + 34], 0xFF000000u);
    rs(dev, RS_CULLMODE, 2);                                 /* CULL_CW */
    clear(dev, 0xFF000000u);
    draw_up(dev, PT_TRIANGLELIST, 1, cw, 3, sizeof cw[0]);
    draw_up(dev, PT_TRIANGLELIST, 1, ccw, 3, sizeof ccw[0]);
    px = frame(dev, 64, 64);
    CHECKEQ(px[2 * 64 + 2], 0xFF000000u);
    CHECKEQ(px[34 * 64 + 34], 0xFFFFFFFFu);
}

static void group_primitive_types(void) {
    uint32_t dev = new_device(64, 64);
    CHECKEQ(vcall(dev, DV_SetFVF, 1, FVF_XYZRHW | FVF_DIFFUSE), D3D_OK);
    uint32_t W = 0xFFFFFFFFu, B = 0xFF000000u;
    /* points */
    clear(dev, B);
    struct tlv pt[2] = { { 5, 5, 0.5f, 1, W }, { 40, 20, 0.5f, 1, W } };
    draw_up(dev, PT_POINTLIST, 2, pt, 2, sizeof pt[0]);
    uint32_t *px = frame(dev, 64, 64);
    CHECKEQ(px[5 * 64 + 5], W);
    CHECKEQ(px[20 * 64 + 40], W);
    CHECKEQ(count_color(px, 64 * 64, W), 2);
    /* lines: a list and a strip */
    clear(dev, B);
    struct tlv ln[2] = { { 2, 10, 0.5f, 1, W }, { 20, 10, 0.5f, 1, W } };
    draw_up(dev, PT_LINELIST, 1, ln, 2, sizeof ln[0]);
    struct tlv ls[3] = { { 2, 30, 0.5f, 1, W }, { 20, 30, 0.5f, 1, W }, { 20, 50, 0.5f, 1, W } };
    draw_up(dev, PT_LINESTRIP, 2, ls, 3, sizeof ls[0]);
    px = frame(dev, 64, 64);
    for (int x = 4; x < 18; x++) CHECKEQ(px[10 * 64 + x], W);
    for (int x = 4; x < 18; x++) CHECKEQ(px[30 * 64 + x], W);
    for (int y = 32; y < 48; y++) CHECKEQ(px[y * 64 + 20], W);
    CHECKEQ(px[12 * 64 + 10], B);
    /* a strip and a fan cover what the same triangles as a list cover */
    struct tlv fan[6] = { { 32, 32, 0.5f, 1, W }, { 32, 4, 0.5f, 1, W }, { 56, 18, 0.5f, 1, W }, { 56, 46, 0.5f, 1, W },
                          { 32, 60, 0.5f, 1, W }, { 8, 46, 0.5f, 1, W } };
    struct tlv list[12];
    for (int i = 0; i < 4; i++) list[3 * i] = fan[0], list[3 * i + 1] = fan[i + 1], list[3 * i + 2] = fan[i + 2];
    static uint32_t a[64 * 64], b[64 * 64];
    clear(dev, B);
    draw_up(dev, PT_TRIANGLEFAN, 4, fan, 6, sizeof fan[0]);
    memcpy(a, frame(dev, 64, 64), sizeof a);
    clear(dev, B);
    draw_up(dev, PT_TRIANGLELIST, 4, list, 12, sizeof list[0]);
    memcpy(b, frame(dev, 64, 64), sizeof b);
    CHECK(!memcmp(a, b, sizeof a));
    CHECK(count_color(a, 64 * 64, W) > 1000);
    struct tlv strip[4] = { { 0, 40, 0.5f, 1, W }, { 20, 40, 0.5f, 1, W }, { 0, 60, 0.5f, 1, W }, { 20, 60, 0.5f, 1, W } };
    struct tlv slist[6] = { strip[0], strip[1], strip[2], strip[2], strip[1], strip[3] };
    clear(dev, B);
    draw_up(dev, PT_TRIANGLESTRIP, 2, strip, 4, sizeof strip[0]);
    memcpy(a, frame(dev, 64, 64), sizeof a);
    clear(dev, B);
    draw_up(dev, PT_TRIANGLELIST, 2, slist, 6, sizeof slist[0]);
    memcpy(b, frame(dev, 64, 64), sizeof b);
    CHECK(!memcmp(a, b, sizeof a));
    CHECKEQ(count_color(a, 64 * 64, W), 20 * 20);
    /* an indexed fan from an index buffer, with a base vertex */
    uint32_t out = gmem(4), ptr = gmem(4);
    CHECKEQ(vcall(dev, DV_CreateVertexBuffer, 6, (uint32_t)sizeof(struct tlv) * 8, 0u, 0u, POOL_MANAGED, out, 0u), D3D_OK);
    uint32_t vb = MEM32(out);
    vcall(vb, VB_Lock, 4, 0u, 0u, ptr, 0u);
    memcpy((uint8_t *)hle_ptr(MEM32(ptr)) + 2 * sizeof(struct tlv), fan, sizeof fan);
    vcall(vb, VB_Unlock, 0);
    CHECKEQ(vcall(dev, DV_CreateIndexBuffer, 6, 32u, 0u, FMT_INDEX16, POOL_MANAGED, out, 0u), D3D_OK);
    uint32_t ib = MEM32(out);
    vcall(ib, VB_Lock, 4, 0u, 0u, ptr, 0u);
    uint16_t idx[8] = { 9, 0, 1, 2, 3, 4, 5, 9 };
    memcpy(hle_ptr(MEM32(ptr)), idx, sizeof idx);
    vcall(ib, VB_Unlock, 0);
    CHECKEQ(vcall(dev, DV_SetStreamSource, 4, 0u, vb, 0u, (uint32_t)sizeof(struct tlv)), D3D_OK);
    CHECKEQ(vcall(dev, DV_SetIndices, 1, ib), D3D_OK);
    clear(dev, B);
    CHECKEQ(vcall(dev, DV_DrawIndexedPrimitive, 6, PT_TRIANGLEFAN, 2u, 0u, 6u, 1u, 4u), D3D_OK);
    memcpy(b, frame(dev, 64, 64), sizeof b);
    clear(dev, B);
    draw_up(dev, PT_TRIANGLEFAN, 4, fan, 6, sizeof fan[0]);
    memcpy(a, frame(dev, 64, 64), sizeof a);
    CHECK(!memcmp(a, b, sizeof a));
    /* the UP draw unbound stream 0, as documented */
    uint32_t off = gmem(4), stride = gmem(4);
    CHECKEQ(vcall(dev, DV_GetStreamSource, 4, 0u, out, off, stride), D3D_OK);
    CHECKEQ(MEM32(out), 0);
    vcall(vb, 2, 0), vcall(ib, 2, 0);
}

static void group_dynamic_discard(void) {
    uint32_t dev = new_device(256, 8), out = gmem(4), ptr = gmem(4);
    CHECKEQ(vcall(dev, DV_CreateVertexBuffer, 6, (uint32_t)sizeof(struct tlv) * 4, USAGE_DYNAMIC | USAGE_WRITEONLY,
                  FVF_XYZRHW | FVF_DIFFUSE, POOL_DEFAULT, out, 0u), D3D_OK);
    uint32_t vb = MEM32(out);
    CHECKEQ(vcall(dev, DV_SetFVF, 1, FVF_XYZRHW | FVF_DIFFUSE), D3D_OK);
    CHECKEQ(vcall(dev, DV_SetStreamSource, 4, 0u, vb, 0u, (uint32_t)sizeof(struct tlv)), D3D_OK);
    clear(dev, 0xFF000000u);
    for (int i = 0; i < 50; i++) {
        CHECKEQ(vcall(vb, VB_Lock, 4, 0u, 0u, ptr, 0x2000u), D3D_OK);      /* DISCARD */
        float x0 = (float)(5 * i), x1 = x0 + 3;
        struct tlv q[4] = { { x0, 2, 0.5f, 1, 0xFFFFFFFFu }, { x1, 2, 0.5f, 1, 0xFFFFFFFFu },
                            { x0, 6, 0.5f, 1, 0xFFFFFFFFu }, { x1, 6, 0.5f, 1, 0xFFFFFFFFu } };
        memcpy(hle_ptr(MEM32(ptr)), q, sizeof q);
        CHECKEQ(vcall(vb, VB_Unlock, 0), D3D_OK);
        CHECKEQ(vcall(dev, DV_DrawPrimitive, 3, PT_TRIANGLESTRIP, 0u, 2u), D3D_OK);
    }
    uint32_t *px = frame(dev, 256, 8);
    int missing = 0;
    for (int i = 0; i < 50; i++) missing += px[4 * 256 + 5 * i + 1] != 0xFFFFFFFFu;
    CHECKEQ(missing, 0);
    CHECKEQ(count_color(px, 256 * 8, 0xFFFFFFFFu), 50 * 3 * 4);
    /* a plain lock while a draw still has to read the buffer keeps the old bytes for that draw too */
    clear(dev, 0xFF000000u);
    CHECKEQ(vcall(vb, VB_Lock, 4, 0u, 0u, ptr, 0u), D3D_OK);
    struct tlv q[4] = { { 0, 0, 0.5f, 1, 0xFF00FF00u }, { 4, 0, 0.5f, 1, 0xFF00FF00u }, { 0, 4, 0.5f, 1, 0xFF00FF00u },
                        { 4, 4, 0.5f, 1, 0xFF00FF00u } };
    memcpy(hle_ptr(MEM32(ptr)), q, sizeof q);
    vcall(vb, VB_Unlock, 0);
    vcall(dev, DV_DrawPrimitive, 3, PT_TRIANGLESTRIP, 0u, 2u);
    CHECKEQ(vcall(vb, VB_Lock, 4, 0u, 0u, ptr, 0u), D3D_OK);
    struct tlv *qq = hle_ptr(MEM32(ptr));
    CHECKEQ(qq[0].c, 0xFF00FF00u);                           /* the copy keeps what was there */
    for (int i = 0; i < 4; i++) qq[i].x += 100, qq[i].c = 0xFFFF0000u;
    vcall(vb, VB_Unlock, 0);
    vcall(dev, DV_DrawPrimitive, 3, PT_TRIANGLESTRIP, 0u, 2u);
    px = frame(dev, 256, 8);
    CHECKEQ(px[2 * 256 + 2], 0xFF00FF00u);
    CHECKEQ(px[2 * 256 + 102], 0xFFFF0000u);
    vcall(dev, DV_SetStreamSource, 4, 0u, 0u, 0u, 0u);
    vcall(vb, 2, 0);
}

static void group_depth_objects_draw(void) {
    uint32_t dev = new_device(32, 32), out = gmem(4);
    CHECKEQ(vcall(dev, DV_CreateDepthStencilSurface, 8, 32u, 32u, FMT_D24S8, 0u, 0u, 0u, out, 0u), D3D_OK);
    uint32_t a = MEM32(out);
    CHECKEQ(vcall(dev, DV_CreateDepthStencilSurface, 8, 32u, 32u, FMT_D24S8, 0u, 0u, 0u, out, 0u), D3D_OK);
    uint32_t b = MEM32(out);
    CHECKEQ(vcall(dev, DV_SetDepthStencilSurface, 1, a), D3D_OK);
    CHECKEQ(vcall(dev, DV_Clear, 6, 0u, 0u, CLEAR_TARGET | CLEAR_ZBUFFER, 0xFF000000u, f32(0.25f), 0u), D3D_OK);
    CHECKEQ(vcall(dev, DV_SetDepthStencilSurface, 1, b), D3D_OK);
    CHECKEQ(vcall(dev, DV_Clear, 6, 0u, 0u, CLEAR_ZBUFFER, 0u, f32(1.0f), 0u), D3D_OK);
    CHECKEQ(vcall(dev, DV_SetDepthStencilSurface, 1, a), D3D_OK);
    quad_tlv(dev, 0, 0, 16, 32, 0.5f, 0xFFFFFFFFu);          /* behind a's 0.25: hidden */
    CHECKEQ(vcall(dev, DV_SetDepthStencilSurface, 1, b), D3D_OK);
    quad_tlv(dev, 16, 0, 32, 32, 0.5f, 0xFFFFFFFFu);         /* in front of b's 1.0: drawn */
    uint32_t *px = frame(dev, 32, 32);
    CHECKEQ(px[10 * 32 + 5], 0xFF000000u);
    CHECKEQ(px[10 * 32 + 20], 0xFFFFFFFFu);
    vcall(dev, DV_SetDepthStencilSurface, 1, 0u);
    vcall(a, 2, 0), vcall(b, 2, 0);
}

static const struct { const char *name; void (*fn)(void); } g_groups[] = {
    { "tri_colour", group_tri_colour }, { "half_pixel", group_half_pixel }, { "transforms", group_transforms },
    { "primitive_types", group_primitive_types }, { "dynamic_discard", group_dynamic_discard },
    { "depth_objects_draw", group_depth_objects_draw },
    { "lock_roundtrip", group_lock_roundtrip }, { "pitches", group_pitches }, { "dxt_small_mips", group_dxt_small_mips },
    { "zeroed", group_zeroed }, { "release_while_bound", group_release_while_bound }, { "child_refs", group_child_refs },
    { "formats_check", group_formats_check },
    { "clear", group_clear }, { "persist", group_persist }, { "present_scaled", group_present_scaled },
    { "reset_rules", group_reset_rules }, { "defaults", group_defaults }, { "fpu", group_fpu },
    { "depth_objects", group_depth_objects },
    { "adapter", group_adapter }, { "ddraw", group_ddraw }, { "objects", group_objects },
    { "unimplemented", group_unimplemented },
};

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: d3d9_selftest <group>\n"); return 2; }
    hle_mem_init();
    bw2_register_shims();
    mach_enter();
    g_esp = hle_va(hle_alloc(1u << 20)) + (1u << 20) - 64;
    for (size_t i = 0; i < sizeof g_groups / sizeof *g_groups; i++)
        if (!strcmp(argv[1], g_groups[i].name)) {
            g_groups[i].fn();
            printf("%s: %d failed\n", argv[1], fails);
            fflush(stdout);
            return fails != 0;
        }
    fprintf(stderr, "d3d9_selftest: no group %s\n", argv[1]);
    return 2;
}
