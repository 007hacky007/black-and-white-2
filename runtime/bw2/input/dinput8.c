/*
 * dinput8.c - DirectInput8Create, IDirectInput8A and a system mouse
 * (plan 5 Task 1). It answers the game's measured setup (0x9AF220: data
 * format, cooperative level, buffer size, Acquire), its mode switch and its
 * two reads, and reports no input: plan 6 replaces the reads and keeps the
 * rest. Every call it does not answer stops loudly; the objects are guest COM
 * objects on plan 4's d3d_com machinery.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "bw2.h"
#include "d3d_com.h"

#define DI_OK                        0u
#define DI_NOEFFECT                  1u
#define DIERR_INVALIDPARAM           0x80070057u
#define DIERR_NOINTERFACE            0x80004002u
#define DIERR_OLDDIRECTINPUTVERSION  0x8007047Eu
#define DIERR_BETADIRECTINPUTVERSION 0x80070481u
#define CLASS_E_NOAGGREGATION        0x80040110u
#define DIERR_ACQUIRED               0x800700AAu
#define DIERR_NOTACQUIRED            0x8007000Cu

#define DIRECTINPUT_VERSION 0x800u
#define DIPROP_BUFFERSIZE   1u           /* MAKEDIPROP(1): the GUID pointer is the number */
#define DIPH_DEVICE         0u
#define DIGDD_PEEK          1u

#define G(a, b, c, d0, d1, d2, d3, d4, d5, d6, d7) \
    { (a) & 0xFF, ((a) >> 8) & 0xFF, ((a) >> 16) & 0xFF, ((a) >> 24) & 0xFF, (b) & 0xFF, ((b) >> 8) & 0xFF, \
      (c) & 0xFF, ((c) >> 8) & 0xFF, d0, d1, d2, d3, d4, d5, d6, d7 }
static const uint8_t IID_IDirectInput8A[16] = G(0xBF798030, 0x483A, 0x4DA2, 0xAA, 0x99, 0x5D, 0x64, 0xED, 0x36, 0x97, 0x00);
static const uint8_t GUID_SysMouse[16] = G(0x6F1D2B60, 0xD5A0, 0x11CF, 0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00);

struct dinput { struct d3d_obj base; };
struct didevice {
    struct d3d_obj base;
    uint32_t di;                         /* the IDirectInput8A it came from (held) */
    uint32_t data_size;                  /* the data format's dwDataSize; 0 before SetDataFormat */
    uint32_t coop;                       /* DISCL_* flags; 0 before SetCooperativeLevel */
    uint32_t buffer_size;                /* DIPROP_BUFFERSIZE */
    int acquired;
};

static void destroy_didevice(struct d3d_obj *o) {
    struct didevice *d = (struct didevice *)o;
    if (d->di && d3d_alive(d->di)) d3d_release(d->di);
    free(d);
}
static void destroy_dinput(struct d3d_obj *o) { free(o); }

static void x_DirectInput8Create(void) {          /* (hinst, version, riid, ppvOut, punkOuter) */
    uint32_t version = A32(1), out = A32(3), outer = A32(4);
    const uint8_t *riid = APTR(2);
    if (out) MEM32(out) = 0;
    if (!out || !riid) RET(DIERR_INVALIDPARAM, 5);
    if (outer) RET(CLASS_E_NOAGGREGATION, 5);
    if (version < DIRECTINPUT_VERSION) RET(DIERR_OLDDIRECTINPUTVERSION, 5);
    if (version > DIRECTINPUT_VERSION) RET(DIERR_BETADIRECTINPUTVERSION, 5);
    if (memcmp(riid, IID_IDirectInput8A, 16)) RET(DIERR_NOINTERFACE, 5);
    struct dinput *di = calloc(1, sizeof *di);
    if (!di) abort();
    di->base.destroy = destroy_dinput;
    MEM32(out) = d3d_new(D3DK_DINPUT8, "IDirectInput8A", &di->base);
    RET(DI_OK, 5);
}

/* ---- IDirectInput8A ---- */

static void di_CreateDevice(void) {               /* (this, rguid, lplpDirectInputDevice, pUnkOuter) */
    uint32_t self = A32(0), out = A32(2), outer = A32(3);
    const uint8_t *guid = APTR(1);
    d3d_host(self, D3DK_DINPUT8);
    if (out) MEM32(out) = 0;
    if (!out || !guid) RET(DIERR_INVALIDPARAM, 4);
    if (outer) RET(CLASS_E_NOAGGREGATION, 4);
    if (memcmp(guid, GUID_SysMouse, 16)) {
        char s[40];
        hle_guid_string(guid, s);
        d3d_fail("IDirectInput8A::CreateDevice of device %s not implemented", s);
    }
    struct didevice *d = calloc(1, sizeof *d);
    if (!d) abort();
    d->base.destroy = destroy_didevice;
    d->di = self;
    d3d_addref(self);
    MEM32(out) = d3d_new(D3DK_DIDEVICE, "IDirectInputDevice8A", &d->base);
    RET(DI_OK, 4);
}

