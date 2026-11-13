/*
 * dinput8.c - DirectInput8Create, IDirectInput8A and the system mouse.
 * Plan 5 Task 1 answered the game's measured setup (0x9AF220: data format,
 * cooperative level, buffer size, Acquire), its mode switch and its two
 * reads; plan 6 Task 2 feeds the mouse from the host input queue (input.c):
 * every MOVE, BUTTON and WHEEL the drain meets reaches bw2_dinput8_sink,
 * which records it in each acquired device's state and buffer. The device
 * is DIMOUSESTATE2's: axes X, Y and the wheel Z, and eight buttons. Every
 * call it does not answer stops loudly; the objects are guest COM objects on
 * plan 4's d3d_com machinery. Follows Microsoft's DirectInput 8 reference.
 */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "bw2.h"
#include "d3d_com.h"
#include "input.h"

#define DI_OK                        0u
#define DI_NOEFFECT                  1u
#define DI_BUFFEROVERFLOW            1u
#define DIERR_INVALIDPARAM           0x80070057u
#define DIERR_NOINTERFACE            0x80004002u
#define DIERR_OLDDIRECTINPUTVERSION  0x8007047Eu
#define DIERR_BETADIRECTINPUTVERSION 0x80070481u
#define CLASS_E_NOAGGREGATION        0x80040110u
#define DIERR_ACQUIRED               0x800700AAu
#define DIERR_NOTACQUIRED            0x8007000Cu
#define DIERR_INPUTLOST              0x8007001Eu
#define DIERR_OTHERAPPHASPRIO        0x80070005u
#define DIERR_NOTBUFFERED            0x80040207u
#define E_NOTIMPL                    0x80004001u
#define E_HANDLE                     0x80070006u

#define DIRECTINPUT_VERSION 0x800u
#define DIPROP_BUFFERSIZE   1u           /* MAKEDIPROP(1): the GUID pointer is the number */
#define DIPROP_AXISMODE     2u
#define DIPROPAXISMODE_ABS  0u
#define DIPROPAXISMODE_REL  1u
#define DIPH_DEVICE         0u
#define DIGDD_PEEK          1u
#define INFINITE            0xFFFFFFFFu

#define DISCL_EXCLUSIVE     1u
#define DISCL_NONEXCLUSIVE  2u
#define DISCL_FOREGROUND    4u
#define DISCL_BACKGROUND    8u

#define DIDF_ABSAXIS        1u
#define DIDF_RELAXIS        2u
#define DIDFT_RELAXIS       1u
#define DIDFT_ABSAXIS       2u
#define DIDFT_PSHBUTTON     4u
#define DIDFT_TGLBUTTON     8u
#define DIDFT_ANYINSTANCE   0xFFFFu      /* the instance field, (type >> 8) & 0xFFFF */
#define DIDFT_OPTIONAL      0x80000000u

#define WS_CHILD            0x40000000u

#define G(a, b, c, d0, d1, d2, d3, d4, d5, d6, d7) \
    { (a) & 0xFF, ((a) >> 8) & 0xFF, ((a) >> 16) & 0xFF, ((a) >> 24) & 0xFF, (b) & 0xFF, ((b) >> 8) & 0xFF, \
      (c) & 0xFF, ((c) >> 8) & 0xFF, d0, d1, d2, d3, d4, d5, d6, d7 }
static const uint8_t IID_IDirectInput8A[16] = G(0xBF798030, 0x483A, 0x4DA2, 0xAA, 0x99, 0x5D, 0x64, 0xED, 0x36, 0x97, 0x00);
static const uint8_t GUID_SysMouse[16] = G(0x6F1D2B60, 0xD5A0, 0x11CF, 0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00);
static const uint8_t GUID_Axis[3][16] = { G(0xA36D02E0, 0xC9F3, 0x11CF, 0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00),
                                          G(0xA36D02E1, 0xC9F3, 0x11CF, 0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00),
                                          G(0xA36D02E2, 0xC9F3, 0x11CF, 0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00) };
static const uint8_t GUID_Button[16] = G(0xA36D02F0, 0xC9F3, 0x11CF, 0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00);

/* The mouse's objects: axes X, Y, Z (instances 0-2), then buttons 0-7 (instances 0-7 of their type). */
enum { N_AXES = 3, N_BUTTONS = 8, N_OBJS = N_AXES + N_BUTTONS };

struct element { uint32_t ofs, data, time, seq; };

