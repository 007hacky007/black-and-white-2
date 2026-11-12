/*
 * audio_selftest.c - the host mixer and its output (plan 8 Task 2), driven
 * directly. Sample data lives in hle_alloc blocks, as the game's buffers will.
 * Built with the engine (minus main.c) by tests/test_audio.py;
 * `audio_selftest <group> [arg]` runs one group and prints "<group>: <n> failed".
 */
#include <math.h>
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
    else { fprintf(stderr, "audio_selftest: no group %s\n", argv[1]); return 2; }
    printf("%s: %d failed\n", argv[1], fails);
    fflush(stdout);
    return fails != 0;
}
