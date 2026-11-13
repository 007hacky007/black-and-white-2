/*
 * bink_dec.h - a Bink 1 movie decoded with FFmpeg (libavformat's Bink demuxer,
 * libavcodec's Bink video decoder), read in place from bytes the caller holds.
 * No FFmpeg header is included here: only bink_dec.c sees FFmpeg.
 */
#ifndef BW2_BINK_DEC_H
#define BW2_BINK_DEC_H
#include <stdint.h>

struct bk_movie;
/* From the movie's 44-byte header: size, frame count, rate (rate / rate_div fps),
 * audio tracks, and bytes = the declared size + 8 (the whole movie). */
struct bk_info { uint32_t width, height, frames, rate, rate_div, tracks, bytes; };

/* The audio track count from the first 44 bytes alone; -1 if they are not a Bink header. */
int bk_peek_tracks(const uint8_t *data, uint32_t len);
/* Opens the movie at data (len bytes readable there; the caller keeps them valid until
 * bk_close). NULL with *err set if it is not a Bink movie, is truncated, or FFmpeg
 * cannot read it. Audio tracks are never opened. */
struct bk_movie *bk_open(const uint8_t *data, uint32_t len, const char **err);
const struct bk_info *bk_info_of(const struct bk_movie *m);
/* 1..frames (anything else stops loudly): the frame bk_frame gives next. */
void bk_goto(struct bk_movie *m, uint32_t frame);
uint32_t bk_current(const struct bk_movie *m);
/* Decodes the current frame, once: frames skipped since the last decode are decoded and
 * dropped, an earlier frame rewinds to frame 1. 1 done, 0 on a decoder error. */
int bk_frame(struct bk_movie *m);
/* The decoded frame into dst (pitch bytes per row, dst_h rows) at (x, y), clipped to
 * dst_h rows and pitch / 4 columns. type 3 BINKSURFACE32 (B, G, R, 0xFF), 5
 * BINKSURFACE32A (B, G, R, alpha or 0xFF). 0 done; -1 type not supported; -2 no
 * frame decoded yet (nothing written). */
int bk_copy(const struct bk_movie *m, uint8_t *dst, uint32_t pitch, uint32_t dst_h,
            uint32_t x, uint32_t y, uint32_t type);
uint32_t bk_decoded(const struct bk_movie *m);      /* frames decoded since bk_open (checks) */
void bk_close(struct bk_movie *m);

#endif