struct dinput { struct d3d_obj base; };
struct didevice {
    struct d3d_obj base;
    struct didevice *next;               /* the live devices, for the sink */
    uint32_t di;                         /* the IDirectInput8A it came from (held) */
    uint32_t data_size;                  /* the data format's dwDataSize; 0 before SetDataFormat */
    int32_t ofs[N_OBJS];                 /* each object's offset in the format, or -1 */
    int absolute;                        /* axis mode: the format's flags, then DIPROP_AXISMODE */
    uint32_t coop;                       /* DISCL_* flags; 0 before SetCooperativeLevel */
    uint32_t buffer_size;                /* DIPROP_BUFFERSIZE, in elements */
    int acquired;
    int lost;                            /* lost while acquired; the next read says DIERR_INPUTLOST */
    /* the state since Acquire: relative motion since the last GetDeviceState, the running sums, buttons */
    int32_t rel[N_AXES], sum[N_AXES];
    uint8_t button[N_BUTTONS];
    /* the buffer: a ring of at most buffer_size elements, oldest dropped */
    struct element *ring;
    uint32_t cap, head, n;
    int overflow;
};

static struct didevice *g_devices;
static uint32_t g_sequence;              /* DirectInput's sequence numbers are global */

static void destroy_didevice(struct d3d_obj *o) {
    struct didevice *d = (struct didevice *)o;
    int was_acquired = d->acquired;
    for (struct didevice **p = &g_devices; *p; p = &(*p)->next)
        if (*p == d) { *p = d->next; break; }
    if (d->di && d3d_alive(d->di)) d3d_release(d->di);
    free(d->ring);
    free(d);
    if (was_acquired) bw2_cursor_update();   /* released while acquired: maybe no exclusive mouse left */
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
    for (int i = 0; i < N_OBJS; i++) d->ofs[i] = -1;
    d3d_addref(self);
    MEM32(out) = d3d_new(D3DK_DIDEVICE, "IDirectInputDevice8A", &d->base);
    d->next = g_devices, g_devices = d;
    RET(DI_OK, 4);
}

/* ---- IDirectInputDevice8A ---- */

static struct didevice *dev(void) { return d3d_host(A32(0), D3DK_DIDEVICE); }

static void empty(struct didevice *d) {
    memset(d->rel, 0, sizeof d->rel);
    memset(d->sum, 0, sizeof d->sum);
    memset(d->button, 0, sizeof d->button);
    d->head = d->n = 0, d->overflow = 0;
}

/* The first unused device object an object format names, or -1: its GUID (NULL for any), its type
 * bits and its instance. Axes are relative or absolute by the axis mode, so either type bit
 * matches them; mouse buttons are push buttons. */
static int match(uint32_t guid, uint32_t type, const int *used) {
    uint32_t inst = (type >> 8) & 0xFFFFu;
    for (int i = 0; i < N_OBJS; i++) {
        int axis = i < N_AXES, k = axis ? i : i - N_AXES;
        if (used[i]) continue;
        if (!(type & (axis ? (DIDFT_RELAXIS | DIDFT_ABSAXIS) : DIDFT_PSHBUTTON))) continue;
        if (inst != DIDFT_ANYINSTANCE && inst != (uint32_t)k) continue;
        if (guid && memcmp(hle_ptr(guid), axis ? GUID_Axis[k] : GUID_Button, 16)) continue;
        return i;
    }
    return -1;
}

