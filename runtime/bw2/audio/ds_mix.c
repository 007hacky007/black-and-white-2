/*
 * ds_mix.c - the host mixer (plan 8 Task 2). A growable table of voices, each
 * a looping PCM ring in guest memory; ds_mix adds the playing ones into
 * interleaved float stereo at DS_OUT_RATE.
 *
 * One mutex guards the table and every voice's position. It is a leaf: it is
 * held only inside these functions, never while taking the machine lock, and
 * ds_mix allocates nothing and never enters guest code, so the CoreAudio
 * render callback may call it. ds_voice_free takes the voice out under the
 * mutex, so once it returns no ds_mix reads its data again.
 *
 * Mixing: 16-bit samples are s/32768, 8-bit (s - 128)/128, mono goes to both
 * channels, gain is 1. A voice's position is 32.32 fixed point in source
 * frames and advances by rate/44,100 per output frame; the sample is linear
 * between frame k and k+1 (k+1 wraps to 0). At 44,100 Hz the fraction stays
 * 0 and the output is the source exactly. The sum is clamped to [-1, 1].
 *
 * BW2_AUDIO_LOG=1 (read once) prints, after each 44,100 mixed frames, the
 * playing count, the peak and the non-silent frames: numbers only.
 */
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "win32hle.h"
#include "audio.h"

struct voice {
    uint32_t data, bytes, frames, block;
    uint16_t channels, bits;
    uint64_t step;          /* source frames per output frame, 32.32 */
    uint64_t consumed;      /* whole source frames read */
    uint32_t idx;           /* consumed mod frames */
    uint32_t frac;          /* the position's fraction, 0.32 */
    uint8_t live, playing;
};

static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static struct voice *g_v;            /* id i is g_v[i - 1] */
static int g_n, g_cap;
static uint64_t g_mixed;

/* the audio log's second, under g_mu */
static uint32_t g_sec_frames, g_sec_nonsilent, g_sec_index;
static float g_sec_peak;

static _Noreturn void die(const char *fmt, int a, int b, int c) {
    fprintf(stderr, "[bw2] FATAL: audio: ");
    fprintf(stderr, fmt, a, b, c);
    fprintf(stderr, "\n");
    fflush(stderr);
    abort();
}

int ds_audio_log(void) {
    static atomic_int on = -1;
    int v = atomic_load(&on);
    if (v < 0) {
        const char *e = getenv("BW2_AUDIO_LOG");
        v = e && !strcmp(e, "1");
        atomic_store(&on, v);
    }
    return v;
}

static struct voice *get(int id) {          /* under g_mu */
    if (id < 1 || id > g_n || !g_v[id - 1].live) {
        pthread_mutex_unlock(&g_mu);
        die("no voice %d (%d slots, %d)", id, g_n, 0);
    }
    return &g_v[id - 1];
}

int ds_voice_new(uint32_t data, uint32_t bytes, struct ds_format f) {
    if ((f.channels != 1 && f.channels != 2) || (f.bits != 8 && f.bits != 16))
        die("voice format %d channels, %d bits not handled (rate %d)", f.channels, f.bits, (int)f.rate);
    if (f.rate < 100 || f.rate > 200000) die("voice rate %d not handled (%d, %d)", (int)f.rate, 0, 0);
    uint32_t block = f.channels * (f.bits / 8u);
    if (!data || bytes < block || bytes % block)
        die("voice of %d bytes at 0x%08X is not whole %d-byte frames", (int)bytes, (int)data, (int)block);
    pthread_mutex_lock(&g_mu);
    int slot = 0;
    while (slot < g_n && g_v[slot].live) slot++;
    if (slot == g_n) {
        if (g_n == g_cap) {
            int cap = g_cap ? 2 * g_cap : 16;
            struct voice *nv = realloc(g_v, (size_t)cap * sizeof *nv);
            if (!nv) { pthread_mutex_unlock(&g_mu); die("no memory for %d voices (%d, %d)", cap, 0, 0); }
            g_v = nv;
            g_cap = cap;
        }
        g_n++;
    }
    g_v[slot] = (struct voice){
        .data = data, .bytes = bytes, .frames = bytes / block, .block = block,
        .channels = f.channels, .bits = f.bits,
        .step = ((uint64_t)f.rate << 32) / DS_OUT_RATE, .live = 1,
    };
    pthread_mutex_unlock(&g_mu);
    return slot + 1;
}

void ds_voice_free(int id) {
    pthread_mutex_lock(&g_mu);
    get(id)->live = 0;
    pthread_mutex_unlock(&g_mu);
}

void ds_voice_play(int id) {
    pthread_mutex_lock(&g_mu);
    get(id)->playing = 1;
    pthread_mutex_unlock(&g_mu);
}

