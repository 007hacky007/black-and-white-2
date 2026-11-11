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

static const struct { const char *name; void (*fn)(void); } g_groups[] = {
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
