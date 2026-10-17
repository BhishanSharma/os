// ff.c - the video player's second decoder: FFmpeg (libavformat,
// libavcodec, libswresample; LGPL 2.1, built by scripts/build-ffmpeg.sh).
// It plays what pl_mpeg does not: MP4, MOV, MKV, WebM, AVI with H.264,
// HEVC, MPEG-4, VP8 or VP9 video and AAC, MP3, Opus, Vorbis, FLAC... sound.
//
// The same model as pl_mpeg: ff_decode(elapsed) moves the clock on and
// decodes what is due; the newest due picture waits in `shown` until
// ff_picture() scales it into the window; the sound is decoded a little
// ahead and written to the 48 kHz stream as it is decoded.
#include <stdio.h>
#include <string.h>
#include <os.h>
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswresample/swresample.h>
#include <libavutil/pixdesc.h>
#include "ff.h"

#define LEAD  0.30                        /* sound decoded ahead of the picture (s) */
#define RING  12                          /* decoded pictures waiting for their time */

int ff_w, ff_h, ff_rate;
double ff_duration;

static int fd;
static long file_size;
static AVFormatContext *fmt;
static AVCodecContext *vctx, *actx;
static int vstream = -1, astream = -1, sound;
static SwrContext *swr;
static AVPacket *pkt;
static AVFrame *dec, *ring[RING], *shown;
static int ring_n, has_shown;
static double clock_t0, vtime, atime, skip_until = -1;
static int demux_done, vdone, adone, flushed_v, flushed_a;
static double start;                      /* the file's first time stamp (s) */

/* ---- Reading the file -------------------------------------------------------------- */

static int read_cb(void *opaque, uint8_t *buf, int size) {
    (void)opaque;
    long n = read(fd, buf, (size_t)size);
    return n > 0 ? (int)n : AVERROR_EOF;
}

static int64_t seek_cb(void *opaque, int64_t offset, int whence) {
    (void)opaque;
    if (whence & AVSEEK_SIZE) return file_size;
    long r = lseek(fd, (long)offset, whence & ~AVSEEK_FORCE);
    return r < 0 ? AVERROR(EIO) : r;
}

static AVCodecContext *open_codec(int index) {
    AVStream *st = fmt->streams[index];
    const AVCodec *codec = avcodec_find_decoder(st->codecpar->codec_id);
    if (!codec) return 0;
    AVCodecContext *c = avcodec_alloc_context3(codec);
    if (!c || avcodec_parameters_to_context(c, st->codecpar) < 0) return 0;
    c->pkt_timebase = st->time_base;
    if (avcodec_open2(c, codec, 0) < 0) return 0;
    return c;
}