/* ---- IDirectInputDevice8A ---- */

static struct didevice *dev(void) { return d3d_host(A32(0), D3DK_DIDEVICE); }

static void dd_SetDataFormat(void) {              /* (this, LPCDIDATAFORMAT) */
    struct didevice *d = dev();
    uint32_t f = A32(1);
    if (!f) RET(DIERR_INVALIDPARAM, 2);
    if (d->acquired) RET(DIERR_ACQUIRED, 2);
    uint32_t size = MEM32(f), objsize = MEM32(f + 4), datasize = MEM32(f + 12);
    if (size != 24 || objsize != 16 || (datasize != 16 && datasize != 20)) RET(DIERR_INVALIDPARAM, 2);
    d->data_size = datasize;
    RET(DI_OK, 2);
}

static void dd_SetCooperativeLevel(void) {        /* (this, hwnd, flags) */
    struct didevice *d = dev();
    uint32_t flags = A32(2);
    if (flags != 5 && flags != 6 && flags != 9 && flags != 10)
        d3d_fail("IDirectInputDevice8A::SetCooperativeLevel flags 0x%X not implemented", flags);
    if (d->acquired) RET(DIERR_ACQUIRED, 3);
    d->coop = flags;
    RET(DI_OK, 3);
}

static void dd_SetProperty(void) {                /* (this, rguidProp, LPCDIPROPHEADER) */
    struct didevice *d = dev();
    uint32_t prop = A32(1), h = A32(2);
    if (prop != DIPROP_BUFFERSIZE)
        d3d_fail("IDirectInputDevice8A::SetProperty property %u not implemented", prop);
    if (!h || MEM32(h) != 20 || MEM32(h + 4) != 16) RET(DIERR_INVALIDPARAM, 3);
    if (MEM32(h + 12) != DIPH_DEVICE || MEM32(h + 8) != 0)
        d3d_fail("IDirectInputDevice8A::SetProperty how %u obj %u not implemented", MEM32(h + 12), MEM32(h + 8));
    if (d->acquired) RET(DIERR_ACQUIRED, 3);
    d->buffer_size = MEM32(h + 16);
    RET(DI_OK, 3);
}

static void dd_Acquire(void) {                    /* (this) */
    struct didevice *d = dev();
    if (d->acquired) RET(DI_NOEFFECT, 1);
    if (!d->data_size) RET(DIERR_INVALIDPARAM, 1);      /* no data format yet */
    d->acquired = 1;
    RET(DI_OK, 1);
}

static void dd_Unacquire(void) {                  /* (this) */
    struct didevice *d = dev();
    if (!d->acquired) RET(DI_NOEFFECT, 1);
    d->acquired = 0;
    RET(DI_OK, 1);
}

static void no_input_yet(void) {
    static int said;
    if (said) return;
    said = 1;
    fprintf(stderr, "[bw2] DirectInput mouse: no input until plan 6\n");
}

static void dd_GetDeviceState(void) {             /* (this, cbData, lpvData) */
    struct didevice *d = dev();
    uint32_t cb = A32(1), p = A32(2);
    if (!d->acquired) RET(DIERR_NOTACQUIRED, 3);
    if (!p || cb != d->data_size) RET(DIERR_INVALIDPARAM, 3);
    no_input_yet();
    memset(hle_ptr(p), 0, cb);
    RET(DI_OK, 3);
}

static void dd_GetDeviceData(void) {              /* (this, cbObjectData, rgdod, pdwInOut, dwFlags) */
    struct didevice *d = dev();
    uint32_t cb = A32(1), inout = A32(3), flags = A32(4);
    if (!d->acquired) RET(DIERR_NOTACQUIRED, 5);
    if ((cb != 16 && cb != 20) || !inout || (flags & ~DIGDD_PEEK)) RET(DIERR_INVALIDPARAM, 5);
    no_input_yet();
    MEM32(inout) = 0;
    RET(DI_OK, 5);
}

static const win32hle_shim g_impls[] = {
    { "IDirectInput8A::CreateDevice", di_CreateDevice },
    { "IDirectInputDevice8A::SetProperty", dd_SetProperty },
    { "IDirectInputDevice8A::Acquire", dd_Acquire },
    { "IDirectInputDevice8A::Unacquire", dd_Unacquire },
    { "IDirectInputDevice8A::GetDeviceState", dd_GetDeviceState },
    { "IDirectInputDevice8A::GetDeviceData", dd_GetDeviceData },
    { "IDirectInputDevice8A::SetDataFormat", dd_SetDataFormat },
    { "IDirectInputDevice8A::SetCooperativeLevel", dd_SetCooperativeLevel },
    { 0, 0 } };
static const win32hle_shim g_exports[] = { { "DirectInput8Create", x_DirectInput8Create }, { 0, 0 } };

void bw2_input_register(void) {
    d3d_impls(g_impls);
    win32hle_register(g_exports);
}
