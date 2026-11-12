/*
 * dsound.c - IDirectSound and its streaming IDirectSoundBuffer (plan 8 Task 3).
 *
 * The game makes its IDirectSound with CoCreateInstance, initializes it on
 * the default device and makes one looping 16-bit stereo buffer, which it
 * streams its own software mix into ("Measured" items 1 and 4). A buffer is a
 * zeroed hle_alloc block that the host mixer (ds_mix.c) plays as one voice.
 * The objects are guest COM objects on plan 4's d3d_com machinery; every
 * method nobody implemented, and every flag, format or level this layer does
 * not handle, stops loudly, naming it.
 *
 * Task 4 adds dsound.dll's DllGetClassObject for the game's audio probe: the
 * private class factory gives an IKsPropertySet whose device enumeration
 * reports one render device with no device path, so the probe keeps the
 * DirectSound output (plan 8 Ruling 3). waveOutOpen stops loudly: the game's
 * other output would otherwise run silently on win32hle's fake device.
 *
 * Threads: these methods run under the machine lock, as every shim does, so
 * the d3d_com state they touch is never shared without it. The mixer's mutex
 * is a leaf, taken only inside the ds_voice_* calls; no method blocks.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "bw2.h"
#include "d3d_com.h"
#include "audio.h"

#define DS_OK                     0u
#define DSERR_INVALIDPARAM        0x80070057u
#define DSERR_NOAGGREGATION       0x80040110u
#define DSERR_ALREADYINITIALIZED  0x88780082u
#define DSERR_UNINITIALIZED       0x887800AAu

#define DSSCL_WRITEPRIMARY        4u
#define DSSPEAKER_STEREO_WIDE     0x00140004u    /* DSSPEAKER_STEREO with DSSPEAKER_GEOMETRY_WIDE (Ruling 4) */

/* The buffer flags this layer handles: they change nothing for a software, looping buffer. */
#define DSBCAPS_LOCSOFTWARE          0x00000008u
#define DSBCAPS_STICKYFOCUS          0x00004000u
#define DSBCAPS_GLOBALFOCUS          0x00008000u
#define DSBCAPS_GETCURRENTPOSITION2  0x00010000u
#define DSB_FLAGS_OK (DSBCAPS_LOCSOFTWARE | DSBCAPS_STICKYFOCUS | DSBCAPS_GLOBALFOCUS | DSBCAPS_GETCURRENTPOSITION2)

#define DSBLOCK_FROMWRITECURSOR   1u
#define DSBLOCK_ENTIREBUFFER      2u
#define DSBPLAY_LOOPING           1u
#define DSBSTATUS_PLAYING         1u
#define DSBSTATUS_LOOPING         4u
#define WAVE_FORMAT_PCM           1u
#define S_OK_                     0u
#define E_POINTER_                0x80004003u
#define CLASS_E_NOAGGREGATION     0x80040110u
#define DSPROPERTY_DIRECTSOUNDDEVICE_ENUMERATE_A 7u
#define DSDEVICE_TYPE_WDM         2u
#define DSDEVICE_DATAFLOW_RENDER  0u
#define DSBSIZE_MIN               4u
#define DSBSIZE_MAX               0x0FFFFFFFu

#define G(a, b, c, d0, d1, d2, d3, d4, d5, d6, d7) \
    { (a) & 0xFF, ((a) >> 8) & 0xFF, ((a) >> 16) & 0xFF, ((a) >> 24) & 0xFF, (b) & 0xFF, ((b) >> 8) & 0xFF, \
      (c) & 0xFF, ((c) >> 8) & 0xFF, d0, d1, d2, d3, d4, d5, d6, d7 }
