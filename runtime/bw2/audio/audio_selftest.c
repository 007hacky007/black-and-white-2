/*
 * audio_selftest.c - the host mixer and its output (plan 8 Task 2), driven
 * directly, and DirectSound (Task 3), driven as lifted code drives it:
 * arguments pushed right to left, a return address, then the shim; COM
 * methods through the guest vtable, each check also verifying the stdcall pop.
 * Sample data lives in hle_alloc blocks, as the game's buffers will.
 * Built with the engine (minus main.c) by tests/test_audio.py;
 * `audio_selftest <group> [arg]` runs one group and prints "<group>: <n> failed".
 */
#include <math.h>
#include <stdarg.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include "win32hle.h"
#include "bw2.h"
#include "audio.h"
#include "d3d_com.h"

const recomp_dispatch_entry_t recomp_dispatch_table[1] = { { 0, 0 } };
const uint32_t recomp_dispatch_count = 0;

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)
#define CHECKEQ(a, b) do { uint64_t _a = (uint64_t)(a), _b = (uint64_t)(b); if (_a != _b) { \
    printf("FAIL %s:%d %s == %s: 0x%llX != 0x%llX\n", __FILE__, __LINE__, #a, #b, \
           (unsigned long long)_a, (unsigned long long)_b); fails++; } } while (0)
