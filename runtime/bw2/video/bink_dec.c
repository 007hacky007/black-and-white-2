/*
 * bink_dec.c - a Bink 1 movie decoded with FFmpeg, read in place (plan 10 Task 5).
 *
 * Adapted from pcrecomp's MIT runtime/win32hle/bink.c (its FFmpeg path: the
 * AVIOContext over the movie's bytes, the demux and decode loop, the rewind to
 * the start, the YUV to RGB copy). The guest API (binkw32's imports) is not
 * here; this is the host side it calls.
 *
 * libavformat's Bink demuxer reads the movie through read and seek callbacks
 * over the caller's bytes (nothing is copied; the caller keeps them valid until
 * bk_close), and libavcodec's Bink video decoder decodes it. Audio streams are
 * discarded and never opened. The demuxer seeks only to the first frame, so an
 * earlier frame is reached by rewinding and decoding forward (Ruling 5).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/avutil.h>
#include "bink_dec.h"

struct bk_movie {
    struct bk_info info;
    const uint8_t *data;
    int64_t pos;                  /* the AVIO read position in data */
    AVFormatContext *fmt;
    AVIOContext *io;
    AVCodecContext *vdec;
    AVFrame *frame;
    AVPacket *pkt;
    int vs;
    uint32_t cur;                 /* the frame bk_frame gives next */
    uint32_t read;                /* the last frame decoded from the stream (0: none since the start) */
    uint32_t have;                /* the frame in `frame` (0: none) */
    uint32_t decoded;             /* every frame decoded since bk_open */
};

static uint32_t rd32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
/* "BIK" and a revision letter */
static int is_magic(const uint8_t *d) { return d[0] == 'B' && d[1] == 'I' && d[2] == 'K' && d[3] >= 'a' && d[3] <= 'z'; }
static int is_bink(const uint8_t *d, uint32_t len) { return len >= 44 && is_magic(d); }

int bk_peek_tracks(const uint8_t *data, uint32_t len) {
    if (!data || !is_bink(data, len)) return -1;
    uint32_t t = rd32(data + 40);
    return t > 0x7FFFFFFFu ? -1 : (int)t;
}

static int io_read(void *opaque, uint8_t *buf, int n) {
    struct bk_movie *m = opaque;
    int64_t left = (int64_t)m->info.bytes - m->pos;
    if (left <= 0) return AVERROR_EOF;
    if (n > left) n = (int)left;
    memcpy(buf, m->data + m->pos, (size_t)n);
    m->pos += n;
    return n;
}
static int64_t io_seek(void *opaque, int64_t off, int whence) {
    struct bk_movie *m = opaque;
    int64_t size = m->info.bytes, to;
    whence &= ~AVSEEK_FORCE;
    if (whence == AVSEEK_SIZE) return size;
    if (whence == SEEK_SET) to = off;
    else if (whence == SEEK_CUR) to = m->pos + off;
    else if (whence == SEEK_END) to = size + off;
    else return AVERROR(EINVAL);
    if (to < 0 || to > size) return AVERROR(EINVAL);
    return m->pos = to;
}

void bk_close(struct bk_movie *m) {
    if (!m) return;
    if (m->vdec) avcodec_free_context(&m->vdec);
    if (m->fmt) avformat_close_input(&m->fmt);
    if (m->io) { av_freep(&m->io->buffer); avio_context_free(&m->io); }
    av_frame_free(&m->frame);
    av_packet_free(&m->pkt);
    free(m);
}

static struct bk_movie *fail(struct bk_movie *m, const char **err, const char *why) {
    if (err) *err = why;
    bk_close(m);
    return NULL;
}

struct bk_movie *bk_open(const uint8_t *data, uint32_t len, const char **err) {
    if (!data) return fail(NULL, err, "not a Bink movie: no bytes");
    if (len >= 4 && !is_magic(data)) return fail(NULL, err, "not a Bink movie");
    if (!is_bink(data, len)) return fail(NULL, err, "truncated: shorter than the 44-byte header");
    struct bk_movie *m = calloc(1, sizeof *m);
    if (!m) return fail(NULL, err, "out of memory");
    struct bk_info *in = &m->info;
    uint64_t bytes = (uint64_t)rd32(data + 4) + 8;
    in->frames = rd32(data + 8), in->width = rd32(data + 20), in->height = rd32(data + 24);
    in->rate = rd32(data + 28), in->rate_div = rd32(data + 32), in->tracks = rd32(data + 40);
    if (bytes > len) return fail(m, err, "truncated: the header declares more bytes than there are");
    in->bytes = (uint32_t)bytes;
    if (!in->frames) return fail(m, err, "no frames");
    if (!in->width || !in->height) return fail(m, err, "zero width or height");
    m->data = data;
    m->cur = 1;