static const uint8_t CLSID_DirectSound[16] = G(0x47D4D946, 0x62E8, 0x11CF, 0x93, 0xBC, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00);
static const uint8_t CLSID_DirectSound8[16] = G(0x3901CC3F, 0x84B5, 0x4FA4, 0xBA, 0x35, 0xAA, 0x81, 0x72, 0xB8, 0xA0, 0x9B);
static const uint8_t CLSID_DirectSoundCapture[16] = G(0xB0210780, 0x89CD, 0x11D0, 0xAF, 0x08, 0x00, 0xA0, 0xC9, 0x25, 0xCD, 0x16);
static const uint8_t CLSID_DirectSoundCapture8[16] = G(0xE4BCAC13, 0x7F99, 0x4908, 0x9A, 0x8E, 0x74, 0xE3, 0xBF, 0x24, 0xB6, 0xE1);
static const uint8_t CLSID_DirectSoundFullDuplex[16] = G(0xFEA4300C, 0x7959, 0x4147, 0xB2, 0x6A, 0x23, 0x77, 0xB9, 0xE7, 0xA9, 0x1D);
static const uint8_t IID_IUnknown[16] = G(0x00000000, 0x0000, 0x0000, 0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46);
static const uint8_t IID_IDirectSound[16] = G(0x279AFA83, 0x4981, 0x11CE, 0xA5, 0x21, 0x00, 0x20, 0xAF, 0x0B, 0xE5, 0x60);
static const uint8_t DSDEVID_DefaultPlayback[16] = G(0xDEF00000, 0x9C6D, 0x47ED, 0xAA, 0xF1, 0x4D, 0xDA, 0x8F, 0x2B, 0x5C, 0x03);
static const uint8_t CLSID_DirectSoundPrivate[16] = G(0x11AB3EC0, 0x25EC, 0x11D1, 0xA4, 0xD8, 0x00, 0xC0, 0x4F, 0xC2, 0x8A, 0xCA);
static const uint8_t IID_IClassFactory[16] = G(0x00000001, 0x0000, 0x0000, 0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46);
static const uint8_t IID_IKsPropertySet[16] = G(0x31EFAC30, 0x515C, 0x11D0, 0xA9, 0xAA, 0x00, 0xAA, 0x00, 0x61, 0xBE, 0x93);
static const uint8_t DSPROPSETID_DirectSoundDevice[16] = G(0x84624F82, 0x25EC, 0x11D1, 0xA4, 0xD8, 0x00, 0xC0, 0x4F, 0xC2, 0x8A, 0xCA);
/* The Mac's default output as the enumeration names it: a fixed GUID of this port's choosing. */
static const uint8_t BW2_DeviceId[16] = G(0xB5D2D6A1, 0x6E0B, 0x4C1F, 0x9A, 0x3E, 0x0D, 0xA5, 0xB0, 0xC0, 0xA8, 0x01);
#undef G

struct dsound {
    struct d3d_obj base;
    int initialized;
};
struct dsbuffer {
    struct d3d_obj base;
    uint32_t ds;                         /* the IDirectSound it came from (held) */
    uint32_t data, bytes;                /* the hle_alloc'd ring, in guest memory */
    int voice;                           /* the mixer's voice; 0 once freed */
};

static void destroy_dsound(struct d3d_obj *o) { free(o); }
/* The voice leaves the mixer first (ds_voice_free returns only once no ds_mix
 * reads it), then the block goes, then the reference on the IDirectSound. */
static void destroy_dsbuffer(struct d3d_obj *o) {
    struct dsbuffer *b = (struct dsbuffer *)o;
    if (b->voice) ds_voice_free(b->voice);
    b->voice = 0;
    hle_free(hle_ptr(b->data));
    if (b->ds && d3d_alive(b->ds)) d3d_release(b->ds);
    free(b);
}

/* ---- classes ---- */

static uint32_t create_dsound(const uint8_t *iid, uint32_t *out) {      /* CoCreateInstance(CLSID_DirectSound) */
    if (!iid || (memcmp(iid, IID_IDirectSound, 16) && memcmp(iid, IID_IUnknown, 16))) {
        char s[40] = "(null)";
        if (iid) hle_guid_string(iid, s);
        d3d_fail("dsound: CoCreateInstance of CLSID_DirectSound with IID %s not implemented", s);
    }
    struct dsound *d = calloc(1, sizeof *d);
    if (!d) abort();
    d->base.destroy = destroy_dsound;
    uint32_t obj = d3d_new(D3DK_DSOUND, "IDirectSound", &d->base);
    if (out) *out = obj;
    return DS_OK;
}
static uint32_t create_dsound8(const uint8_t *iid, uint32_t *out) {
    (void)iid, (void)out;
    char s[40];
    hle_guid_string(CLSID_DirectSound8, s);
    d3d_fail("dsound: CoCreateInstance of CLSID_DirectSound8 %s: IDirectSound8 not implemented", s);
}
static uint32_t create_capture(const uint8_t *iid, uint32_t *out) {
    (void)iid, (void)out;
    d3d_fail("dsound: DirectSoundCapture not implemented");
}
static uint32_t create_full_duplex(const uint8_t *iid, uint32_t *out) {
    (void)iid, (void)out;
    d3d_fail("dsound: DirectSoundFullDuplex not implemented");
}

