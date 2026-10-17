// ff.c - the video player's second decoder: FFmpeg (libavformat,
// libavcodec, libswresample; LGPL 2.1, built by scripts/build-ffmpeg.sh).
// It plays what pl_mpeg does not: MP4, MOV, MKV, WebM, AVI with H.264,
// HEVC, MPEG-4, VP8 or VP9 video and AAC, MP3, Opus, Vorbis, FLAC... sound.
//
// Two threads. The decoder thread reads the file, decodes, turns pictures
// into RGB at the window's size in a few slots ahead of time, and keeps the
// sound stream fed (never waiting on it). The window's thread only takes the
// picture that is due (ff_next). The clock is the sound being heard, so the
// picture follows the sound; without sound, the time since it started.
#include <stdio.h>
#include <string.h>
#include <os.h>
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswresample/swresample.h>
#include <libavutil/pixdesc.h>
#include <pthread.h>
#include "ff.h"

#define SLOTS     4                       /* RGB pictures ready ahead */
#define QUEUE     1024                    /* video packets read ahead of the decoder (for the sound) */
#define RATE      48000
#define DEVICE    (RATE / 4)              /* keep ~0.25 s in the sound stream (it holds 0.34) */
#define FIFO      (RATE * 3)              /* our own 3 s of sound behind it */
#define AHEAD     (RATE * 3 / 2)          /* decode sound ~1.5 s ahead: slow reads cannot starve it */

int ff_w, ff_h, ff_rate;
double ff_duration;

static int fd;
static long file_size;
static AVFormatContext *fmt;
static AVCodecContext *vctx, *actx;
static int vstream = -1, astream = -1, sound;
static SwrContext *swr;
static AVPacket *pkt, *vq[QUEUE];
static AVFrame *dec, *pending;
static int vq_head, vq_n, has_pending;
static int demux_done, vdone, adone, flushed_v, flushed_a;
static double start, skip_until = -1;

/* The pictures, for the window's thread. */
static int pw, ph;
static const int *pxmap;
static unsigned int *slot_px[SLOTS];
static double slot_t[SLOTS];
static volatile int slot_ready[SLOTS];
static mutex_t lock;

/* The sound: a ring of 16-bit stereo frames waiting for the stream. */
static short fifo[FIFO * 2];
static int fifo_head, fifo_n;
static double fifo_t = -1;                /* the time of the frame at fifo_head (-1: none yet) */
static mutex_t snd_lock;                  /* the fifo: the decoder fills it, the sound thread empties it */

/* What went slowly, for the log line at the end. */
static unsigned long max_read_ms, max_gap_ms, last_shown_ms;
static long underruns;

/* The clock. */
static volatile int paused, quitting, finished;
static volatile double seek_req = -1;
static double wall_t;                     /* without sound: wall_t + (now - wall_ms) */
static unsigned long wall_ms;
static double frozen;                     /* the time when paused */
static int threads_started;

/* ---- Reading the file -------------------------------------------------------------- */

