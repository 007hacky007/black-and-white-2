/*
 * ds_out.m - where the mix goes (plan 8 Task 2).
 *
 * The unit: CoreAudio's default output AudioUnit, fed Float32 interleaved
 * stereo at 44,100 Hz (it converts to the device's rate). Its render callback
 * only calls ds_mix: no allocation, no guest code, no lock but the mixer's.
 *
 * The null sink: headless, or when a step of the unit fails (one notice line),
 * a host thread mixes the frames that are due at the real-time rate into a
 * scratch block every 5 ms, so the game's cursors still move. A wake mixes at
 * most 2,048 frames (under half the game's 5,376-frame buffer); a larger
 * backlog, from the sink's own thread stalling, is dropped by counting it as
 * skipped, so the sink never bursts past the game's write offset.
 *
 * Exit: hle_exit ends in _exit, which runs no atexit handler and tears the
 * process down at once, so the render callback has nothing to race. For
 * exit() paths only, an atexit handler stops the unit (AudioOutputUnitStop
 * waits for the callback) or stops and joins the sink. It takes no machine
 * lock.
 */
#include <AudioToolbox/AudioToolbox.h>
#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include "bw2.h"
#include "audio.h"

#define SINK_WAKE_NS 5000000L
#define SINK_MAX_FRAMES 2048u

enum { OUT_NONE, OUT_UNIT, OUT_NULL };

static pthread_mutex_t g_start_mu = PTHREAD_MUTEX_INITIALIZER;
static int g_started, g_kind = OUT_NONE, g_force_null, g_manual;
static AudioComponentInstance g_unit;

static pthread_t g_sink;
static atomic_int g_sink_stop;
static atomic_uint g_stall_ms, g_wake_max;
static _Atomic uint64_t g_skipped;
static double g_sink_t0;

void ds_out_force_null_for_test(void) { g_force_null = 1; }
void ds_out_manual_for_test(void) { g_manual = 1; }
void ds_out_stall_for_test(uint32_t ms) { atomic_store(&g_stall_ms, ms); }
uint64_t ds_out_skipped_frames_for_test(void) { return atomic_load(&g_skipped); }
uint32_t ds_out_wake_max_for_test(void) { return atomic_load(&g_wake_max); }

static double now_s(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec + (double)t.tv_nsec * 1e-9;
}

static OSStatus render(void *ref, AudioUnitRenderActionFlags *flags, const AudioTimeStamp *ts, UInt32 bus,
                       UInt32 frames, AudioBufferList *io) {
    (void)ref; (void)flags; (void)ts; (void)bus;
    if (io->mNumberBuffers != 1 || io->mBuffers[0].mDataByteSize < frames * 8u) {
        fprintf(stderr, "[bw2] FATAL: audio: render asked for %u buffers of %u bytes for %u frames\n",
                (unsigned)io->mNumberBuffers, (unsigned)io->mBuffers[0].mDataByteSize, (unsigned)frames);
        abort();
    }
    ds_mix((float *)io->mBuffers[0].mData, frames);
    return noErr;
}

/* Every step of the unit; on a failure the unit is gone and the status is given. */
static OSStatus unit_start(void) {
    AudioComponentDescription d = { .componentType = kAudioUnitType_Output,
                                    .componentSubType = kAudioUnitSubType_DefaultOutput,
                                    .componentManufacturer = kAudioUnitManufacturer_Apple };
    AudioComponent c = AudioComponentFindNext(NULL, &d);
    if (!c) return -1;                                  /* no status to give: no component */
    OSStatus s = AudioComponentInstanceNew(c, &g_unit);
    if (s != noErr) { g_unit = NULL; return s; }
    AudioStreamBasicDescription f = {
        .mSampleRate = DS_OUT_RATE, .mFormatID = kAudioFormatLinearPCM,
        .mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked,
        .mBytesPerPacket = 8, .mFramesPerPacket = 1, .mBytesPerFrame = 8, .mChannelsPerFrame = 2,
        .mBitsPerChannel = 32,
    };
    AURenderCallbackStruct cb = { .inputProc = render, .inputProcRefCon = NULL };
    s = AudioUnitSetProperty(g_unit, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Input, 0, &f, sizeof f);
    if (s == noErr)
        s = AudioUnitSetProperty(g_unit, kAudioUnitProperty_SetRenderCallback, kAudioUnitScope_Input, 0, &cb,
                                 sizeof cb);
    if (s == noErr) s = AudioUnitInitialize(g_unit);
    if (s == noErr) s = AudioOutputUnitStart(g_unit);
    if (s != noErr) {
        AudioUnitUninitialize(g_unit);
        AudioComponentInstanceDispose(g_unit);
        g_unit = NULL;
    }
    return s;
}

static void *sink_main(void *arg) {
    (void)arg;
    static float scratch[2 * SINK_MAX_FRAMES];
    uint64_t mixed = 0;
    while (!atomic_load(&g_sink_stop)) {
        struct timespec ts = { 0, SINK_WAKE_NS };
        nanosleep(&ts, NULL);
        uint32_t stall = atomic_exchange(&g_stall_ms, 0);
        if (stall) {
            struct timespec st = { stall / 1000u, (long)(stall % 1000u) * 1000000L };
            nanosleep(&st, NULL);
        }
        if (atomic_load(&g_sink_stop)) break;
        uint64_t target = (uint64_t)floor((now_s() - g_sink_t0) * DS_OUT_RATE);
        uint64_t skipped = atomic_load(&g_skipped);
        if (target <= skipped + mixed) continue;
        uint64_t due = target - skipped - mixed;
        if (due > SINK_MAX_FRAMES) {
            atomic_fetch_add(&g_skipped, due - SINK_MAX_FRAMES);
            due = SINK_MAX_FRAMES;
        }
        ds_mix(scratch, (uint32_t)due);
        mixed += due;
        if (due > atomic_load(&g_wake_max)) atomic_store(&g_wake_max, (uint32_t)due);
    }
    return NULL;
}

static void out_at_exit(void) {
    if (g_kind == OUT_UNIT) {
        AudioOutputUnitStop(g_unit);
    } else if (g_kind == OUT_NULL) {
        atomic_store(&g_sink_stop, 1);
        pthread_join(g_sink, NULL);
    }
    if (ds_audio_log()) {
        fprintf(stderr, "[bw2] audio: output stopped at exit\n");
        fflush(stderr);
    }
}

void ds_out_start(void) {
    pthread_mutex_lock(&g_start_mu);
    if (g_started) { pthread_mutex_unlock(&g_start_mu); return; }
    g_started = 1;
    if (g_manual) { pthread_mutex_unlock(&g_start_mu); return; }
    /* Forced (a test) and a failed unit give the notice; headless is quietly null. */
    int use_null = 1;
    if (g_force_null || !bw2_headless()) {
        OSStatus s = g_force_null ? -1 : unit_start();
        if (s == noErr) {
            g_kind = OUT_UNIT;
            use_null = 0;
        } else {
            fprintf(stderr, "[bw2] audio: no output device (OSStatus %d), mixing to the null sink\n", (int)s);
            fflush(stderr);
        }
    }
    if (use_null) {
        g_sink_t0 = now_s();
        int e = pthread_create(&g_sink, NULL, sink_main, NULL);
        if (e) {
            fprintf(stderr, "[bw2] FATAL: audio: the null sink's thread did not start (error %d)\n", e);
            abort();
        }
        g_kind = OUT_NULL;
    }
    atexit(out_at_exit);
    pthread_mutex_unlock(&g_start_mu);
}