/* dsound.dll's exports: no plan owns them, and the game uses none. */
static void x_DirectSoundCreate(void) { d3d_fail("dsound: DirectSoundCreate not implemented"); }
static void x_DirectSoundCreate8(void) { d3d_fail("dsound: DirectSoundCreate8 not implemented"); }
static void x_DirectSoundEnumerateA(void) { d3d_fail("dsound: DirectSoundEnumerateA not implemented"); }

/* ---- IDirectSound ---- */

static struct dsound *ds_of(uint32_t obj) { return d3d_host(obj, D3DK_DSOUND); }

static void ds_Initialize(void) {                  /* (this, LPCGUID pcGuidDevice) */
    struct dsound *d = ds_of(A32(0));
    uint32_t g = A32(1);
    if (d->initialized) RET(DSERR_ALREADYINITIALIZED, 2);
    if (g && memcmp(hle_ptr(g), DSDEVID_DefaultPlayback, 16)) {
        char s[40];
        hle_guid_string(hle_ptr(g), s);
        d3d_fail("IDirectSound::Initialize of device %s not implemented", s);
    }
    d->initialized = 1;
    RET(DS_OK, 2);
}

static void ds_SetCooperativeLevel(void) {         /* (this, hwnd, dwLevel) */
    struct dsound *d = ds_of(A32(0));
    uint32_t level = A32(2);
    if (!d->initialized) RET(DSERR_UNINITIALIZED, 3);
    if (level == DSSCL_WRITEPRIMARY) d3d_fail("IDirectSound::SetCooperativeLevel DSSCL_WRITEPRIMARY not implemented");
    if (level < 1 || level > 3) RET(DSERR_INVALIDPARAM, 3);
    RET(DS_OK, 3);                                  /* normal, priority, exclusive: one software device */
}

static void ds_GetSpeakerConfig(void) {            /* (this, LPDWORD) */
    struct dsound *d = ds_of(A32(0));
    uint32_t c = A32(1);
    if (!d->initialized) RET(DSERR_UNINITIALIZED, 2);
    if (!c) RET(DSERR_INVALIDPARAM, 2);
    MEM32(c) = DSSPEAKER_STEREO_WIDE;
    RET(DS_OK, 2);
}