static void dd_SetDataFormat(void) {              /* (this, LPCDIDATAFORMAT) */
    struct didevice *d = dev();
    uint32_t f = A32(1);
    if (!f) RET(DIERR_INVALIDPARAM, 2);
    if (d->acquired) RET(DIERR_ACQUIRED, 2);
    uint32_t size = MEM32(f), objsize = MEM32(f + 4), flags = MEM32(f + 8), datasize = MEM32(f + 12);
    uint32_t nobjs = MEM32(f + 16), objs = MEM32(f + 20);
    if (size != 24 || objsize != 16 || !datasize || datasize % 4 || (flags & ~(DIDF_ABSAXIS | DIDF_RELAXIS)) ||
        flags == (DIDF_ABSAXIS | DIDF_RELAXIS) || (nobjs && !objs) || nobjs > 0x10000u)
        RET(DIERR_INVALIDPARAM, 2);
    int used[N_OBJS] = { 0 };
    int32_t ofs[N_OBJS];
    for (int i = 0; i < N_OBJS; i++) ofs[i] = -1;
    for (uint32_t i = 0; i < nobjs; i++) {
        uint32_t o = objs + 16 * i, guid = MEM32(o), at = MEM32(o + 4), type = MEM32(o + 8);
        if (type & 0x7F000000u)
            d3d_fail("IDirectInputDevice8A::SetDataFormat object %u type 0x%08X not implemented", i, type);
        int k = match(guid, type, used);
        if (k < 0) {
            if (type & DIDFT_OPTIONAL) continue;
            RET(DIERR_INVALIDPARAM, 2);
        }
        int axis = k < N_AXES;
        /* an axis is a DWORD at a multiple of 4; a button a byte; each inside dwDataSize */
        if ((axis && (at % 4 || at > datasize - 4)) || (!axis && at >= datasize)) RET(DIERR_INVALIDPARAM, 2);
        used[k] = 1, ofs[k] = (int32_t)at;
    }
    memcpy(d->ofs, ofs, sizeof ofs);
    d->data_size = datasize;
    d->absolute = flags == DIDF_ABSAXIS;
    RET(DI_OK, 2);
}

static void dd_SetCooperativeLevel(void) {        /* (this, hwnd, flags) */
    struct didevice *d = dev();
    uint32_t hwnd = A32(1), flags = A32(2);
    if (flags & ~0xFu)
        d3d_fail("IDirectInputDevice8A::SetCooperativeLevel flags 0x%X not implemented", flags);
    uint32_t excl = flags & (DISCL_EXCLUSIVE | DISCL_NONEXCLUSIVE), fg = flags & (DISCL_FOREGROUND | DISCL_BACKGROUND);
    if ((excl != DISCL_EXCLUSIVE && excl != DISCL_NONEXCLUSIVE) || (fg != DISCL_FOREGROUND && fg != DISCL_BACKGROUND))
        RET(DIERR_INVALIDPARAM, 3);
    if (excl == DISCL_EXCLUSIVE && fg == DISCL_BACKGROUND) RET(E_NOTIMPL, 3);      /* no exclusive background mouse */
    if (!hle_is_window(hwnd) || (hle_window_style(hwnd) & WS_CHILD)) RET(E_HANDLE, 3);
    if (d->acquired) RET(DIERR_ACQUIRED, 3);
    d->coop = flags;
    bw2_cursor_update();
    RET(DI_OK, 3);
}

static void dd_SetProperty(void) {                /* (this, rguidProp, LPCDIPROPHEADER) */
    struct didevice *d = dev();
    uint32_t prop = A32(1), h = A32(2);
    if (prop != DIPROP_BUFFERSIZE && prop != DIPROP_AXISMODE)
        d3d_fail("IDirectInputDevice8A::SetProperty property %u not implemented", prop);
    if (!h || MEM32(h) != 20 || MEM32(h + 4) != 16) RET(DIERR_INVALIDPARAM, 3);
    if (MEM32(h + 12) != DIPH_DEVICE || MEM32(h + 8) != 0)
        d3d_fail("IDirectInputDevice8A::SetProperty how %u obj %u not implemented", MEM32(h + 12), MEM32(h + 8));
    uint32_t v = MEM32(h + 16);
    if (prop == DIPROP_AXISMODE && v != DIPROPAXISMODE_ABS && v != DIPROPAXISMODE_REL) RET(DIERR_INVALIDPARAM, 3);
    if (d->acquired) RET(DIERR_ACQUIRED, 3);
    if (prop == DIPROP_AXISMODE) d->absolute = v == DIPROPAXISMODE_ABS;
    else d->buffer_size = v, d->head = d->n = 0, d->overflow = 0;
    RET(DI_OK, 3);
}

static void dd_Acquire(void) {                    /* (this) */
    struct didevice *d = dev();
    if (!d->data_size) RET(DIERR_INVALIDPARAM, 1);      /* no data format yet */
    if ((d->coop & DISCL_FOREGROUND) && !bw2_in_active()) RET(DIERR_OTHERAPPHASPRIO, 1);
    if (d->acquired) RET(DI_NOEFFECT, 1);
    empty(d);
    d->acquired = 1, d->lost = 0;
    bw2_cursor_update();                 /* an exclusive mouse detaches the pointer (cursor.c) */
    RET(DI_OK, 1);
}