static int read_cb(void *opaque, uint8_t *buf, int size) {
    (void)opaque;
    unsigned long t0 = uptime_ms();
    long n = read(fd, buf, (size_t)size);
    unsigned long dt = uptime_ms() - t0;
    if (dt > max_read_ms) max_read_ms = dt;
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
    if (st->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {   /* decode on several cores */
        int n = cpu_count() - 2;
        c->thread_count = n < 1 ? 1 : n > 8 ? 8 : n;
        c->thread_type = FF_THREAD_FRAME | FF_THREAD_SLICE;
    }
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
    pending = av_frame_alloc();
    if (!pkt || !dec || !pending) { *why = "out of memory"; return -1; }
    return 0;
}

const char *ff_codecs(void) {
    static char s[64];
    snprintf(s, sizeof(s), "%s%s%s", avcodec_get_name(vctx->codec_id), actx ? " + " : "",
             actx ? avcodec_get_name(actx->codec_id) : "");
    return s;
}

int ff_has_audio(void) { return actx != 0; }

int ff_sound(void) {
    if (!actx) return -1;
    AVChannelLayout stereo = AV_CHANNEL_LAYOUT_STEREO;
    if (swr_alloc_set_opts2(&swr, &stereo, AV_SAMPLE_FMT_S16, RATE, &actx->ch_layout, actx->sample_fmt,
                            actx->sample_rate, 0, 0) < 0 || swr_init(swr) < 0)
        return -1;
    sound = 1;
    return 0;
}

/* ---- The clock --------------------------------------------------------------------- */

static double wall_now(void) { return wall_t + (uptime_ms() - wall_ms) / 1000.0; }

static void set_wall(double t) {
    wall_t = t;
    wall_ms = uptime_ms();
}

/* The time being seen and heard. */
static double clock_now(void) {
    if (paused) return frozen;
    if (sound) {
        mutex_lock(&snd_lock);
        double ft = fifo_t;
        int left = fifo_n;
        mutex_unlock(&snd_lock);
        long q = sound_queued();
        if (ft >= 0 && q >= 0 && (q > 0 || left > 0)) {      /* the sound being heard: written minus still queued */
            double t = ft - q / (double)RATE;
            set_wall(t);                  /* (if the sound runs out, the time goes on from here) */
            return t;
        }
    }
    return wall_now();
}

double ff_time(void) {
    double t = clock_now();
    return ff_duration > 0 && t > ff_duration ? ff_duration : t;
}

void ff_pause(int on) {
    if (on && !paused) frozen = clock_now();
    if (!on && paused) set_wall(frozen);
    paused = on;
}

void ff_seek(double t) {
    seek_req = t < 0 ? 0 : t;
    frozen = seek_req;
    set_wall(seek_req);
}

/* ---- The sound --------------------------------------------------------------------- */

static double stamp(const AVFrame *f, int stream) {
    int64_t ts = f->best_effort_timestamp != AV_NOPTS_VALUE ? f->best_effort_timestamp : f->pts;
    if (ts == AV_NOPTS_VALUE) return -1;
    return ts * av_q2d(fmt->streams[stream]->time_base) - start;
}

static void fifo_put(const short *pcm, int n) {
    if (n > FIFO - fifo_n) n = FIFO - fifo_n;                 /* (full: the rest is dropped) */
    for (int i = 0; i < n; i++) {
        int k = (fifo_head + fifo_n + i) % FIFO;
        fifo[2 * k] = pcm[2 * i];
        fifo[2 * k + 1] = pcm[2 * i + 1];
    }
    fifo_n += n;
}

static void play_audio(AVFrame *f) {
    double t = stamp(f, astream);
    if (!sound) return;
    if (skip_until >= 0 && t >= 0 && t + (double)f->nb_samples / actx->sample_rate < skip_until) return;
    static short out[2 * 8192];
    uint8_t *o[1] = { (uint8_t *)out };
    int n = swr_convert(swr, o, 8192, (const uint8_t **)f->extended_data, f->nb_samples);
    if (n <= 0) return;
    mutex_lock(&snd_lock);
    if (fifo_n == 0 && t >= 0) fifo_t = t;                    /* (re)starts: its time */
    else if (fifo_t < 0) fifo_t = 0;
    fifo_put(out, n);
    mutex_unlock(&snd_lock);
}

/* Fill the sound stream up to ~0.25 s; it never waits. */
static void pump_sound(void) {
    if (!sound || paused) return;
    long q = sound_queued();
    if (q < 0) {                                              /* the stream is not ours: go on without sound */
        sound = 0;
        return;
    }
    mutex_lock(&snd_lock);
    static int starving;
    int out = q == 0 && fifo_n == 0 && fifo_t > 0 && !adone;   /* it ran out while playing */
    if (out && !starving) underruns++;
    starving = out;
    long room = DEVICE - q;
    while (room > 0 && fifo_n) {
        int n = fifo_n < room ? fifo_n : (int)room;
        if (n > FIFO - fifo_head) n = FIFO - fifo_head;       /* up to the ring's end */
        long w = sound_write(fifo + 2 * fifo_head, n);
        if (w <= 0) {
            if (w < 0) sound = 0;
            break;
        }
        fifo_head = (fifo_head + (int)w) % FIFO;
        fifo_n -= (int)w;
        fifo_t += w / (double)RATE;
        room -= w;
    }
    mutex_unlock(&snd_lock);
}

/* The sound thread: keeps the stream fed whatever the decoder is doing. */
static void *sound_thread(void *arg) {
    (void)arg;
    while (!quitting) {
        pump_sound();
        sleep_ms(5);
    }
    return 0;
}

/* ---- Decoding ---------------------------------------------------------------------- */

static void drain_audio(void) {
    while (avcodec_receive_frame(actx, dec) == 0) {
        play_audio(dec);
        av_frame_unref(dec);
    }
}

/* One more packet from the file: video into the queue, sound decoded now.
 * -> 0 when the file is over. */
static int feed(void) {
    if (demux_done) {
        if (actx && !flushed_a) {
            avcodec_send_packet(actx, 0);
            flushed_a = 1;
            drain_audio();
            adone = 1;
        }
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
        while (avcodec_send_packet(actx, pkt) == AVERROR(EAGAIN)) drain_audio();
        drain_audio();
    }
    av_packet_unref(pkt);
    return 1;
}

/* The next picture from the video decoder into `pending`. -> 1 got one. */
static int next_picture(void) {
    for (;;) {
        int r = avcodec_receive_frame(vctx, pending);
        if (r == 0) {
            double t = stamp(pending, vstream);
            if (skip_until >= 0 && t >= 0 && t < skip_until - 0.02) { av_frame_unref(pending); continue; }
            skip_until = -1;
            return 1;
        }
        if (r == AVERROR_EOF) { vdone = 1; return 0; }
        if (vq_n) {                               /* it wants more input */
            AVPacket *p = vq[vq_head];
            r = avcodec_send_packet(vctx, p);
            if (r == AVERROR(EAGAIN)) continue;   /* (cannot happen after receive said EAGAIN) */
            av_packet_unref(p);
            vq_head = (vq_head + 1) % QUEUE;
            vq_n--;
            continue;
        }
        if (demux_done) {
            if (!flushed_v) { avcodec_send_packet(vctx, 0); flushed_v = 1; continue; }
            return 0;
        }
        if (!feed()) return 0;                    /* read on until a video packet comes */
    }
}

/* ---- The picture: YUV to RGB at the window's size ---------------------------------- */

/* Fixed point (x 256) colour matrices: BT.601 for SD, BT.709 for HD.
 * Rows y_from..y_to of the picture: several threads each do a band. */
static void to_rgb_rows(const AVFrame *f, unsigned int *dst, int y_from, int y_to) {
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
    for (int y = y_from; y < y_to; y++) {
        int sy = (int)((long)y * f->height / ph), cy = sy >> ch;
        const uint8_t *Y = f->data[0] + (long)sy * f->linesize[0];
        const uint8_t *U = f->data[1] + (long)cy * f->linesize[1];
        const uint8_t *V = f->data[2] + (long)cy * f->linesize[2];
        unsigned int *out = dst + (long)y * pw;
        for (int x = 0; x < pw; x++) {
            int sx = pxmap[x], cx = sx >> cw, yy, u, v;
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

/* The colour conversion on several cores: workers wait for a job number
 * to change, each converts its band, the last one done says so. */
#define MAX_WORKERS 6
static int n_bands = 1, job_seq, job_left, workers_quit;
static const AVFrame *job_frame;
static unsigned int *job_dst;
static pthread_mutex_t job_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t job_go = PTHREAD_COND_INITIALIZER, job_done = PTHREAD_COND_INITIALIZER;

static void band(int i, const AVFrame *f, unsigned int *dst) {
    to_rgb_rows(f, dst, ph * i / n_bands, ph * (i + 1) / n_bands);
}

static void *worker(void *arg) {
    int i = (int)(long)arg, seen = 0;
    for (;;) {
        pthread_mutex_lock(&job_lock);
        while (job_seq == seen && !workers_quit) pthread_cond_wait(&job_go, &job_lock);
        if (workers_quit) {
            pthread_mutex_unlock(&job_lock);
            return 0;
        }
        seen = job_seq;
        const AVFrame *f = job_frame;
        unsigned int *dst = job_dst;
        pthread_mutex_unlock(&job_lock);
        band(i, f, dst);
        pthread_mutex_lock(&job_lock);
        if (--job_left == 0) pthread_cond_signal(&job_done);
        pthread_mutex_unlock(&job_lock);
    }
}

static void to_rgb(const AVFrame *f, unsigned int *dst) {
    if (n_bands == 1) {
        band(0, f, dst);
        return;
    }
    pthread_mutex_lock(&job_lock);
    job_frame = f;
    job_dst = dst;
    job_left = n_bands - 1;
    job_seq++;
    pthread_cond_broadcast(&job_go);
    pthread_mutex_unlock(&job_lock);
    band(0, f, dst);                          /* this thread does the first band */
    pthread_mutex_lock(&job_lock);
    while (job_left > 0) pthread_cond_wait(&job_done, &job_lock);
    pthread_mutex_unlock(&job_lock);
}

static int free_slot(void) {
    for (int i = 0; i < SLOTS; i++)
        if (!slot_ready[i]) return i;
    return -1;
}

/* ---- The decoder thread ------------------------------------------------------------ */

static long decoded, dropped;

static void do_seek(double t) {
    int64_t ts = (int64_t)((t + start) * AV_TIME_BASE);
    av_seek_frame(fmt, -1, ts, AVSEEK_FLAG_BACKWARD);
    avcodec_flush_buffers(vctx);
    if (actx) avcodec_flush_buffers(actx);
    for (; vq_n; vq_n--, vq_head = (vq_head + 1) % QUEUE) av_packet_unref(vq[vq_head]);
    av_frame_unref(pending);
    has_pending = 0;
    mutex_lock(&lock);
    for (int i = 0; i < SLOTS; i++) slot_ready[i] = 0;
    mutex_unlock(&lock);
    mutex_lock(&snd_lock);
    fifo_n = 0;
    fifo_t = -1;
    mutex_unlock(&snd_lock);
    demux_done = vdone = adone = flushed_v = flushed_a = 0;
    finished = 0;
    skip_until = t;                           /* decode from the key frame before it, show from t */
}

static void *decoder(void *arg) {
    (void)arg;
    while (!quitting) {
        double s = seek_req;
        if (s >= 0) {
            seek_req = -1;
            do_seek(s);
            set_wall(s);
        }
        if (paused) { sleep_ms(5); continue; }
        if (!has_pending && !vdone) has_pending = next_picture();
        if (has_pending) {
            double t = stamp(pending, vstream);
            static unsigned long last_made;
            if (t >= 0 && t < clock_now() - 0.1 && uptime_ms() - last_made < 200) {   /* late: skip it (but show 5 a second) */
                av_frame_unref(pending);
                has_pending = 0;
                dropped++;
                continue;
            }
            int k = free_slot();
            if (k >= 0) {
                to_rgb(pending, slot_px[k]);
                slot_t[k] = t;
                av_frame_unref(pending);
                has_pending = 0;
                decoded++;
                last_made = uptime_ms();
                mutex_lock(&lock);
                slot_ready[k] = 1;
                mutex_unlock(&lock);
                continue;
            }
        }
        if (sound && fifo_n < AHEAD && !adone) {           /* keep the sound ahead */
            feed();
            continue;
        }
        if (vdone && !has_pending && (adone || !actx || !sound)) finished = 1;
        sleep_ms(2);                                       /* all ready: wait for the clock */
    }
    quitting = 2;
    return 0;
}

int ff_start(int w, int h, const int *xmap) {
    pw = w;
    ph = h;
    pxmap = xmap;
    for (int i = 0; i < SLOTS; i++)
        if (!(slot_px[i] = malloc((size_t)w * h * 4))) return -1;
    int bands = cpu_count() / 3;              /* (the rest of the cores decode) */
    if (bands > MAX_WORKERS + 1) bands = MAX_WORKERS + 1;
    if ((long)w * h < 640 * 360) bands = 1;   /* small: not worth waking anyone */
    for (int i = 1; i < bands; i++) {
        pthread_t t;
        if (pthread_create(&t, 0, worker, (void *)(long)i) != 0) break;
        n_bands = i + 1;
    }
    set_wall(0);
    pthread_t t;
    if (pthread_create(&t, 0, decoder, 0) != 0) return -1;
    if (sound && pthread_create(&t, 0, sound_thread, 0) != 0) return -1;
    threads_started = 1;
    return 0;
}

/* The newest picture that is due, into dst. -> 1 one, 0 none yet, -1 it is over. */
int ff_next(unsigned int *dst) {
    double now = clock_now();
    int best = -1;
    mutex_lock(&lock);
    for (int i = 0; i < SLOTS; i++)
        if (slot_ready[i] && slot_t[i] <= now && (best < 0 || slot_t[i] > slot_t[best])) best = i;
    for (int i = 0; i < SLOTS; i++)                          /* older due ones: never shown */
        if (best >= 0 && i != best && slot_ready[i] && slot_t[i] < slot_t[best]) slot_ready[i] = 0;
    mutex_unlock(&lock);
    if (best >= 0) {
        unsigned long now_ms = uptime_ms();
        if (last_shown_ms && now_ms - last_shown_ms > max_gap_ms) max_gap_ms = now_ms - last_shown_ms;
        last_shown_ms = now_ms;
        memcpy(dst, slot_px[best], (size_t)pw * ph * 4);
        mutex_lock(&lock);
        slot_ready[best] = 0;
        mutex_unlock(&lock);
        return 1;
    }
    if (finished && !(sound && sound_queued() > 0)) {
        for (int i = 0; i < SLOTS; i++)
            if (slot_ready[i]) return 0;
        return -1;
    }
    return 0;
}

void ff_stats(long *shown_ok, long *late) {
    *shown_ok = decoded;
    *late = dropped;
    printf("video: longest gap between pictures %lu ms, longest read %lu ms, sound ran out %ld times\n",
           max_gap_ms, max_read_ms, underruns);
}

void ff_close(void) {
    if (threads_started) {
        quitting = 1;
        for (int i = 0; i < 500 && quitting != 2; i++) sleep_ms(2);
    }
    pthread_mutex_lock(&job_lock);
    workers_quit = 1;
    pthread_cond_broadcast(&job_go);
    pthread_mutex_unlock(&job_lock);
}