static void ds_CreateSoundBuffer(void) {           /* (this, LPCDSBUFFERDESC, LPDIRECTSOUNDBUFFER *, pUnkOuter) */
    uint32_t self = A32(0), desc = A32(1), out = A32(2), outer = A32(3);
    struct dsound *d = ds_of(self);
    if (out) MEM32(out) = 0;
    if (!d->initialized) RET(DSERR_UNINITIALIZED, 4);
    if (!desc || !out) RET(DSERR_INVALIDPARAM, 4);
    if (outer) RET(DSERR_NOAGGREGATION, 4);
    uint32_t size = MEM32(desc), flags = MEM32(desc + 4), bytes = MEM32(desc + 8), wfx = MEM32(desc + 16);
    if (size != 20 && size != 36) RET(DSERR_INVALIDPARAM, 4);     /* DirectX 3 or DirectX 7 description */
    if (flags & ~DSB_FLAGS_OK)
        d3d_fail("IDirectSound::CreateSoundBuffer flags 0x%X: bits 0x%X not implemented", flags, flags & ~DSB_FLAGS_OK);
    if (!wfx) RET(DSERR_INVALIDPARAM, 4);                         /* only a primary buffer has no format */
    const uint8_t *w = hle_ptr(wfx);
    uint16_t tag, ch, block, bits;
    uint32_t rate;
    memcpy(&tag, w, 2), memcpy(&ch, w + 2, 2), memcpy(&rate, w + 4, 4), memcpy(&block, w + 12, 2);
    memcpy(&bits, w + 14, 2);
    if (tag != WAVE_FORMAT_PCM || (ch != 1 && ch != 2) || (bits != 8 && bits != 16) || rate < 100 || rate > 200000
        || block != ch * bits / 8)
        d3d_fail("IDirectSound::CreateSoundBuffer format tag 0x%X, %u channels, %u bits (%u Hz, block %u) not implemented",
                 tag, ch, bits, rate, block);
    if (bytes < DSBSIZE_MIN || bytes > DSBSIZE_MAX || bytes % block) RET(DSERR_INVALIDPARAM, 4);

    struct dsbuffer *b = calloc(1, sizeof *b);
    if (!b) abort();
    b->base.destroy = destroy_dsbuffer;
    b->data = hle_va(hle_alloc(bytes));                           /* zeroed */
    b->bytes = bytes;
    b->voice = ds_voice_new(b->data, bytes, (struct ds_format){ rate, ch, bits });
    b->ds = self;
    d3d_addref(self);
    MEM32(out) = d3d_new(D3DK_DSBUFFER, "IDirectSoundBuffer", &b->base);
    ds_out_start();                                               /* once: later calls do nothing */
    static uint32_t made;
    made++;
    if (ds_audio_log()) {
        fprintf(stderr, "[bw2] audio buffer %u: %u ch, %u Hz, %u bit, %u bytes, flags 0x%X\n", made, ch, rate, bits,
                bytes, flags);
        fflush(stderr);
    }
    RET(DS_OK, 4);
}

/* ---- IDirectSoundBuffer ---- */

static struct dsbuffer *buf_of(uint32_t obj) { return d3d_host(obj, D3DK_DSBUFFER); }

static void dsb_GetCurrentPosition(void) {         /* (this, LPDWORD play, LPDWORD write) */
    struct dsbuffer *b = buf_of(A32(0));
    uint32_t pp = A32(1), wp = A32(2), p, w;
    ds_voice_cursors(b->voice, &p, &w);
    if (pp) MEM32(pp) = p;
    if (wp) MEM32(wp) = w;
    RET(DS_OK, 3);
}

static void dsb_GetStatus(void) {                  /* (this, LPDWORD) */
    struct dsbuffer *b = buf_of(A32(0));
    uint32_t s = A32(1);
    if (!s) RET(DSERR_INVALIDPARAM, 2);
    MEM32(s) = ds_voice_playing(b->voice) ? DSBSTATUS_PLAYING | DSBSTATUS_LOOPING : 0;  /* every play loops */
    RET(DS_OK, 2);
}

/* (this, dwOffset, dwBytes, LPVOID *p1, LPDWORD n1, LPVOID *p2, LPDWORD n2, dwFlags) */
static void dsb_Lock(void) {
    struct dsbuffer *b = buf_of(A32(0));
    uint32_t off = A32(1), bytes = A32(2), p1 = A32(3), n1 = A32(4), p2 = A32(5), n2 = A32(6), flags = A32(7);
    if (flags & ~(DSBLOCK_FROMWRITECURSOR | DSBLOCK_ENTIREBUFFER))
        d3d_fail("IDirectSoundBuffer::Lock flags 0x%X not implemented", flags);
    if (flags & DSBLOCK_FROMWRITECURSOR) ds_voice_cursors(b->voice, NULL, &off);
    if (flags & DSBLOCK_ENTIREBUFFER) bytes = b->bytes;
    if (!p1 || !n1 || off >= b->bytes || bytes > b->bytes || !bytes) RET(DSERR_INVALIDPARAM, 8);
    uint32_t first = bytes < b->bytes - off ? bytes : b->bytes - off, rest = bytes - first;
    MEM32(p1) = b->data + off;
    MEM32(n1) = first;
    if (p2) MEM32(p2) = rest ? b->data : 0;
    if (n2) MEM32(n2) = p2 ? rest : 0;              /* no second pointer: only the first part */
    RET(DS_OK, 8);
}

