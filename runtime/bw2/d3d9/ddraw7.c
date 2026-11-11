/*
 * ddraw7.c - the DirectDraw 7 the game asks for video memory and the driver's
 * identity (plan 4 Task 3, "Measured"): CoCreateInstance(CLSID_DirectDraw) or
 * DirectDrawCreateEx, Initialize, GetAvailableVidMem, GetDeviceIdentifier,
 * GetCaps, Release; and DirectDrawEnumerateA. Nothing draws through it.
 */
#include <stdlib.h>
#include <string.h>
#include "bw2.h"
#include "d3d_com.h"
#include "d3d9_adapter.h"

#define DD_OK               0u
#define DDERR_INVALIDPARAMS 0x80070057u

static const uint8_t CLSID_DirectDraw[16] = { 0xE0, 0x0E, 0xB7, 0xD7, 0x40, 0x43, 0xCF, 0x11,
                                              0xB0, 0x63, 0x00, 0x20, 0xAF, 0xC2, 0xCD, 0x35 };
static const uint8_t IID_IDirectDraw7[16] = { 0xC0, 0x5E, 0xE6, 0x15, 0x9C, 0x3B, 0xD2, 0x11,
                                              0xB9, 0x2F, 0x00, 0x60, 0x97, 0x97, 0xEA, 0x5B };
static const uint8_t IID_IUnknown[16] = { 0, 0, 0, 0, 0, 0, 0, 0, 0xC0, 0, 0, 0, 0, 0, 0, 0x46 };

struct ddraw7 { struct d3d_obj base; };
static void destroy_dd(struct d3d_obj *o) { free(o); }
static uint32_t dd_new(void) {
    struct ddraw7 *d = calloc(1, sizeof *d);
    d->base.destroy = destroy_dd;
    return d3d_new(D3DK_DDRAW7, "IDirectDraw7", &d->base);
}

static uint32_t create_class(const uint8_t *iid, uint32_t *out) {      /* CoCreateInstance */
    if (!iid || (memcmp(iid, IID_IDirectDraw7, 16) && memcmp(iid, IID_IUnknown, 16))) return E_NOINTERFACE_;
    *out = dd_new();
    return 0;
}

static void x_DirectDrawCreateEx(void) {              /* (guid, &dd, iid, outer) */
    const uint8_t *iid = APTR(2);
    if (!A32(1) || !iid || memcmp(iid, IID_IDirectDraw7, 16) || A32(3)) RET(DDERR_INVALIDPARAMS, 4);
    MEM32(A32(1)) = dd_new();
    RET(DD_OK, 4);
}

static void x_DirectDrawEnumerateA(void) {            /* (callback, context) */
    uint32_t cb = A32(0), ctx = A32(1);
    if (!cb) RET(DDERR_INVALIDPARAMS, 2);
    uint32_t mark = hle_scratch_mark();
    uint32_t a[4] = { 0, hle_scratch_copy("Primary Display Driver", 23), hle_scratch_copy("display", 8), ctx };
    hle_call_guest(cb, 4, a);
    hle_scratch_release(mark);
    RET(DD_OK, 2);
}

static void dd_Initialize(void) { (void)d3d_host(A32(0), D3DK_DDRAW7); RET(DD_OK, 2); }
static void dd_GetAvailableVidMem(void) {             /* (this, DDSCAPS2*, &total, &free) */
    (void)d3d_host(A32(0), D3DK_DDRAW7);
    if (A32(2)) MEM32(A32(2)) = BW2_VIDMEM;
    if (A32(3)) MEM32(A32(3)) = BW2_VIDMEM;
    RET(DD_OK, 4);
}
static void dd_GetDeviceIdentifier(void) {            /* (this, DDDEVICEIDENTIFIER2*, flags) */
    uint32_t id = A32(1);
    (void)d3d_host(A32(0), D3DK_DDRAW7);
    if (!id) RET(DDERR_INVALIDPARAMS, 3);
    memset(hle_ptr(id), 0, 1068);
    strcpy(hle_ptr(id), BW2_GPU_DRIVER);
    strcpy(hle_ptr(id + 512), BW2_GPU_DESCRIPTION);
    MEM32(id + 1024) = BW2_GPU_VERSION_LO, MEM32(id + 1028) = BW2_GPU_VERSION_HI;
    MEM32(id + 1032) = BW2_GPU_VENDOR, MEM32(id + 1036) = BW2_GPU_DEVICE;
    memcpy(hle_ptr(id + 1048), bw2_gpu_guid, 16);
    MEM32(id + 1064) = 1;
    RET(DD_OK, 3);
}
static void put_ddcaps(uint32_t c) {
    if (!c) return;
    uint32_t size = MEM32(c);
    if (size < 0x44) return;
    memset(hle_ptr(c + 4), 0, size - 4);
    MEM32(c + 0x3C) = BW2_VIDMEM;                     /* dwVidMemTotal */
    MEM32(c + 0x40) = BW2_VIDMEM;                     /* dwVidMemFree */
}
static void dd_GetCaps(void) {                        /* (this, driver caps, HEL caps) */
    (void)d3d_host(A32(0), D3DK_DDRAW7);
    put_ddcaps(A32(1));
    put_ddcaps(A32(2));
    RET(DD_OK, 3);
}

static const win32hle_shim g_impls[] = {
    { "IDirectDraw7::Initialize", dd_Initialize },
    { "IDirectDraw7::GetAvailableVidMem", dd_GetAvailableVidMem },
    { "IDirectDraw7::GetDeviceIdentifier", dd_GetDeviceIdentifier },
    { "IDirectDraw7::GetCaps", dd_GetCaps },
    { 0, 0 } };
static const win32hle_shim g_exports[] = {
    { "DirectDrawCreateEx", x_DirectDrawCreateEx }, { "DirectDrawEnumerateA", x_DirectDrawEnumerateA }, { 0, 0 } };

void ddraw7_register(void) {
    d3d_impls(g_impls);
    win32hle_register(g_exports);
    hle_com_register_class(CLSID_DirectDraw, create_class);
}