void ds_voice_stop(int id) {
    pthread_mutex_lock(&g_mu);
    get(id)->playing = 0;
    pthread_mutex_unlock(&g_mu);
}

int ds_voice_playing(int id) {
    pthread_mutex_lock(&g_mu);
    int p = get(id)->playing;
    pthread_mutex_unlock(&g_mu);
    return p;
}

void ds_voice_cursors(int id, uint32_t *play, uint32_t *write) {
    pthread_mutex_lock(&g_mu);
    struct voice *v = get(id);
    uint32_t p = v->idx * v->block;
    uint32_t w = (uint32_t)(((uint64_t)p + (uint64_t)DS_LEAD_FRAMES * v->block) % v->bytes);
    pthread_mutex_unlock(&g_mu);
    if (play) *play = p;
    if (write) *write = w;
}

uint64_t ds_frames_mixed(void) {
    pthread_mutex_lock(&g_mu);
    uint64_t n = g_mixed;
    pthread_mutex_unlock(&g_mu);
    return n;
}

static inline float sample(const struct voice *v, const uint8_t *base, uint32_t frame, int ch) {
    const uint8_t *p = base + (size_t)frame * v->block;
    if (v->bits == 16) {
        int16_t s;
        memcpy(&s, p + 2 * ch, 2);
        return (float)s / 32768.0f;
    }
    return (float)((int)p[ch] - 128) / 128.0f;
}

static void mix_voice(struct voice *v, float *out, uint32_t frames) {     /* under g_mu */
    const uint8_t *base = (const uint8_t *)hle_ptr(v->data);
    int right = v->channels == 2;
    for (uint32_t i = 0; i < frames; i++) {
        uint32_t k = v->idx;
        float l = sample(v, base, k, 0), r = right ? sample(v, base, k, 1) : l;
        if (v->frac) {
            uint32_t k1 = k + 1 == v->frames ? 0 : k + 1;
            float t = (float)((double)v->frac / 4294967296.0);
            float l1 = sample(v, base, k1, 0), r1 = right ? sample(v, base, k1, 1) : l1;
            l += t * (l1 - l);
            r += t * (r1 - r);
        }
        out[2 * i] += l;
        out[2 * i + 1] += r;
        uint64_t f = (uint64_t)v->frac + v->step;
        uint32_t adv = (uint32_t)(f >> 32);
        v->frac = (uint32_t)f;
        if (adv) {
            v->consumed += adv;
            v->idx = (uint32_t)(((uint64_t)v->idx + adv) % v->frames);
        }
    }
}

void ds_mix(float *out, uint32_t frames) {
    int log = ds_audio_log();
    uint32_t done = 0;
    while (done < frames) {
        int line = 0;
        uint32_t l_index = 0, l_playing = 0, l_peak = 0, l_nonsilent = 0;
        pthread_mutex_lock(&g_mu);
        uint32_t c = frames - done;
        if (log && c > DS_OUT_RATE - g_sec_frames) c = DS_OUT_RATE - g_sec_frames;
        float *o = out + 2 * (size_t)done;
        memset(o, 0, (size_t)c * 2 * sizeof *o);
        uint32_t playing = 0;
        for (int i = 0; i < g_n; i++) {
            struct voice *v = &g_v[i];
            if (v->live && v->playing) { mix_voice(v, o, c); playing++; }
        }
        for (uint32_t i = 0; i < 2 * c; i++) {
            if (o[i] > 1.0f) o[i] = 1.0f;
            else if (o[i] < -1.0f) o[i] = -1.0f;
        }
        g_mixed += c;
        if (log) {
            for (uint32_t i = 0; i < c; i++) {
                float a = o[2 * i] < 0 ? -o[2 * i] : o[2 * i], b = o[2 * i + 1] < 0 ? -o[2 * i + 1] : o[2 * i + 1];
                if (a > g_sec_peak) g_sec_peak = a;
                if (b > g_sec_peak) g_sec_peak = b;
                if (a != 0.0f || b != 0.0f) g_sec_nonsilent++;
            }
            g_sec_frames += c;
            if (g_sec_frames == DS_OUT_RATE) {
                line = 1;
                l_index = ++g_sec_index;
                l_playing = playing;
                l_peak = (uint32_t)((double)g_sec_peak * 32767.0);
                l_nonsilent = g_sec_nonsilent;
                g_sec_frames = g_sec_nonsilent = 0;
                g_sec_peak = 0.0f;
            }
        }
        pthread_mutex_unlock(&g_mu);
        if (line) {
            fprintf(stderr, "[bw2] audio second %u: %u playing, peak %u, nonsilent %u of %u\n", l_index, l_playing,
                    l_peak, l_nonsilent, (unsigned)DS_OUT_RATE);
            fflush(stderr);
        }
        done += c;
    }
}