int ff_open(const char *path, const char **why) {
    static char reason[96];
    av_log_set_level(AV_LOG_ERROR);
    fd = open(path, OPEN_READ);
    if (fd < 0) { *why = os_strerror(fd); return -1; }
    file_size = lseek(fd, 0, SEEK_END);
    lseek(fd, 0, SEEK_SET);
    unsigned char *iobuf = av_malloc(64 * 1024);
    AVIOContext *io = avio_alloc_context(iobuf, 64 * 1024, 0, 0, read_cb, 0, seek_cb);
    fmt = avformat_alloc_context();
    if (!io || !fmt) { *why = "out of memory"; return -1; }
    fmt->pb = io;
    fmt->flags |= AVFMT_FLAG_CUSTOM_IO;
    if (avformat_open_input(&fmt, path, 0, 0) < 0) { *why = "not a video this OS can read"; return -1; }
    if (avformat_find_stream_info(fmt, 0) < 0) { *why = "the video's streams cannot be read"; return -1; }
    vstream = av_find_best_stream(fmt, AVMEDIA_TYPE_VIDEO, -1, -1, 0, 0);
    if (vstream < 0) { *why = "it has no video"; return -1; }
    vctx = open_codec(vstream);
    if (!vctx) {
        snprintf(reason, sizeof(reason), "its video (%s) cannot be decoded here",
                 avcodec_get_name(fmt->streams[vstream]->codecpar->codec_id));
        *why = reason;
        return -1;
    }
    ff_w = vctx->width;
    ff_h = vctx->height;
    if (ff_w <= 0 || ff_h <= 0) { *why = "the video has no size"; return -1; }
    astream = av_find_best_stream(fmt, AVMEDIA_TYPE_AUDIO, -1, vstream, 0, 0);
    if (astream >= 0 && !(actx = open_codec(astream))) astream = -1;   /* plays without sound */
    ff_rate = actx ? actx->sample_rate : 0;
    ff_duration = fmt->duration > 0 ? fmt->duration / (double)AV_TIME_BASE : 0;
    start = fmt->start_time != AV_NOPTS_VALUE ? fmt->start_time / (double)AV_TIME_BASE : 0;
    for (unsigned i = 0; i < fmt->nb_streams; i++)          /* read only what is played */
        if ((int)i != vstream && (int)i != astream) fmt->streams[i]->discard = AVDISCARD_ALL;
    pkt = av_packet_alloc();
    dec = av_frame_alloc();
    shown = av_frame_alloc();
    for (int i = 0; i < RING; i++) ring[i] = av_frame_alloc();
    if (!pkt || !dec || !shown || !ring[RING - 1]) { *why = "out of memory"; return -1; }
    return 0;
}

const char *ff_codecs(void) {
    static char s[64];
    snprintf(s, sizeof(s), "%s%s%s", avcodec_get_name(vctx->codec_id), actx ? " + " : "",
             actx ? avcodec_get_name(actx->codec_id) : "");
    return s;
}

int ff_has_audio(void) { return actx != 0; }

/* Turn the sound on: decoded sound is resampled to 48 kHz stereo 16-bit. */
int ff_sound(void) {
    if (!actx) return -1;
    AVChannelLayout stereo = AV_CHANNEL_LAYOUT_STEREO;
    if (swr_alloc_set_opts2(&swr, &stereo, AV_SAMPLE_FMT_S16, 48000, &actx->ch_layout, actx->sample_fmt,
                            actx->sample_rate, 0, 0) < 0 || swr_init(swr) < 0)
        return -1;
    sound = 1;
    return 0;
}

/* ---- Decoding ---------------------------------------------------------------------- */

static double stamp(const AVFrame *f, int stream) {
    int64_t ts = f->best_effort_timestamp != AV_NOPTS_VALUE ? f->best_effort_timestamp : f->pts;
    if (ts == AV_NOPTS_VALUE) return -1;
    return ts * av_q2d(fmt->streams[stream]->time_base) - start;
}

static void play_audio(AVFrame *f) {
    double t = stamp(f, astream);
    if (t >= 0) atime = t + (double)f->nb_samples / actx->sample_rate;
    if (!sound || (skip_until >= 0 && t >= 0 && atime < skip_until)) return;
    static short out[2 * 8192];
    int room = 8192, n = swr_convert(swr, (uint8_t *[]){ (uint8_t *)out }, room,
                                     (const uint8_t **)f->extended_data, f->nb_samples);
    while (n > 0) {
        sound_write(out, n);                  /* waits while the stream's buffer is full */
        n = swr_convert(swr, (uint8_t *[]){ (uint8_t *)out }, room, 0, 0);
    }
}

/* Take the frames a decoder has ready: pictures into the ring, sound out. */
static void drain(AVCodecContext *c) {
    for (;;) {
        if (c == vctx && ring_n == RING) return;      /* full: decode more when there is room */
        int r = avcodec_receive_frame(c, dec);
        if (r == AVERROR_EOF) { if (c == vctx) vdone = 1; else adone = 1; return; }
        if (r < 0) return;
        if (c == vctx) {
            double t = stamp(dec, vstream);
            if (skip_until >= 0 && t >= 0 && t < skip_until - 0.05) { av_frame_unref(dec); continue; }
            av_frame_move_ref(ring[ring_n++], dec);
        } else {
            play_audio(dec);
            av_frame_unref(dec);
        }
    }
}