    const int io_size = 65536;
    uint8_t *iobuf = av_malloc(io_size);
    if (!iobuf) return fail(m, err, "out of memory");
    m->io = avio_alloc_context(iobuf, io_size, 0, m, io_read, NULL, io_seek);
    if (!m->io) { av_free(iobuf); return fail(m, err, "out of memory"); }
    m->fmt = avformat_alloc_context();
    if (!m->fmt) return fail(m, err, "out of memory");
    m->fmt->pb = m->io;
    const AVInputFormat *bik = av_find_input_format("bink");
    if (!bik) { avformat_free_context(m->fmt); m->fmt = NULL; return fail(m, err, "FFmpeg has no Bink demuxer"); }
    if (avformat_open_input(&m->fmt, NULL, bik, NULL) < 0)   /* frees fmt on failure */
        return fail(m, err, "FFmpeg's Bink demuxer cannot read the movie");
    m->vs = -1;
    for (unsigned i = 0; i < m->fmt->nb_streams; i++) {
        AVStream *st = m->fmt->streams[i];
        if (st->codecpar->codec_type == AVMEDIA_TYPE_VIDEO && m->vs < 0) m->vs = (int)i;
        else st->discard = AVDISCARD_ALL;                    /* audio tracks: never opened */
    }
    if (m->vs < 0) return fail(m, err, "no video stream in the movie");
    AVStream *v = m->fmt->streams[m->vs];
    const AVCodec *vc = avcodec_find_decoder(v->codecpar->codec_id);
    if (!vc) return fail(m, err, "FFmpeg has no Bink video decoder");
    m->vdec = avcodec_alloc_context3(vc);
    if (!m->vdec || avcodec_parameters_to_context(m->vdec, v->codecpar) < 0)
        return fail(m, err, "out of memory");
    m->vdec->thread_count = 1;
    if (avcodec_open2(m->vdec, vc, NULL) < 0) return fail(m, err, "FFmpeg cannot open the Bink video decoder");
    if ((uint32_t)m->vdec->width != in->width || (uint32_t)m->vdec->height != in->height)
        return fail(m, err, "the decoder's size differs from the header's");
    m->frame = av_frame_alloc(), m->pkt = av_packet_alloc();
    if (!m->frame || !m->pkt) return fail(m, err, "out of memory");
    return m;
}

const struct bk_info *bk_info_of(const struct bk_movie *m) { return &m->info; }
uint32_t bk_current(const struct bk_movie *m) { return m->cur; }
uint32_t bk_decoded(const struct bk_movie *m) { return m->decoded; }

void bk_goto(struct bk_movie *m, uint32_t frame) {
    if (frame < 1 || frame > m->info.frames) {
        fprintf(stderr, "[bw2] FATAL: bink: goto frame %u outside 1..%u\n", frame, m->info.frames);
        fflush(stderr);
        abort();
    }
    m->cur = frame;
}

/* The next frame of the stream into m->frame; 0 on a decoder error or the end. */
static int decode_next(struct bk_movie *m) {
    for (;;) {
        int r = avcodec_receive_frame(m->vdec, m->frame);
        if (r == 0) { m->read++, m->decoded++; return 1; }
        if (r != AVERROR(EAGAIN)) return 0;
        r = av_read_frame(m->fmt, m->pkt);
        if (r < 0) {
            if (avcodec_send_packet(m->vdec, NULL) < 0) return 0;   /* drain; a second EOF ends it */
            continue;
        }
        if (m->pkt->stream_index == m->vs) r = avcodec_send_packet(m->vdec, m->pkt);
        av_packet_unref(m->pkt);
        if (r < 0) return 0;
    }
}

int bk_frame(struct bk_movie *m) {
    if (m->have == m->cur) return 1;
    m->have = 0;
    if (m->cur <= m->read) {                                  /* earlier: back to the first frame */
        if (av_seek_frame(m->fmt, m->vs, 0, AVSEEK_FLAG_BACKWARD) < 0) return 0;
        avcodec_flush_buffers(m->vdec);
        m->read = 0;
    }
    while (m->read < m->cur)                                  /* frames in between are dropped */
        if (!decode_next(m)) return 0;
    m->have = m->cur;
    return 1;
}

static uint8_t clamp8(int v) { return (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v); }

int bk_copy(const struct bk_movie *m, uint8_t *dst, uint32_t pitch, uint32_t dst_h,
            uint32_t x, uint32_t y, uint32_t type) {
    if (type != 3 && type != 5) return -1;
    if (!m->have) return -2;
    const AVFrame *f = m->frame;
    int alpha = f->format == AV_PIX_FMT_YUVA420P;
    if (f->format != AV_PIX_FMT_YUV420P && !alpha) {
        fprintf(stderr, "[bw2] FATAL: bink: decoded pixel format %d is not YUV 4:2:0\n", f->format);
        fflush(stderr);
        abort();
    }
    /* BT.601, 16.16 fixed point: limited range (16..235, 16..240) unless the decoder says full */
    int full = f->color_range == AVCOL_RANGE_JPEG ||
               (f->color_range == AVCOL_RANGE_UNSPECIFIED && m->vdec->color_range == AVCOL_RANGE_JPEG);
    int ky = full ? 65536 : 76309, y0 = full ? 0 : 16;
    int krv = full ? 91881 : 104597, kgu = full ? 22554 : 25675, kgv = full ? 46802 : 53279,
        kbu = full ? 116130 : 132201;
    uint64_t w = (uint64_t)f->width, h = (uint64_t)f->height, cols = pitch / 4;
    uint64_t r_end = (uint64_t)y + h < dst_h ? (uint64_t)y + h : dst_h;
    uint64_t c_end = (uint64_t)x + w < cols ? (uint64_t)x + w : cols;
    for (uint64_t r = y; r < r_end; r++) {
        uint64_t sy = r - y;
        const uint8_t *Y = f->data[0] + sy * f->linesize[0], *U = f->data[1] + (sy / 2) * f->linesize[1],
                      *V = f->data[2] + (sy / 2) * f->linesize[2], *A = alpha ? f->data[3] + sy * f->linesize[3] : NULL;
        uint8_t *row = dst + r * pitch;
        for (uint64_t c = x; c < c_end; c++) {
            uint64_t sx = c - x;
            int l = (Y[sx] - y0) * ky, u = U[sx / 2] - 128, vv = V[sx / 2] - 128;
            uint8_t *p = row + c * 4;
            p[0] = clamp8((l + kbu * u + 32768) >> 16);
            p[1] = clamp8((l - kgu * u - kgv * vv + 32768) >> 16);
            p[2] = clamp8((l + krv * vv + 32768) >> 16);
            p[3] = type == 5 && A ? A[sx] : 0xFF;
        }
    }
    return 0;
}