static void dsb_Unlock(void) {                     /* (this, p1, n1, p2, n2) */
    struct dsbuffer *b = buf_of(A32(0));
    for (int i = 0; i < 2; i++) {
        uint32_t p = A32(1 + 2 * i), n = A32(2 + 2 * i);
        if (p && (p < b->data || p >= b->data + b->bytes || n > b->data + b->bytes - p)) RET(DSERR_INVALIDPARAM, 5);
    }
    RET(DS_OK, 5);                                  /* the mixer reads the ring in place: nothing to copy */
}

static void dsb_Play(void) {                       /* (this, dwReserved1, dwPriority, dwFlags) */
    struct dsbuffer *b = buf_of(A32(0));
    uint32_t r1 = A32(1), prio = A32(2), flags = A32(3);
    if (flags != DSBPLAY_LOOPING || r1 || prio)
        d3d_fail("IDirectSoundBuffer::Play flags 0x%X (reserved %u, priority %u) not implemented", flags, r1, prio);
    ds_voice_play(b->voice);
    RET(DS_OK, 4);
}

static void dsb_Stop(void) {                       /* (this) */
    ds_voice_stop(buf_of(A32(0))->voice);
    RET(DS_OK, 1);
}

/* ---- dsound.dll's private device enumeration ("Measured" item 3) ---- */

static void x_DllGetClassObject(void) {            /* (rclsid, riid, ppv) */
    const uint8_t *clsid = APTR(0), *iid = APTR(1);
    uint32_t out = A32(2);
    char c[40] = "(null)", i[40] = "(null)";
    if (clsid) hle_guid_string(clsid, c);
    if (iid) hle_guid_string(iid, i);
    if (!clsid || memcmp(clsid, CLSID_DirectSoundPrivate, 16))
        d3d_fail("dsound: DllGetClassObject of class %s not answered", c);
    if (!iid || memcmp(iid, IID_IClassFactory, 16))
        d3d_fail("dsound: DllGetClassObject of class %s with IID %s not answered", c, i);
    if (!out) RET(E_POINTER_, 3);
    struct d3d_obj *f = calloc(1, sizeof *f);
    if (!f) abort();
    MEM32(out) = d3d_new(D3DK_DSPRIVATE, "IClassFactory", f);
    RET(S_OK_, 3);
}

static void cf_CreateInstance(void) {              /* (this, pUnkOuter, riid, ppv) */
    (void)d3d_host(A32(0), D3DK_DSPRIVATE);
    uint32_t outer = A32(1), out = A32(3);
    const uint8_t *iid = APTR(2);
    if (!out) RET(E_POINTER_, 4);
    MEM32(out) = 0;
    if (outer) RET(CLASS_E_NOAGGREGATION, 4);
    if (!iid || memcmp(iid, IID_IKsPropertySet, 16)) RET(E_NOINTERFACE_, 4);
    struct d3d_obj *k = calloc(1, sizeof *k);
    if (!k) abort();
    MEM32(out) = d3d_new(D3DK_KSPROPSET, "IKsPropertySet", k);
    RET(S_OK_, 4);
}

static void cf_LockServer(void) {                  /* (this, fLock) */
    (void)d3d_host(A32(0), D3DK_DSPRIVATE);
    RET(S_OK_, 2);
}

/* The one render device, as DSPROPERTY_DIRECTSOUNDDEVICE_DESCRIPTION_A (40 bytes):
 * Type, DataFlow, DeviceId, then the Description, Module and Interface string
 * pointers and WaveDeviceId. The game copies the Interface string without
 * checking the pointer, so all three point to strings that live for the run;
 * Interface is "" (no \\?\ device path, Ruling 3). */