static void dd_Unacquire(void) {                  /* (this) */
    struct didevice *d = dev();
    d->lost = 0;
    if (!d->acquired) RET(DI_NOEFFECT, 1);
    d->acquired = 0;
    bw2_cursor_update();
    RET(DI_OK, 1);
}

/* ---- the sink: the drain's mouse events, into every acquired device ---- */

static void put(struct didevice *d, int obj, uint32_t data, uint32_t time, uint32_t seq) {
    if (d->ofs[obj] < 0 || !d->buffer_size) return;
    if (d->n == d->buffer_size) {        /* full: the oldest goes (plan 6 Ruling 14) */
        d->head = (d->head + 1) % d->cap, d->n--;
        d->overflow = 1;
    }
    if (d->n == d->cap) {                /* grow toward buffer_size */
        uint32_t cap = d->cap ? d->cap * 2 : 64;
        if (cap > d->buffer_size) cap = d->buffer_size;
        struct element *r = malloc(cap * sizeof *r);
        if (!r) abort();
        for (uint32_t i = 0; i < d->n; i++) r[i] = d->ring[(d->head + i) % d->cap];
        free(d->ring);
        d->ring = r, d->cap = cap, d->head = 0;
    }
    d->ring[(d->head + d->n) % d->cap] = (struct element){ (uint32_t)d->ofs[obj], data, time, seq };
    d->n++;
}

static void axis(struct didevice *d, int a, int delta, uint32_t time, uint32_t seq) {
    d->rel[a] += delta, d->sum[a] += delta;
    put(d, a, (uint32_t)(d->absolute ? d->sum[a] : delta), time, seq);
}

void bw2_dinput8_sink(const struct bw2_in_event *e) {
    if (e->kind != BW2_IN_MOVE && e->kind != BW2_IN_BUTTON && e->kind != BW2_IN_WHEEL) return;
    if (e->kind == BW2_IN_MOVE && !e->dx && !e->dy) return;
    if (e->kind == BW2_IN_WHEEL && !e->wheel) return;
    if (e->kind == BW2_IN_BUTTON && (e->button < 0 || e->button >= N_BUTTONS)) return;
    uint32_t seq = 0;
    for (struct didevice *d = g_devices; d; d = d->next) {
        if (!d->acquired) continue;
        if (e->kind == BW2_IN_BUTTON) {
            uint8_t v = e->down ? 0x80 : 0;
            if (d->button[e->button] == v) continue;        /* no change */
            d->button[e->button] = v;
            if (!seq) seq = ++g_sequence;
            put(d, N_AXES + e->button, v, e->time_ms, seq);
            continue;
        }
        if (!seq) seq = ++g_sequence;
        if (e->kind == BW2_IN_WHEEL) { axis(d, 2, e->wheel, e->time_ms, seq); continue; }
        if (e->dx) axis(d, 0, e->dx, e->time_ms, seq);
        if (e->dy) axis(d, 1, e->dy, e->time_ms, seq);
    }
}

/* The drain met an inactive activation: the foreground devices lose acquisition at once. */
void bw2_in_mouse_lost(void) {
    for (struct didevice *d = g_devices; d; d = d->next)
        if (d->acquired && (d->coop & DISCL_FOREGROUND)) d->acquired = 0, d->lost = 1;
    bw2_cursor_update();                 /* inactive: shown and reattached at once */
}

/* An exclusive mouse is acquired and the window is active: no Windows mouse messages. */
int bw2_in_mouse_exclusive(void) {
    if (!bw2_in_active()) return 0;
    for (struct didevice *d = g_devices; d; d = d->next)
        if (d->acquired && (d->coop & DISCL_EXCLUSIVE)) return 1;
    return 0;
}

/* The first-read marker (plan 6 Ruling 16): once per process, at the first read. */
static void first_read(void) {
    static int said;
    if (said) return;
    said = 1;
    fprintf(stderr, "[bw2] DirectInput mouse: first read\n");
}

/* The read counter: every GetDeviceData and GetDeviceState call counts,
 * answered or not. BW2_QUIT_AFTER_READS=<n> (plan 8 Task 5): a test ends its
 * run at the nth read, with exit 0, before answering it. The game's front end
 * waits for input and reads the mouse once per frame, so a read count marks
 * the same point in every run. Read once; unset, the reads are only counted.
 * A value that is not a positive integer stops the run at the first read,
 * naming it. Reads run under the machine lock; the counter is atomic anyway. */
