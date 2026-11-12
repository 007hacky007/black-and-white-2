/*
 * audio.h - the host mixer (ds_mix.c) and its output (ds_out.m), plan 8 Task 2.
 *
 * A voice is a looping PCM ring in guest memory. ds_mix adds the playing
 * voices into interleaved float stereo at DS_OUT_RATE, under one mutex (a
 * leaf: nothing takes the machine lock while holding it). The output calls
 * ds_mix from a CoreAudio render callback, or from a real-time null sink
 * thread when headless or when the unit cannot start. Neither ever enters
 * guest code.
 */
#ifndef BW2_AUDIO_H
#define BW2_AUDIO_H
#include <stdint.h>

#define DS_OUT_RATE 44100
#define DS_LEAD_FRAMES 512          /* the write cursor's lead over the play cursor */

/* PCM: 1 or 2 channels, 8 (unsigned) or 16 (signed) bits, rate 100 to 200,000. */
struct ds_format { uint32_t rate; uint16_t channels, bits; };

int  ds_voice_new(uint32_t data, uint32_t bytes, struct ds_format f);  /* stopped at 0; a positive id */
void ds_voice_free(int id);         /* after it returns the mixer never reads the data again */
void ds_voice_play(int id);         /* looping */
void ds_voice_stop(int id);         /* the position is kept */
void ds_voice_cursors(int id, uint32_t *play, uint32_t *write);       /* bytes; either may be NULL */
int  ds_voice_playing(int id);

void     ds_mix(float *out, uint32_t frames);   /* frames interleaved stereo frames */
uint64_t ds_frames_mixed(void);

void ds_out_start(void);            /* once, at the first voice */
int  ds_audio_log(void);            /* BW2_AUDIO_LOG=1, read once */

/* Tests only. */
void     ds_out_force_null_for_test(void);     /* the next start acts as if the unit failed */
void     ds_out_manual_for_test(void);         /* start nothing; the test calls ds_mix */
void     ds_out_stall_for_test(uint32_t ms);   /* the null sink's next wake sleeps ms first */
uint64_t ds_out_skipped_frames_for_test(void); /* frames the null sink has dropped */
uint32_t ds_out_wake_max_for_test(void);       /* the most frames one null-sink wake has mixed */

#endif