#define CHECKF(a, b) do { float _a = (a), _b = (b); if (_a != _b) { \
    printf("FAIL %s:%d %s == %s: %.9g != %.9g\n", __FILE__, __LINE__, #a, #b, _a, _b); fails++; } } while (0)

static double now_s(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec + (double)t.tv_nsec * 1e-9;
}
static void sleep_ms(uint32_t ms) { usleep(ms * 1000u); }

static uint32_t gblock(uint32_t bytes) { return hle_va(hle_alloc(bytes)); }
static int16_t *s16(uint32_t va) { return (int16_t *)hle_ptr(va); }
static uint32_t play_of(int id) { uint32_t p = 0xFFFFFFFFu; ds_voice_cursors(id, &p, NULL); return p; }
static uint32_t write_of(int id) { uint32_t w = 0xFFFFFFFFu; ds_voice_cursors(id, NULL, &w); return w; }

static const struct ds_format ST16 = { 44100, 2, 16 }, MONO16 = { 44100, 1, 16 };

static float g_out[2 * 8192];

/* A voice that keeps playing for 10 s without wrapping, so its play cursor is
 * its consumed count. */
static int long_voice(void) {
    uint32_t bytes = 10u * 44100u * 2u;
    int id = ds_voice_new(gblock(bytes), bytes, MONO16);
    ds_voice_play(id);
    return id;
}

static void group_ds_mix_exact(void) {
    uint32_t data = gblock(1000 * 4);
    int16_t *s = s16(data);
    for (int i = 0; i < 1000; i++) { s[2 * i] = (int16_t)(i * 16 - 16000); s[2 * i + 1] = (int16_t)(i * 16 + 16); }
    int id = ds_voice_new(data, 1000 * 4, ST16);
    CHECK(id > 0);
    CHECK(!ds_voice_playing(id));
    ds_mix(g_out, 16);                                  /* stopped: silence, no movement */
    for (int i = 0; i < 32; i++) CHECKF(g_out[i], 0.0f);
    CHECKEQ(play_of(id), 0);
    CHECKEQ(write_of(id), 512 * 4);
    uint64_t before = ds_frames_mixed();
    ds_voice_play(id);
    CHECK(ds_voice_playing(id));
    ds_mix(g_out, 1100);
    CHECKEQ(ds_frames_mixed() - before, 1100);
    int bad = 0;
    for (int j = 0; j < 1100; j++) {
        int k = j % 1000;
        if (g_out[2 * j] != (float)s[2 * k] / 32768.0f || g_out[2 * j + 1] != (float)s[2 * k + 1] / 32768.0f) {
            if (bad++ < 4) printf("FAIL frame %d: %.9g %.9g\n", j, g_out[2 * j], g_out[2 * j + 1]);
        }
    }
    CHECKEQ(bad, 0);
    CHECKEQ(play_of(id), 100 * 4);
    CHECKEQ(write_of(id), ((100 + 512) % 1000) * 4);
    ds_voice_free(id);
    hle_free(hle_ptr(data));
}

static void group_ds_mix_formats(void) {
    uint32_t d8 = gblock(2);
    uint8_t *b = (uint8_t *)hle_ptr(d8);
    b[0] = 128; b[1] = 192;
    int id = ds_voice_new(d8, 2, (struct ds_format){ 22050, 1, 8 });
    ds_voice_play(id);
    ds_mix(g_out, 5);
    static const float want[5] = { 0.0f, 0.25f, 0.5f, 0.25f, 0.0f };
    for (int i = 0; i < 5; i++) { CHECKF(g_out[2 * i], want[i]); CHECKF(g_out[2 * i + 1], want[i]); }
    ds_voice_free(id);
    hle_free(b);

    uint32_t d16 = gblock(3 * 2);
    int16_t *s = s16(d16);
    s[0] = 1000; s[1] = -2000; s[2] = 3000;
    id = ds_voice_new(d16, 6, MONO16);
    ds_voice_play(id);
    ds_mix(g_out, 4);
    for (int i = 0; i < 4; i++) {
        CHECKF(g_out[2 * i], (float)s[i % 3] / 32768.0f);
        CHECKF(g_out[2 * i + 1], (float)s[i % 3] / 32768.0f);
    }
    CHECKEQ(play_of(id), 1 * 2);
    ds_voice_free(id);
    hle_free(s);
}

static int const_voice(int16_t v, uint32_t frames) {
    uint32_t data = gblock(frames * 4);
    int16_t *s = s16(data);
    for (uint32_t i = 0; i < 2 * frames; i++) s[i] = v;
    int id = ds_voice_new(data, frames * 4, ST16);
    ds_voice_play(id);
    return id;
}

static void group_ds_mix_sum(void) {
    int a = const_voice(24576, 64), b = const_voice(24576, 64);     /* 0.75 each */
    CHECK(a != b);
    ds_mix(g_out, 10);
    for (int i = 0; i < 20; i++) CHECKF(g_out[i], 1.0f);
    ds_voice_stop(a);
    CHECK(!ds_voice_playing(a));
    CHECKEQ(play_of(a), 10 * 4);
    ds_mix(g_out, 10);
    for (int i = 0; i < 20; i++) CHECKF(g_out[i], 0.75f);
    CHECKEQ(play_of(a), 10 * 4);
    CHECKEQ(play_of(b), 20 * 4);
    ds_voice_play(a);
    ds_mix(g_out, 1);
    CHECKEQ(play_of(a), 11 * 4);
    ds_voice_free(a);
    ds_voice_free(b);
}

static void group_ds_mix_cursor(void) {
    static const uint32_t ns[] = { 1, 511, 512, 5375, 5376, 5377 };
    for (size_t t = 0; t < sizeof ns / sizeof ns[0]; t++) {
        uint32_t n = ns[t], data = gblock(5376 * 4);
        int id = ds_voice_new(data, 5376 * 4, ST16);
        ds_voice_play(id);
        for (uint32_t done = 0; done < n;) {
            uint32_t c = n - done > 4096 ? 4096 : n - done;
            ds_mix(g_out, c);
            done += c;
        }
        uint32_t p, w;
        ds_voice_cursors(id, &p, &w);
        if (p != (n % 5376) * 4 || w != ((n % 5376 + 512) % 5376) * 4) {
            printf("FAIL n %u: play %u write %u\n", n, p, w);
            fails++;
        }
        ds_voice_free(id);
        hle_free(hle_ptr(data));
    }
}

/* ---- release while the mixer reads ---- */
static atomic_int g_mix_stop;
static uint64_t g_checked, g_over;
static void *mix_loop(void *arg) {
    (void)arg;
    static float buf[2 * 256];
    const float lim = 2.0f * 0x0100 / 32768.0f;
    while (!atomic_load(&g_mix_stop)) {
        ds_mix(buf, 256);
        for (int i = 0; i < 512; i++) {
            if (buf[i] > lim || buf[i] < 0.0f) {
                if (g_over++ < 4) printf("FAIL sample %.9g over %.9g\n", buf[i], lim);
            }
            g_checked++;
        }
    }
    return NULL;
}

static void group_ds_mix_free(void) {
    pthread_t th;
    pthread_create(&th, NULL, mix_loop, NULL);
    int ids[2];
    uint32_t blocks[2];
    int alive = 0;
    for (int i = 0; i < 1000; i++) {
        if (alive == 2) {
            ds_voice_free(ids[0]);
            uint16_t *p = (uint16_t *)hle_ptr(blocks[0]);
            for (int k = 0; k < 128; k++) p[k] = 0x7F7F;        /* poison before the heap sees it */
            hle_free(p);
            ids[0] = ids[1];
            blocks[0] = blocks[1];
            alive = 1;
        }
        uint32_t data = gblock(64 * 4);
        uint16_t *p = (uint16_t *)hle_ptr(data);
        for (int k = 0; k < 128; k++) p[k] = 0x0100;
        int id = ds_voice_new(data, 64 * 4, ST16);
        ds_voice_play(id);
        ids[alive] = id;
        blocks[alive] = data;
        alive++;
        usleep(20);
    }
    while (alive) {
        alive--;
        ds_voice_free(ids[alive]);
        hle_free(hle_ptr(blocks[alive]));
    }
    atomic_store(&g_mix_stop, 1);
    pthread_join(th, NULL);
    CHECKEQ(g_over, 0);
    CHECK(g_checked > 0);
    printf("ds_mix_free: checked %llu samples\n", (unsigned long long)g_checked);
}

/* ---- the output ---- */
static void group_ds_out_null(void) {
    ds_out_force_null_for_test();
    int id = long_voice();
    ds_out_start();
    double t0 = now_s();
    ds_out_start();                                     /* once: no second notice */
    sleep_ms(500);
    double el = now_s() - t0;
    uint32_t got = play_of(id) / 2;
    printf("ds_out_null: %u frames in %.3f s\n", got, el);
    CHECK(fabs((double)got - 22050.0) <= 2205.0);
    CHECK(ds_voice_playing(id));
    CHECKEQ(ds_out_skipped_frames_for_test(), 0);
}

static void group_ds_out_stall(void) {
    int id = long_voice();
    ds_out_start();
    double t0 = now_s();
    sleep_ms(100);
    uint32_t calm = ds_out_wake_max_for_test();
    CHECK(calm > 0 && calm < 2048);
    ds_out_stall_for_test(300);
    sleep_ms(600);
    double el = now_s() - t0;
    uint64_t skipped = ds_out_skipped_frames_for_test();
    uint32_t got = play_of(id) / 2;
    uint32_t wmax = ds_out_wake_max_for_test();
    printf("ds_out_stall: %u frames, %llu skipped, wake max %u (calm %u) in %.3f s\n", got,
           (unsigned long long)skipped, wmax, calm, el);
    CHECKEQ(wmax, 2048);                                /* the wake after the stall is capped */
    CHECK(skipped >= 8000);
    CHECK(fabs((double)(got + skipped) - el * 44100.0) <= 0.1 * el * 44100.0);
    CHECK((double)got < el * 44100.0 - 8000.0);         /* no burst to catch up */
}

static const char *g_exit_how;
static int g_exit_id;
static void *exit_later(void *arg) {
    (void)arg;
    sleep_ms(100);
    printf("ds_out_exit: consumed %u frames, leaving by %s\n", play_of(g_exit_id) / 2, g_exit_how);
    fflush(stdout);
    if (!strcmp(g_exit_how, "hle")) hle_exit(0);
    exit(0);
}

static void group_ds_out_exit(const char *how) {
    g_exit_how = how;
    g_exit_id = long_voice();
    ds_out_start();
    pthread_t th;
    pthread_create(&th, NULL, exit_later, NULL);
    for (;;) sleep(1);
}

static void group_ds_audio_log(void) {
    ds_out_manual_for_test();
    int id = const_voice(16384, 100);                   /* 0.5 */
    ds_out_start();
    uint64_t before = ds_frames_mixed();
    int bad = 0;
    for (uint32_t done = 0; done < 88200;) {
        uint32_t c = 88200 - done > 1000 ? 1000 : 88200 - done;
        ds_mix(g_out, c);
        for (uint32_t i = 0; i < 2 * c; i++) bad += g_out[i] != 0.5f;
        done += c;
    }
    CHECKEQ(bad, 0);
    CHECKEQ(ds_frames_mixed() - before, 88200);
    sleep_ms(50);
    CHECKEQ(ds_frames_mixed() - before, 88200);         /* manual: no sink mixes */
    ds_voice_free(id);
}

static void group_ds_device(void) {
    uint32_t frames = 2 * 44100, data = gblock(frames * 2);
    int16_t *s = s16(data);
    for (uint32_t i = 0; i < frames; i++) s[i] = (int16_t)lrint(3000.0 * sin(2.0 * M_PI * 440.0 * i / 44100.0));
    int id = ds_voice_new(data, frames * 2, MONO16);
    ds_voice_play(id);
    ds_out_start();
    double t0 = now_s();
    sleep_ms(1000);
    double el = now_s() - t0;
    uint32_t got = play_of(id) / 2;
    printf("ds_device: %u frames in %.3f s\n", got, el);
    CHECK(got >= 39690 && got <= 48510);
    ds_voice_stop(id);
}

/* ---- DirectSound (Task 3) ---- */
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
static uint32_t vcall(uint32_t obj, int slot, int n, ...) {
    uint32_t va = MEM32(MEM32(obj) + 4u * (uint32_t)slot);
    recomp_func_t f = recomp_lookup_import(va);
    if (!f) { printf("FAIL slot %d of 0x%08X: no method\n", slot, obj); fails++; return 0xDEADBEEFu; }
    uint32_t a[16] = { obj };
    va_list ap;
    va_start(ap, n);
    for (int i = 0; i < n; i++) a[1 + i] = va_arg(ap, uint32_t);
    va_end(ap);
    return run(f, g_cur_import, n + 1, a);
}

static uint32_t gmem(uint32_t n) { uint32_t p = gblock(n); memset(hle_ptr(p), 0, n); return p; }
#define G(a, b, c, d0, d1, d2, d3, d4, d5, d6, d7) \
    { (a) & 0xFF, ((a) >> 8) & 0xFF, ((a) >> 16) & 0xFF, ((a) >> 24) & 0xFF, (b) & 0xFF, ((b) >> 8) & 0xFF, \
      (c) & 0xFF, ((c) >> 8) & 0xFF, d0, d1, d2, d3, d4, d5, d6, d7 }
static const uint8_t CLSID_DirectSound[16] = G(0x47D4D946, 0x62E8, 0x11CF, 0x93, 0xBC, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00);
static const uint8_t CLSID_DirectSound8[16] = G(0x3901CC3F, 0x84B5, 0x4FA4, 0xBA, 0x35, 0xAA, 0x81, 0x72, 0xB8, 0xA0, 0x9B);
static const uint8_t CLSID_DirectSoundCapture[16] = G(0xB0210780, 0x89CD, 0x11D0, 0xAF, 0x08, 0x00, 0xA0, 0xC9, 0x25, 0xCD, 0x16);
static const uint8_t IID_IDirectSound[16] = G(0x279AFA83, 0x4981, 0x11CE, 0xA5, 0x21, 0x00, 0x20, 0xAF, 0x0B, 0xE5, 0x60);
static const uint8_t IID_IDirectSound8[16] = G(0xC50A7E93, 0xF395, 0x4834, 0x9E, 0xF6, 0x7F, 0xA9, 0x9D, 0xE5, 0x09, 0x66);
static const uint8_t IID_IUnknown[16] = G(0x00000000, 0x0000, 0x0000, 0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46);
static const uint8_t DSDEVID_DefaultPlayback[16] = G(0xDEF00000, 0x9C6D, 0x47ED, 0xAA, 0xF1, 0x4D, 0xDA, 0x8F, 0x2B, 0x5C, 0x03);
static const uint8_t DSDEVID_DefaultCapture[16] = G(0xDEF00001, 0x9C6D, 0x47ED, 0xAA, 0xF1, 0x4D, 0xDA, 0x8F, 0x2B, 0x5C, 0x03);
static uint32_t gguid(const uint8_t *g) { uint32_t p = gmem(16); memcpy(hle_ptr(p), g, 16); return p; }

#define DS_OK_ 0u
#define DSERR_INVALIDPARAM_ 0x80070057u
#define DSERR_UNINITIALIZED_ 0x887800AAu
#define DSERR_ALREADYINITIALIZED_ 0x88780082u
#define DSERR_NOAGGREGATION_ 0x80040110u
#define HWND 0x00010010u
#define GAME_FLAGS 0x18000u
#define GAME_BYTES 21504u

/* IDirectSound and IDirectSoundBuffer slots */
enum { DS_CreateSoundBuffer = 3, DS_GetCaps, DS_DuplicateSoundBuffer, DS_SetCooperativeLevel, DS_Compact,
       DS_GetSpeakerConfig, DS_SetSpeakerConfig, DS_Initialize };
enum { DSB_GetCaps = 3, DSB_GetCurrentPosition, DSB_GetFormat, DSB_GetVolume, DSB_GetPan, DSB_GetFrequency,
       DSB_GetStatus, DSB_Initialize, DSB_Lock, DSB_Play, DSB_SetCurrentPosition, DSB_SetFormat, DSB_SetVolume,
       DSB_SetPan, DSB_SetFrequency, DSB_Stop, DSB_Unlock, DSB_Restore };

/* CoCreateInstance(CLSID_DirectSound, NULL, CLSCTX_INPROC_SERVER, IID_IDirectSound, &ds), as the game makes it */
static uint32_t ds_cocreate(const uint8_t *clsid, const uint8_t *iid, uint32_t *hr) {
    uint32_t out = gmem(4);
    MEM32(out) = 0xCCCCCCCCu;
    *hr = call("CoCreateInstance", 5, gguid(clsid), 0u, 1u, gguid(iid), out);
    return MEM32(out);
}
static uint32_t ds_ready(void) {
    uint32_t hr, ds = ds_cocreate(CLSID_DirectSound, IID_IDirectSound, &hr);
    CHECKEQ(hr, 0);
    CHECKEQ(vcall(ds, DS_Initialize, 1, 0u), DS_OK_);
    CHECKEQ(vcall(ds, DS_SetCooperativeLevel, 2, HWND, 2u), DS_OK_);
    return ds;
}
/* A WAVEFORMATEX (18 bytes) and a DSBUFFERDESC of dwSize size. */
static uint32_t wave(uint16_t tag, uint16_t ch, uint32_t rate, uint16_t bits) {
    uint32_t w = gmem(40);
    uint8_t *p = hle_ptr(w);
    uint16_t block = (uint16_t)(ch * bits / 8);
    uint32_t avg = rate * block;
    memcpy(p, &tag, 2), memcpy(p + 2, &ch, 2), memcpy(p + 4, &rate, 4), memcpy(p + 8, &avg, 4);
    memcpy(p + 12, &block, 2), memcpy(p + 14, &bits, 2);
    return w;
}
static uint32_t desc(uint32_t size, uint32_t flags, uint32_t bytes, uint32_t wfx) {
    uint32_t d = gmem(36);
    MEM32(d) = size, MEM32(d + 4) = flags, MEM32(d + 8) = bytes, MEM32(d + 16) = wfx;
    return d;
}
static uint32_t make_buffer(uint32_t ds, uint32_t d, uint32_t *hr) {
    uint32_t out = gmem(4);
    MEM32(out) = 0xCCCCCCCCu;
    *hr = vcall(ds, DS_CreateSoundBuffer, 3, d, out, 0u);
    return MEM32(out);
}
static uint32_t game_buffer(uint32_t ds) {
    uint32_t hr, b = make_buffer(ds, desc(20, GAME_FLAGS, GAME_BYTES, wave(1, 2, 44100, 16)), &hr);
    CHECKEQ(hr, DS_OK_);
    CHECK(b != 0);
    return b;
}
struct lockout { uint32_t hr, p1, n1, p2, n2; };
static struct lockout lock(uint32_t b, uint32_t off, uint32_t bytes, uint32_t flags, int want_p2) {
    uint32_t o = gmem(16);
    for (int i = 0; i < 4; i++) MEM32(o + 4u * (uint32_t)i) = 0xCCCCCCCCu;
    struct lockout r;
    r.hr = vcall(b, DSB_Lock, 7, off, bytes, o, o + 4, want_p2 ? o + 8 : 0u, want_p2 ? o + 12 : 0u, flags);
    r.p1 = MEM32(o), r.n1 = MEM32(o + 4), r.p2 = MEM32(o + 8), r.n2 = MEM32(o + 12);
    return r;
}
static uint32_t status(uint32_t b) {
    uint32_t s = gmem(4);
    MEM32(s) = 0xCCCCCCCCu;
    CHECKEQ(vcall(b, DSB_GetStatus, 1, s), DS_OK_);
    return MEM32(s);
}
static uint32_t cursor(uint32_t b, int which) {
    uint32_t c = gmem(8);
    MEM32(c) = MEM32(c + 4) = 0xCCCCCCCCu;
    CHECKEQ(vcall(b, DSB_GetCurrentPosition, 2, which ? 0u : c, which ? c + 4 : 0u), DS_OK_);
    return which ? MEM32(c + 4) : MEM32(c);
}

static void group_ds_create(void) {
    uint32_t live0 = d3d_live_count(), hr;
    /* 1: as the game makes it; nothing answers before Initialize */
    uint32_t ds = ds_cocreate(CLSID_DirectSound, IID_IDirectSound, &hr);
    CHECKEQ(hr, 0);
    CHECK(ds != 0 && ds != 0xCCCCCCCCu);
    uint32_t c = gmem(4), out = gmem(4);
    CHECKEQ(vcall(ds, DS_CreateSoundBuffer, 3, desc(20, GAME_FLAGS, GAME_BYTES, wave(1, 2, 44100, 16)), out, 0u),
            DSERR_UNINITIALIZED_);
    CHECKEQ(MEM32(out), 0);
    CHECKEQ(vcall(ds, DS_SetCooperativeLevel, 2, HWND, 2u), DSERR_UNINITIALIZED_);
    CHECKEQ(vcall(ds, DS_GetSpeakerConfig, 1, c), DSERR_UNINITIALIZED_);
    /* 2 */
    CHECKEQ(vcall(ds, DS_Initialize, 1, 0u), DS_OK_);
    CHECKEQ(vcall(ds, DS_Initialize, 1, 0u), DSERR_ALREADYINITIALIZED_);
    /* 3 */
    CHECKEQ(vcall(ds, DS_SetCooperativeLevel, 2, HWND, 2u), DS_OK_);
    CHECKEQ(vcall(ds, DS_SetCooperativeLevel, 2, HWND, 1u), DS_OK_);
    CHECKEQ(vcall(ds, DS_SetCooperativeLevel, 2, HWND, 3u), DS_OK_);
    CHECKEQ(vcall(ds, DS_SetCooperativeLevel, 2, HWND, 2u), DS_OK_);
    MEM32(c) = 0xCCCCCCCCu;
    CHECKEQ(vcall(ds, DS_GetSpeakerConfig, 1, c), DS_OK_);
    CHECKEQ(MEM32(c), 0x00140004u);
    /* 4: the game's buffer */
    uint32_t b = game_buffer(ds);
    CHECK(b != ds);
    /* 5: the whole buffer, zeroed */
    struct lockout l = lock(b, 0, 0, 2, 1);
    CHECKEQ(l.hr, DS_OK_);
    CHECK(l.p1 != 0);
    CHECKEQ(l.n1, GAME_BYTES);
    CHECKEQ(l.p2, 0);
    CHECKEQ(l.n2, 0);
    int nonzero = 0;
    for (uint32_t i = 0; i < GAME_BYTES; i++) nonzero += ((uint8_t *)hle_ptr(l.p1))[i] != 0;
    CHECKEQ(nonzero, 0);
    CHECKEQ(vcall(b, DSB_Unlock, 4, l.p1, l.n1, 0u, 0u), DS_OK_);
    CHECKEQ(status(b), 0);
    CHECKEQ(cursor(b, 0), 0);
    CHECKEQ(cursor(b, 1), 512u * 4u);
    /* 6: playing, looping; the null sink moves the play cursor */
    CHECKEQ(vcall(b, DSB_Play, 3, 0u, 0u, 1u), DS_OK_);
    CHECKEQ(status(b), 5);
    uint32_t moved = 0;
    double t0 = now_s();
    while (!moved && now_s() - t0 < 0.2) { moved = cursor(b, 0) != 0; if (!moved) sleep_ms(2); }
    CHECK(moved);
    /* 7 */
    CHECKEQ(vcall(b, DSB_Stop, 0), DS_OK_);
    CHECKEQ(status(b), 0);
    uint32_t p = cursor(b, 0);
    sleep_ms(30);
    CHECKEQ(cursor(b, 0), p);                           /* stopped: the cursor stays */
    CHECKEQ(vcall(b, 2, 0), 0);                         /* the buffer's last Release */
    CHECKEQ(d3d_live_count(), live0 + 1);
    CHECKEQ(vcall(ds, 2, 0), 0);
    CHECKEQ(d3d_live_count(), live0);
    /* IUnknown makes the same object; it holds no buffer, so one Release ends it */
    ds = ds_cocreate(CLSID_DirectSound, IID_IUnknown, &hr);
    CHECKEQ(hr, 0);
    CHECKEQ(vcall(ds, DS_Initialize, 1, gguid(DSDEVID_DefaultPlayback)), DS_OK_);
    CHECKEQ(vcall(ds, 2, 0), 0);
    CHECKEQ(d3d_live_count(), live0);
}

static void group_ds_rules(void) {
    uint32_t ds = ds_ready(), hr, w = wave(1, 2, 44100, 16);
    CHECKEQ(vcall(ds, DS_SetCooperativeLevel, 2, HWND, 0u), DSERR_INVALIDPARAM_);
    CHECKEQ(vcall(ds, DS_SetCooperativeLevel, 2, HWND, 5u), DSERR_INVALIDPARAM_);
    CHECKEQ(vcall(ds, DS_GetSpeakerConfig, 1, 0u), DSERR_INVALIDPARAM_);
    uint32_t live = d3d_live_count();
    CHECKEQ(make_buffer(ds, desc(24, GAME_FLAGS, GAME_BYTES, w), &hr), 0);
    CHECKEQ(hr, DSERR_INVALIDPARAM_);
    CHECKEQ(make_buffer(ds, desc(20, GAME_FLAGS, GAME_BYTES + 2, w), &hr), 0);     /* not whole frames */
    CHECKEQ(hr, DSERR_INVALIDPARAM_);
    CHECKEQ(make_buffer(ds, desc(20, GAME_FLAGS, 0, w), &hr), 0);
    CHECKEQ(hr, DSERR_INVALIDPARAM_);
    CHECKEQ(make_buffer(ds, desc(20, GAME_FLAGS, 0x10000000u, w), &hr), 0);
    CHECKEQ(hr, DSERR_INVALIDPARAM_);
    CHECKEQ(make_buffer(ds, desc(20, GAME_FLAGS, 2, wave(1, 1, 22050, 8)), &hr), 0);   /* under 4 bytes */
    CHECKEQ(hr, DSERR_INVALIDPARAM_);
    CHECKEQ(make_buffer(ds, desc(20, GAME_FLAGS, GAME_BYTES, 0), &hr), 0);
    CHECKEQ(hr, DSERR_INVALIDPARAM_);
    CHECKEQ(vcall(ds, DS_CreateSoundBuffer, 3, 0u, gmem(4), 0u), DSERR_INVALIDPARAM_);
    CHECKEQ(vcall(ds, DS_CreateSoundBuffer, 3, desc(20, GAME_FLAGS, GAME_BYTES, w), 0u, 0u), DSERR_INVALIDPARAM_);
    uint32_t out = gmem(4);
    CHECKEQ(vcall(ds, DS_CreateSoundBuffer, 3, desc(20, GAME_FLAGS, GAME_BYTES, w), out, 0x1000u),
            DSERR_NOAGGREGATION_);
    CHECKEQ(d3d_live_count(), live);
    /* the 36-byte (DirectX 7) description, the other allowed flags and the formats the mixer plays */
    uint32_t b = make_buffer(ds, desc(36, 0x4008u, 4, wave(1, 1, 22050, 8)), &hr);
    CHECKEQ(hr, DS_OK_);
    CHECKEQ(vcall(b, 2, 0), 0);
    b = make_buffer(ds, desc(20, 0x18000u, 4000, wave(1, 1, 11025, 16)), &hr);
    CHECKEQ(hr, DS_OK_);
    CHECKEQ(vcall(b, DSB_GetStatus, 1, 0u), DSERR_INVALIDPARAM_);
    CHECKEQ(vcall(b, 2, 0), 0);
    CHECKEQ(d3d_live_count(), live);
    CHECKEQ(vcall(ds, 2, 0), 0);
}

static void group_ds_buffer_lock(void) {
    uint32_t ds = ds_ready(), b = game_buffer(ds);
    struct lockout all = lock(b, 0, 0, 2, 1);
    uint32_t data = all.p1;
    CHECKEQ(all.hr, DS_OK_);
    /* a lock that wraps: 504 bytes at the end, 520 at the start */
    struct lockout l = lock(b, 21000, 1024, 0, 1);
    CHECKEQ(l.hr, DS_OK_);
    CHECKEQ(l.p1, data + 21000);
    CHECKEQ(l.n1, 504);
    CHECKEQ(l.p2, data);
    CHECKEQ(l.n2, 520);
    CHECKEQ(vcall(b, DSB_Unlock, 4, l.p1, l.n1, l.p2, l.n2), DS_OK_);
    /* without a second pointer, only the first part */
    l = lock(b, 21000, 1024, 0, 0);
    CHECKEQ(l.hr, DS_OK_);
    CHECKEQ(l.p1, data + 21000);
    CHECKEQ(l.n1, 504);
    /* no wrap: the second part is empty */
    l = lock(b, 100, 512, 0, 1);
    CHECKEQ(l.hr, DS_OK_);
    CHECKEQ(l.p1, data + 100);
    CHECKEQ(l.n1, 512);
    CHECKEQ(l.p2, 0);
    CHECKEQ(l.n2, 0);
    /* past the end, larger than the buffer, empty */
    CHECKEQ(lock(b, 21504, 4, 0, 1).hr, DSERR_INVALIDPARAM_);
    CHECKEQ(lock(b, 0, 21508, 0, 1).hr, DSERR_INVALIDPARAM_);
    CHECKEQ(lock(b, 0, 0, 0, 1).hr, DSERR_INVALIDPARAM_);
    CHECKEQ(vcall(b, DSB_Lock, 7, 0u, 4u, 0u, 0u, 0u, 0u, 0u), DSERR_INVALIDPARAM_);
    /* from the write cursor (512 frames ahead of the play cursor) */
    l = lock(b, 999, 100, 1, 1);
    CHECKEQ(l.hr, DS_OK_);
    CHECKEQ(l.p1, data + 512u * 4u);
    CHECKEQ(l.n1, 100);
    /* the whole buffer from the write cursor wraps */
    l = lock(b, 0, 0, 3, 1);
    CHECKEQ(l.hr, DS_OK_);
    CHECKEQ(l.p1, data + 2048);
    CHECKEQ(l.n1, GAME_BYTES - 2048);
    CHECKEQ(l.p2, data);
    CHECKEQ(l.n2, 2048);
    /* Unlock: each nonzero pointer with its count lies in the buffer */
    CHECKEQ(vcall(b, DSB_Unlock, 4, data - 4, 8u, 0u, 0u), DSERR_INVALIDPARAM_);
    CHECKEQ(vcall(b, DSB_Unlock, 4, data + GAME_BYTES, 0u, 0u, 0u), DSERR_INVALIDPARAM_);
    CHECKEQ(vcall(b, DSB_Unlock, 4, data + 21000, 505u, 0u, 0u), DSERR_INVALIDPARAM_);
    CHECKEQ(vcall(b, DSB_Unlock, 4, data + 21000, 504u, data + 1, 520u), DS_OK_);
    CHECKEQ(vcall(b, DSB_Unlock, 4, data + 21000, 504u, data + 21504, 4u), DSERR_INVALIDPARAM_);
    CHECKEQ(vcall(b, DSB_Unlock, 4, data, GAME_BYTES, 0u, 0u), DS_OK_);
    CHECKEQ(vcall(b, 2, 0), 0);
    CHECKEQ(vcall(ds, 2, 0), 0);
}

/* The game's streaming loop (plan 8 "Measured" item 4) in C, with the test mixing. */
static void group_ds_buffer_stream(void) {
    ds_out_manual_for_test();
    uint32_t ds = ds_ready(), b = game_buffer(ds);
    struct lockout all = lock(b, 0, 0, 2, 1);
    memset(hle_ptr(all.p1), 0, all.n1);
    CHECKEQ(vcall(b, DSB_Unlock, 4, all.p1, all.n1, 0u, 0u), DS_OK_);
    CHECKEQ(vcall(b, DSB_Play, 3, 0u, 0u, 1u), DS_OK_);
    enum { ROUNDS = 200, MIX = 441 };
    static float out[2 * ROUNDS * MIX];
    uint32_t wo = 0, chunks = 0;
    uint16_t next = 0;
    for (int r = 0; r < ROUNDS; r++) {
        uint32_t play = cursor(b, 0);
        uint32_t free_ = (play + GAME_BYTES - wo) % GAME_BYTES;
        while (free_ >= 512) {
            struct lockout l = lock(b, wo, 512, 0, 1);
            CHECKEQ(l.hr, DS_OK_);
            CHECKEQ(l.n1 + l.n2, 512);
            for (uint32_t k = 0; k < l.n1 / 2; k++) s16(l.p1)[k] = (int16_t)next++;
            for (uint32_t k = 0; k < l.n2 / 2; k++) s16(l.p2)[k] = (int16_t)next++;
            CHECKEQ(vcall(b, DSB_Unlock, 4, l.p1, l.n1, l.p2, l.n2), DS_OK_);
            wo = (wo + 512) % GAME_BYTES;
            free_ -= 512;
            chunks++;
        }
        ds_mix(out + 2 * (size_t)r * MIX, MIX);
    }
    int bad = 0;
    for (uint32_t f = 0; f < ROUNDS * MIX; f++) {
        float wl = 0.0f, wr = 0.0f;
        if (f >= GAME_BYTES / 4) {                      /* after the first, silent, buffer */
            uint32_t j = f - GAME_BYTES / 4;
            wl = (float)(int16_t)(uint16_t)(2 * j) / 32768.0f;
            wr = (float)(int16_t)(uint16_t)(2 * j + 1) / 32768.0f;
        }
        if (out[2 * f] != wl || out[2 * f + 1] != wr) {
            if (bad++ < 4) printf("FAIL frame %u: %.9g %.9g, want %.9g %.9g\n", f, out[2 * f], out[2 * f + 1], wl, wr);
        }
    }
    CHECKEQ(bad, 0);
    printf("ds_buffer_stream: %u chunks written, %u frames checked\n", chunks, ROUNDS * MIX);
    CHECK(chunks * 512u + 511u >= (ROUNDS - 1) * MIX * 4u);      /* up to the last round's play cursor */
    CHECKEQ(vcall(b, 2, 0), 0);
    CHECKEQ(vcall(ds, 2, 0), 0);
}

/* Each case stops the run; it prints "ds_stops: returned" only if it did not. */
static void group_ds_stops(const char *what) {
    uint32_t hr;
    if (!strcmp(what, "flags")) {
        uint32_t ds = ds_ready();
        make_buffer(ds, desc(20, 0x18014u, GAME_BYTES, wave(1, 2, 44100, 16)), &hr);
    } else if (!strcmp(what, "ds8")) {
        ds_cocreate(CLSID_DirectSound8, IID_IDirectSound, &hr);
    } else if (!strcmp(what, "iid")) {
        ds_cocreate(CLSID_DirectSound, IID_IDirectSound8, &hr);
    } else if (!strcmp(what, "extensible")) {
        uint32_t ds = ds_ready(), w = wave(0xFFFE, 2, 44100, 16);
        MEM32(w + 16) = 22;                             /* cbSize of WAVEFORMATEXTENSIBLE */
        make_buffer(ds, desc(20, GAME_FLAGS, GAME_BYTES, w), &hr);
    } else if (!strcmp(what, "format")) {
        uint32_t ds = ds_ready();
        make_buffer(ds, desc(20, GAME_FLAGS, GAME_BYTES, wave(1, 6, 44100, 16)), &hr);
    } else if (!strcmp(what, "play")) {
        uint32_t ds = ds_ready(), b = game_buffer(ds);
        vcall(b, DSB_Play, 3, 0u, 0u, 0u);
    } else if (!strcmp(what, "lockflags")) {
        uint32_t ds = ds_ready(), b = game_buffer(ds);
        lock(b, 0, 4, 4, 1);
    } else if (!strcmp(what, "coop")) {
        uint32_t ds = ds_ready();
        vcall(ds, DS_SetCooperativeLevel, 2, HWND, 4u);
    } else if (!strcmp(what, "dscaps")) {
        uint32_t ds = ds_ready();
        vcall(ds, DS_GetCaps, 1, gmem(96));
    } else if (!strcmp(what, "caps")) {
        uint32_t ds = ds_ready(), b = game_buffer(ds);
        vcall(b, DSB_GetCaps, 1, gmem(20));
    } else if (!strcmp(what, "volume")) {
        uint32_t ds = ds_ready(), b = game_buffer(ds);
        vcall(b, DSB_SetVolume, 1, 0u);
    } else if (!strcmp(what, "capture")) {
        ds_cocreate(CLSID_DirectSoundCapture, IID_IUnknown, &hr);
    } else if (!strcmp(what, "create")) {
        call("DirectSoundCreate", 3, 0u, gmem(4), 0u);
    } else if (!strcmp(what, "create8")) {
        call("DirectSoundCreate8", 3, 0u, gmem(4), 0u);
    } else if (!strcmp(what, "enumerate")) {
        call("DirectSoundEnumerateA", 2, 0x12345678u, 0u);
    } else if (!strcmp(what, "init")) {
        uint32_t ds = ds_cocreate(CLSID_DirectSound, IID_IDirectSound, &hr);
        vcall(ds, DS_Initialize, 1, gguid(DSDEVID_DefaultCapture));
    } else if (!strcmp(what, "openal")) {
        uint32_t name = gmem(32);
        strcpy(hle_ptr(name), "OpenAL32.dll");
        call("LoadLibraryA", 1, name);
    } else {
        printf("FAIL no stop case %s\n", what);
        fails++;
        return;
    }
    printf("ds_stops: returned\n");
}

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: audio_selftest <group> [arg]\n"); return 2; }
    hle_mem_init();
    bw2_register_shims();
    mach_enter();
    g_esp = hle_va(hle_alloc(1u << 20)) + (1u << 20) - 64;
    if (!strcmp(argv[1], "ds_mix_exact")) group_ds_mix_exact();
    else if (!strcmp(argv[1], "ds_mix_formats")) group_ds_mix_formats();
    else if (!strcmp(argv[1], "ds_mix_sum")) group_ds_mix_sum();
    else if (!strcmp(argv[1], "ds_mix_cursor")) group_ds_mix_cursor();
    else if (!strcmp(argv[1], "ds_mix_free")) group_ds_mix_free();
    else if (!strcmp(argv[1], "ds_out_null")) group_ds_out_null();
    else if (!strcmp(argv[1], "ds_out_stall")) group_ds_out_stall();
    else if (!strcmp(argv[1], "ds_out_exit") && argc > 2) group_ds_out_exit(argv[2]);
    else if (!strcmp(argv[1], "ds_audio_log")) group_ds_audio_log();
    else if (!strcmp(argv[1], "ds_device")) group_ds_device();
    else if (!strcmp(argv[1], "ds_create")) group_ds_create();
    else if (!strcmp(argv[1], "ds_rules")) group_ds_rules();
    else if (!strcmp(argv[1], "ds_buffer_lock")) group_ds_buffer_lock();
    else if (!strcmp(argv[1], "ds_buffer_stream")) group_ds_buffer_stream();
    else if (!strcmp(argv[1], "ds_stops") && argc > 2) group_ds_stops(argv[2]);
    else { fprintf(stderr, "audio_selftest: no group %s\n", argv[1]); return 2; }
    printf("%s: %d failed\n", argv[1], fails);
    fflush(stdout);
    return fails != 0;
}