static uint32_t device_description(void) {
    static uint32_t desc, text[3];
    static const char *const s[3] = { "Default output", "coreaudio", "" };
    if (!desc) {
        for (int i = 0; i < 3; i++) {
            text[i] = hle_va(hle_alloc((uint32_t)strlen(s[i]) + 1));
            memcpy(hle_ptr(text[i]), s[i], strlen(s[i]) + 1);
        }
        desc = hle_va(hle_alloc(40));
    }
    MEM32(desc) = DSDEVICE_TYPE_WDM;                /* written each time: the callback may have changed it */
    MEM32(desc + 4) = DSDEVICE_DATAFLOW_RENDER;
    memcpy(hle_ptr(desc + 8), BW2_DeviceId, 16);
    for (int i = 0; i < 3; i++) {
        memcpy(hle_ptr(text[i]), s[i], strlen(s[i]) + 1);
        MEM32(desc + 0x18 + 4u * (uint32_t)i) = text[i];
    }
    MEM32(desc + 0x24) = 0;
    return desc;
}

/* (this, rguidPropSet, ulId, pInstanceData, ulInstanceLength, pPropertyData, ulDataLength, pulBytesReturned) */
static void ks_Get(void) {
    (void)d3d_host(A32(0), D3DK_KSPROPSET);
    const uint8_t *set = APTR(1);
    uint32_t id = A32(2), data = A32(5), cb = A32(6), ret = A32(7);
    char g[40] = "(null)";
    if (set) hle_guid_string(set, g);
    if (!set || memcmp(set, DSPROPSETID_DirectSoundDevice, 16) || id != DSPROPERTY_DIRECTSOUNDDEVICE_ENUMERATE_A)
        d3d_fail("IKsPropertySet::Get of set %s, id %u not implemented", g, id);
    if (!data || cb < 8)
        d3d_fail("IKsPropertySet::Get of set %s, id %u with data 0x%08X of %u bytes not implemented", g, id, data, cb);
    uint32_t callback = MEM32(data), context = MEM32(data + 4);
    if (!callback) d3d_fail("IKsPropertySet::Get of set %s, id %u with no callback not implemented", g, id);
    uint32_t a[2] = { device_description(), context };
    hle_call_guest(callback, 2, a);                 /* one device: its answer (continue or not) changes nothing */
    if (ret) MEM32(ret) = 8;
    RET(S_OK_, 8);
}

/* ---- the waveOut guard ---- */

static void x_waveOutOpen(void) { d3d_fail("waveOutOpen: the waveOut output is not answered"); }

static const win32hle_shim g_impls[] = {
    { "IClassFactory::CreateInstance", cf_CreateInstance },
    { "IClassFactory::LockServer", cf_LockServer },
    { "IKsPropertySet::Get", ks_Get },
    { "IDirectSound::CreateSoundBuffer", ds_CreateSoundBuffer },
    { "IDirectSound::SetCooperativeLevel", ds_SetCooperativeLevel },
    { "IDirectSound::GetSpeakerConfig", ds_GetSpeakerConfig },
    { "IDirectSound::Initialize", ds_Initialize },
    { "IDirectSoundBuffer::GetCurrentPosition", dsb_GetCurrentPosition },
    { "IDirectSoundBuffer::GetStatus", dsb_GetStatus },
    { "IDirectSoundBuffer::Lock", dsb_Lock },
    { "IDirectSoundBuffer::Play", dsb_Play },
    { "IDirectSoundBuffer::Stop", dsb_Stop },
    { "IDirectSoundBuffer::Unlock", dsb_Unlock },
    { 0, 0 } };
static const win32hle_shim g_exports[] = {
    { "DirectSoundCreate", x_DirectSoundCreate },
    { "DirectSoundCreate8", x_DirectSoundCreate8 },
    { "DirectSoundEnumerateA", x_DirectSoundEnumerateA },
    { "DllGetClassObject", x_DllGetClassObject },   /* answered for any system module; only the private class */
    { "waveOutOpen", x_waveOutOpen },               /* before win32hle's silent one */
    { 0, 0 } };

void bw2_audio_register(void) {
    d3d_impls(g_impls);
    win32hle_register(g_exports);
    hle_com_register_class(CLSID_DirectSound, create_dsound);
    hle_com_register_class(CLSID_DirectSound8, create_dsound8);
    hle_com_register_class(CLSID_DirectSoundCapture, create_capture);
    hle_com_register_class(CLSID_DirectSoundCapture8, create_capture);
    hle_com_register_class(CLSID_DirectSoundFullDuplex, create_full_duplex);
}