/* Video packets wait here while the ring is full (the sound must still be
 * read on ahead of them). */
#define QUEUE 1024
static AVPacket *vq[QUEUE];
static int vq_head, vq_n;

/* The waiting video packets into the decoder, as far as the ring has room. */
static void pump_video(void) {
    while (ring_n < RING) {
        if (!vq_n) {
            if (demux_done && !flushed_v) { avcodec_send_packet(vctx, 0); flushed_v = 1; }
            drain(vctx);
            return;
        }
        AVPacket *p = vq[vq_head];
        int r = avcodec_send_packet(vctx, p);
        if (r == AVERROR(EAGAIN)) {               /* its output must be taken first */
            int before = ring_n;
            drain(vctx);
            if (ring_n == before && ring_n == RING) return;
            if (ring_n == before) { av_packet_unref(p); vq_head = (vq_head + 1) % QUEUE; vq_n--; }   /* stuck: skip it */
            continue;
        }
        av_packet_unref(p);
        vq_head = (vq_head + 1) % QUEUE;
        vq_n--;
        drain(vctx);
    }
}

/* One more packet from the file: video into the queue, sound decoded now.
 * -> 0 when the file is over. */
static int feed(void) {
    if (demux_done) {
        if (actx && !flushed_a) { avcodec_send_packet(actx, 0); flushed_a = 1; drain(actx); }
        return 0;
    }
    if (vq_n == QUEUE) return 1;                  /* (the picture is far behind: let it catch up) */
    if (av_read_frame(fmt, pkt) < 0) {
        demux_done = 1;
        return 1;
    }
    if (pkt->stream_index == vstream) {
        int i = (vq_head + vq_n) % QUEUE;
        if (!vq[i]) vq[i] = av_packet_alloc();
        if (vq[i]) {
            av_packet_move_ref(vq[i], pkt);
            vq_n++;
        }
    } else if (pkt->stream_index == astream) {
        while (avcodec_send_packet(actx, pkt) == AVERROR(EAGAIN)) drain(actx);
        drain(actx);
    }
    av_packet_unref(pkt);
    return 1;
}

static double ring_time(int i) {
    double t = stamp(ring[i], vstream);
    return t < 0 ? vtime : t;
}

static void ring_pop(void) {
    av_frame_unref(shown);
    av_frame_move_ref(shown, ring[0]);
    AVFrame *first = ring[0];
    memmove(ring, ring + 1, (RING - 1) * sizeof(ring[0]));
    ring[RING - 1] = first;
    ring_n--;
}

int ff_decode(double elapsed) {
    clock_t0 += elapsed;
    double now = clock_t0;
    for (int guard = 0; guard < 1000; guard++) {
        pump_video();
        if (ring_n && ring_time(0) <= now) {          /* due: the newest due one is shown */
            vtime = ring_time(0);
            ring_pop();
            has_shown = 1;
            continue;
        }
        int sound_ahead = !actx || adone || !sound || atime >= now + LEAD;
        if ((ring_n || vdone) && sound_ahead) break;  /* the next picture is not due yet */
        if (!feed() && (vdone || !vq_n)) {            /* the file is read: only the queue is left */
            pump_video();
            if (!ring_n) break;
        }
    }
    if (skip_until >= 0 && vtime >= skip_until - 0.05) skip_until = -1;
    return !(vdone && demux_done && !ring_n);
}

int ff_picture_ready(void) { return has_shown; }
double ff_time(void) { return clock_t0; }