static unsigned long long g_reads;
static unsigned long long count_read(void) {
    static int parsed;
    static const char *bad;
    static unsigned long long limit;
    if (!parsed) {
        const char *v = getenv("BW2_QUIT_AFTER_READS");
        if (v) {
            char *end = NULL;
            errno = 0;
            unsigned long long n = (*v >= '0' && *v <= '9') ? strtoull(v, &end, 10) : 0;
            if (!end || *end || errno || n == 0) bad = v;
            else limit = n;
        }
        parsed = 1;
    }
    unsigned long long reads = __atomic_add_fetch(&g_reads, 1, __ATOMIC_SEQ_CST);
    if (bad) d3d_fail("BW2_QUIT_AFTER_READS='%s' is not a positive integer", bad);
    if (limit && reads == limit) {
        fprintf(stderr, "[bw2] quit after %llu input reads (BW2_QUIT_AFTER_READS)\n", limit);
        fflush(stderr);
        hle_exit(0);
    }
    return reads;
}
unsigned long long bw2_dinput_reads(void) { return __atomic_load_n(&g_reads, __ATOMIC_SEQ_CST); }

/* What every read does first: the marker, the counter (and the quit check),
 * the script's events up to this read (script.c), the partial drain; then a
 * loss is reported once, and an unacquired device refuses. */
static uint32_t begin_read(struct didevice *d) {
    first_read();
    bw2_script_at_read(count_read());
    bw2_in_drain_input();
    if (d->lost) { d->lost = 0; return DIERR_INPUTLOST; }
    return d->acquired ? DI_OK : DIERR_NOTACQUIRED;
}

static void dd_GetDeviceState(void) {             /* (this, cbData, lpvData) */
    struct didevice *d = dev();
    uint32_t hr = begin_read(d);
    if (hr != DI_OK) RET(hr, 3);
    uint32_t cb = A32(1), p = A32(2);
    if (!p || cb != d->data_size) RET(DIERR_INVALIDPARAM, 3);
    uint8_t *out = hle_ptr(p);
    memset(out, 0, cb);
    for (int a = 0; a < N_AXES; a++) {
        int32_t v = d->absolute ? d->sum[a] : d->rel[a];
        d->rel[a] = 0;
        if (d->ofs[a] >= 0) MEM32(p + (uint32_t)d->ofs[a]) = (uint32_t)v;
    }
    for (int b = 0; b < N_BUTTONS; b++)
        if (d->ofs[N_AXES + b] >= 0) out[d->ofs[N_AXES + b]] = d->button[b];
    RET(DI_OK, 3);
}

static void dd_GetDeviceData(void) {              /* (this, cbObjectData, rgdod, pdwInOut, dwFlags) */
    struct didevice *d = dev();
    uint32_t hr = begin_read(d);
    if (hr != DI_OK) RET(hr, 5);
    uint32_t cb = A32(1), rgdod = A32(2), inout = A32(3), flags = A32(4);
    if ((cb != 16 && cb != 20) || !inout || (flags & ~DIGDD_PEEK)) RET(DIERR_INVALIDPARAM, 5);
    if (!d->buffer_size) RET(DIERR_NOTBUFFERED, 5);
    /* rgdod NULL: the elements are taken (or, with DIGDD_PEEK, counted) without being stored;
     * NULL with INFINITE is the flush */
    uint32_t want = MEM32(inout), take = d->n < want ? d->n : want;
    for (uint32_t i = 0; rgdod && i < take; i++) {
        const struct element *el = &d->ring[(d->head + i) % d->cap];
        uint32_t o = rgdod + cb * i;
        MEM32(o) = el->ofs, MEM32(o + 4) = el->data, MEM32(o + 8) = el->time, MEM32(o + 12) = el->seq;
        if (cb == 20) MEM32(o + 16) = 0;                 /* uAppData: no action map */
    }
    MEM32(inout) = take;
    hr = d->overflow ? DI_BUFFEROVERFLOW : DI_OK;
    if (!(flags & DIGDD_PEEK)) {
        if (take) d->head = (d->head + take) % d->cap, d->n -= take;
        d->overflow = 0;
    }
    RET(hr, 5);
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

void bw2_dinput8_register(void) {               /* from input.c's bw2_input_register */
    d3d_impls(g_impls);
    win32hle_register(g_exports);
    bw2_in_set_mouse_sink(bw2_dinput8_sink);
}