void ff_seek(double t) {
    if (t < 0) t = 0;
    int64_t ts = (int64_t)((t + start) * AV_TIME_BASE);
    av_seek_frame(fmt, -1, ts, AVSEEK_FLAG_BACKWARD);
    avcodec_flush_buffers(vctx);
    if (actx) avcodec_flush_buffers(actx);
    for (int i = 0; i < ring_n; i++) av_frame_unref(ring[i]);
    ring_n = 0;
    for (; vq_n; vq_n--, vq_head = (vq_head + 1) % QUEUE) av_packet_unref(vq[vq_head]);
    demux_done = vdone = adone = flushed_v = flushed_a = 0;
    clock_t0 = vtime = atime = t;
    skip_until = t;                           /* decode from the key frame before it, show from t */
}

/* ---- The picture: YUV to RGB, scaled to dw x dh ------------------------------------ */

/* Fixed point (x 256) colour matrices: BT.601 for SD, BT.709 for HD. */
void ff_picture(unsigned int *dst, int dw, int dh, const int *xmap) {
    AVFrame *f = shown;
    has_shown = 0;
    const AVPixFmtDescriptor *d = av_pix_fmt_desc_get(f->format);
    if (!d || !(d->flags & AV_PIX_FMT_FLAG_PLANAR) || (d->flags & AV_PIX_FMT_FLAG_RGB) || d->nb_components < 3)
        return;                               /* (not a planar YUV picture: decoders here give those) */
    int shift = d->comp[0].depth - 8, wide = d->comp[0].depth > 8;
    int cw = d->log2_chroma_w, ch = d->log2_chroma_h;
    int full = f->color_range == AVCOL_RANGE_JPEG || f->format == AV_PIX_FMT_YUVJ420P ||
               f->format == AV_PIX_FMT_YUVJ422P || f->format == AV_PIX_FMT_YUVJ444P;
    int hd = f->colorspace == AVCOL_SPC_BT709 || (f->colorspace == AVCOL_SPC_UNSPECIFIED && f->height >= 720);
    int ky = full ? 256 : 298, rv, gu, gv, bu;
    if (hd) { rv = full ? 403 : 459; gu = full ? 48 : 55; gv = full ? 120 : 136; bu = full ? 475 : 541; }
    else { rv = full ? 359 : 409; gu = full ? 88 : 100; gv = full ? 183 : 208; bu = full ? 454 : 516; }
    int y0 = full ? 0 : 16;
    for (int y = 0; y < dh; y++) {
        int sy = (int)((long)y * f->height / dh), cy = sy >> ch;
        const uint8_t *Y = f->data[0] + (long)sy * f->linesize[0];
        const uint8_t *U = f->data[1] + (long)cy * f->linesize[1];
        const uint8_t *V = f->data[2] + (long)cy * f->linesize[2];
        unsigned int *out = dst + (long)y * dw;
        for (int x = 0; x < dw; x++) {
            int sx = xmap[x], cx = sx >> cw, yy, u, v;
            if (wide) {
                yy = ((const uint16_t *)Y)[sx] >> shift;
                u = ((const uint16_t *)U)[cx] >> shift;
                v = ((const uint16_t *)V)[cx] >> shift;
            } else {
                yy = Y[sx], u = U[cx], v = V[cx];
            }
            int c = (yy - y0) * ky, du = u - 128, dv = v - 128;
            int r = (c + rv * dv + 128) >> 8, g = (c - gu * du - gv * dv + 128) >> 8, b = (c + bu * du + 128) >> 8;
            r = r < 0 ? 0 : r > 255 ? 255 : r;
            g = g < 0 ? 0 : g > 255 ? 255 : g;
            b = b < 0 ? 0 : b > 255 ? 255 : b;
            out[x] = (unsigned)r << 16 | (unsigned)g << 8 | (unsigned)b;
        }
    }
}

void ff_close(void) {
    av_frame_free(&shown);
    av_frame_free(&dec);
    for (int i = 0; i < RING; i++) av_frame_free(&ring[i]);
    av_packet_free(&pkt);
    avcodec_free_context(&vctx);
    avcodec_free_context(&actx);
    swr_free(&swr);
    avformat_close_input(&fmt);
    close(fd);
}
