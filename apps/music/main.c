// music - Music: plays a song. Files opens MP3, MP2 and WAV files with it.
//
//   Space  play/pause      Left/Right  10 s back/forward      Q or Esc  close
//
// MP3 and MP2 are decoded by minimp3 (third_party/minimp3, CC0), WAV (PCM,
// 8/16/24/32-bit) here. The sound goes out as the system's 48 kHz stereo
// stream, resampled on the way. Built from views (view.h); decoding runs from
// v_run's tick, a little ahead of what is heard.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <os.h>
#include <view.h>

#define MINIMP3_IMPLEMENTATION
#define MINIMP3_NO_SIMD
#include "minimp3.h"

#define OUT_RATE  48000
#define AHEAD     (OUT_RATE / 5)          /* keep ~0.2 s queued */
#define SEEK_STEP 10

static unsigned char *data;
static long size;
static int is_wav;

/* MP3: where each frame starts, and the samples before it. */
static mp3dec_t mp3;
static int *frame_at, n_frames, frame_samples, next_frame;

/* WAV: the samples. */
static const unsigned char *wav;
static long wav_frames, wav_next;
static int wav_bits, wav_float;

static int channels, rate, kbps;
static long total;                        /* source frames in the song */

/* The source, as 16-bit stereo, and the resampler's place in it. */
static short src[MINIMP3_MAX_SAMPLES_PER_FRAME * 2];
static int src_n, src_i;
static unsigned phase, step;              /* 16.16: between src_i-1 and src_i */
static short prev_l, prev_r;
static long src_done;                     /* source frames consumed (for the time) */

static int sound_on, paused = 1, ended;
static long written, base;                /* output frames since the last seek; source frame there */
static unsigned long clock0;              /* no sound device: the time plays by the clock */

static view_t *slider, *elapsed_lbl, *left_lbl, *play_btn;

/* ---- Reading the file ------------------------------------------------------ */

static unsigned rd16(const unsigned char *p) { return p[0] | p[1] << 8; }
static unsigned rd32(const unsigned char *p) { return p[0] | p[1] << 8 | p[2] << 16 | (unsigned)p[3] << 24; }

static long load(const char *path) {
    int fd = open(path, OPEN_READ);
    if (fd < 0) return fd;
    long cap = 1 << 20, len = 0;
    data = malloc(cap);
    while (data) {
        if (len == cap) {
            unsigned char *bigger = realloc(data, cap * 2);
            if (!bigger) { free(data); data = 0; break; }
            data = bigger;
            cap *= 2;
        }
        long n = read(fd, data + len, cap - len);
        if (n <= 0) break;
        len += n;
    }
    close(fd);
    return data ? len : SYSERR_NOMEM;
}

static char title[96], artist[96];

/* An ID3v2 text frame's value (Latin-1, UTF-8, or UTF-16: its low bytes). */
static void id3_text(const unsigned char *p, int n, char *out, int max) {
    if (n < 1) return;
    int enc = p[0], k = 0;
    p++, n--;
    if (enc == 1 || enc == 2) {
        int lo = 0;                       /* UTF-16: which byte of a pair is the character */
        if (n >= 2 && p[0] == 0xFF && p[1] == 0xFE) p += 2, n -= 2;
        else if (n >= 2 && p[0] == 0xFE && p[1] == 0xFF) p += 2, n -= 2, lo = 1;
        else if (enc == 2) lo = 1;
        for (int i = lo; i < n && k < max - 1; i += 2)
            if (p[i] && (p[i] < 0x80)) out[k++] = (char)p[i];
            else if (!p[i] && !p[i ^ 1]) break;
    } else {
        for (int i = 0; i < n && p[i] && k < max - 1; i++) out[k++] = (char)p[i];
    }
    out[k] = 0;
}

/* -> where the audio starts (after an ID3v2 tag, whose title it reads). */
static long skip_id3(void) {
    if (size < 10 || memcmp(data, "ID3", 3)) return 0;
    long len = (data[6] & 0x7F) << 21 | (data[7] & 0x7F) << 14 | (data[8] & 0x7F) << 7 | (data[9] & 0x7F);
    long end = 10 + len + (data[5] & 0x10 ? 10 : 0), p = 10;
    if (data[3] >= 3)
        while (p + 10 <= end && p + 10 <= size && data[p]) {
            long n = data[3] == 4 ? (data[p + 4] & 0x7F) << 21 | (data[p + 5] & 0x7F) << 14 | (data[p + 6] & 0x7F) << 7 | (data[p + 7] & 0x7F)
                                  : (long)data[p + 4] << 24 | data[p + 5] << 16 | data[p + 6] << 8 | data[p + 7];
            if (n <= 0 || p + 10 + n > size) break;
            if (!memcmp(data + p, "TIT2", 4)) id3_text(data + p + 10, (int)n, title, sizeof(title));
            if (!memcmp(data + p, "TPE1", 4)) id3_text(data + p + 10, (int)n, artist, sizeof(artist));
            p += 10 + n;
        }
    return end < size ? end : size;
}

static const char *open_wav(void) {
    if (size < 12 || memcmp(data, "RIFF", 4) || memcmp(data + 8, "WAVE", 4)) return 0;
    is_wav = 1;
    long p = 12;
    int format = 0;
    while (p + 8 <= size) {
        long n = rd32(data + p + 4);
        if (!memcmp(data + p, "fmt ", 4) && n >= 16) {
            format = rd16(data + p + 8);
            channels = rd16(data + p + 10);
            rate = (int)rd32(data + p + 12);
            wav_bits = rd16(data + p + 22);
            if (format == 0xFFFE && n >= 26) format = rd16(data + p + 32);   /* WAVE_FORMAT_EXTENSIBLE */
        } else if (!memcmp(data + p, "data", 4)) {
            if (!format) return "the WAV file has no format chunk";
            if (n > size - p - 8) n = size - p - 8;
            wav = data + p + 8;
            wav_float = format == 3;
            if ((format != 1 && format != 3) || (wav_float && wav_bits != 32))
                return "only PCM WAV files can be played (not compressed ones)";
            if (wav_bits != 8 && wav_bits != 16 && wav_bits != 24 && wav_bits != 32) return "unusual WAV sample size";
            if (channels < 1 || channels > 8 || rate < 1000 || rate > 384000) return "unusual WAV format";
            wav_frames = n / (channels * wav_bits / 8);
            total = wav_frames;
            kbps = rate * channels * wav_bits / 1000;
            return "";
        }
        p += 8 + n + (n & 1);
    }
    return "the WAV file has no sound data";
}

static const char *open_mp3(void) {
    long p = skip_id3();
    int cap = 4096;
    frame_at = malloc(cap * sizeof(int));
    mp3dec_init(&mp3);
    mp3dec_frame_info_t info;
    long kb = 0;
    while (p < size && frame_at) {        /* every frame (headers only: fast) */
        int n = mp3dec_decode_frame(&mp3, data + p, (int)(size - p > 1 << 20 ? 1 << 20 : size - p), 0, &info);
        if (!info.frame_bytes) break;
        if (n) {
            if (n_frames == cap) {
                int *bigger = realloc(frame_at, (cap *= 2) * sizeof(int));
                if (!bigger) break;
                frame_at = bigger;
            }
            frame_at[n_frames++] = (int)(p + info.frame_offset);
            if (!rate) {
                rate = info.hz;
                channels = info.channels;
                frame_samples = n;
            }
            kb += info.bitrate_kbps;
        }
        p += info.frame_bytes;
    }
    if (!n_frames) return "not a song this OS can play (MP3, MP2 or WAV)";
    total = (long)n_frames * frame_samples;
    kbps = (int)(kb / n_frames);
    mp3dec_init(&mp3);
    return "";
}

/* ---- Decoding -------------------------------------------------------------- */

static int wav_sample(const unsigned char *p) {
    switch (wav_bits) {
    case 8: return (p[0] - 128) << 8;
    case 16: return (short)rd16(p);
    case 24: return (short)(p[1] | p[2] << 8);
    default:
        if (wav_float) {
            float f;
            memcpy(&f, p, 4);
            int s = (int)(f * 32767.0f);
            return s > 32767 ? 32767 : s < -32768 ? -32768 : s;
        }
        return (short)rd16(p + 2);
    }
}

/* The next block of source frames into `src`. -> 0 at the end. */
static int refill(void) {
    src_i = src_n = 0;
    if (is_wav) {
        int bytes = wav_bits / 8, frame = channels * bytes;
        while (src_n < MINIMP3_MAX_SAMPLES_PER_FRAME && wav_next < wav_frames) {
            const unsigned char *p = wav + wav_next++ * frame;
            src[2 * src_n] = (short)wav_sample(p);
            src[2 * src_n + 1] = (short)wav_sample(channels > 1 ? p + bytes : p);
            src_n++;
        }
    } else {
        short pcm[MINIMP3_MAX_SAMPLES_PER_FRAME];
        mp3dec_frame_info_t info;
        while (!src_n && next_frame < n_frames) {
            long p = frame_at[next_frame++];
            int n = mp3dec_decode_frame(&mp3, data + p, (int)(size - p > 16384 ? 16384 : size - p), pcm, &info);
            for (int i = 0; i < n; i++) {
                src[2 * i] = pcm[i * info.channels];
                src[2 * i + 1] = pcm[i * info.channels + (info.channels > 1)];
            }
            src_n = n;
            if (!n) src_done += frame_samples;   /* a frame that did not decode: its time still passes */
        }
    }
    return src_n > 0;
}

static long queued(void) {
    if (sound_on) return sound_queued();
    long heard = (long)(uptime_ms() - clock0) * (OUT_RATE / 1000);
    return written > heard ? written - heard : 0;
}

/* Decode and send until ~0.2 s is waiting to be heard. */
static void feed(void) {
    static short out[2048 * 2];
    while (!paused && !ended && queued() < AHEAD) {
        int n = 0;
        while (n < 2048) {
            if (src_i >= src_n && !refill()) {
                ended = 1;
                break;
            }
            short l = src[2 * src_i], r = src[2 * src_i + 1];
            while (phase < 0x10000 && n < 2048) {   /* between the previous sample and this one */
                out[2 * n] = (short)(prev_l + ((l - prev_l) * (int)phase >> 16));
                out[2 * n + 1] = (short)(prev_r + ((r - prev_r) * (int)phase >> 16));
                n++;
                phase += step;
            }
            if (phase >= 0x10000) {
                phase -= 0x10000;
                prev_l = l;
                prev_r = r;
                src_i++;
                src_done++;
            }
        }
        if (n && sound_on) sound_write(out, n);
        written += n;
    }
}

/* ---- Time and seeking ------------------------------------------------------ */

static long now_frame(void) {             /* the source frame being heard */
    long t = base + (long)((written - queued()) * (long long)rate / OUT_RATE);
    return t < 0 ? 0 : t > total ? total : t;
}

static void seek(long frame) {
    if (frame < 0) frame = 0;
    if (frame >= total) frame = total - 1;
    if (is_wav) {
        wav_next = frame;
    } else {
        next_frame = (int)(frame / frame_samples);
        frame = (long)next_frame * frame_samples;
        if (next_frame > 0) next_frame--;    /* one frame before: the decoder needs it (bit reservoir) */
        mp3dec_init(&mp3);
        if (next_frame) {
            short pcm[MINIMP3_MAX_SAMPLES_PER_FRAME];
            mp3dec_frame_info_t info;
            long p = frame_at[next_frame++];
            mp3dec_decode_frame(&mp3, data + p, (int)(size - p > 16384 ? 16384 : size - p), pcm, &info);
        }
    }
    src_i = src_n = 0;
    base = src_done = frame;
    written = 0;
    clock0 = uptime_ms();
    ended = 0;
}

static void fmt(char *out, int max, long frames, const char *sign) {
    long s = rate ? frames / rate : 0;
    snprintf(out, max, "%s%ld:%02ld", sign, s / 60, s % 60);
}

static void show_time(void) {
    long f = now_frame();
    char t[24];
    fmt(t, sizeof(t), f, "");
    v_set_text(elapsed_lbl, t);
    fmt(t, sizeof(t), total - f, "-");
    v_set_text(left_lbl, t);
    v_set_value(slider, total ? (int)(f * 100 / total) : 0);
}

static void play_pause(view_t *b) {
    (void)b;
    if (ended || now_frame() >= total) seek(0);
    paused = !paused;
    if (!paused) seek(now_frame());       /* from where it is heard (drops what was queued ahead) */
    v_set_text(play_btn, paused ? "Play" : "Pause");
    feed();
}

static void back(view_t *b) { (void)b; seek(now_frame() - (long)SEEK_STEP * rate); show_time(); }
static void forward(view_t *b) { (void)b; seek(now_frame() + (long)SEEK_STEP * rate); show_time(); }

static void dragged(view_t *s) {
    seek(total * s->value / 100);
    show_time();
}

static int keys(view_t *v, int k) {
    (void)v;
    if (k == ' ') play_pause(0);
    else if (k == KEYCODE_LEFT) back(0);
    else if (k == KEYCODE_RIGHT) forward(0);
    else if (k == 'q' || k == 'Q' || k == 27) v_quit(0);
    else return 0;
    return 1;
}

static void tick(void) {
    feed();
    if (ended && !paused && queued() == 0) {   /* the end was heard: back to the start */
        paused = 1;
        seek(0);
        v_set_text(play_btn, "Play");
    }
    show_time();
}

int main(int argc, char **argv) {
    if (argc < 2) {
        v_alert("music", "Music", "Open a song (MP3, MP2 or WAV) from Files.");
        return 2;
    }
    const char *name = strrchr(argv[1], '/') ? strrchr(argv[1], '/') + 1 : argv[1];
    char head[128];
    snprintf(head, sizeof(head), "Can't play %s", name);
    size = load(argv[1]);
    if (size < 0) {
        v_alert("music", head, os_strerror((int)size));
        return 1;
    }
    const char *why = open_wav();
    if (!why) why = open_mp3();
    if (*why) {
        v_alert("music", head, why);
        return 1;
    }
    step = (unsigned)(((unsigned long long)rate << 16) / OUT_RATE);
    if (!title[0]) {
        snprintf(title, sizeof(title), "%s", name);
        char *dot = strrchr(title, '.');
        if (dot && dot != title) *dot = 0;
    }

    int r = sound_open();
    sound_on = r == 0;
    char info[128];
    snprintf(info, sizeof(info), "%s  %d.%d kHz  %s  %d kbps%s", is_wav ? "WAV" : "MP3", rate / 1000, rate % 1000 / 100,
             channels == 1 ? "mono" : "stereo", kbps,
             sound_on ? "" : r == SYSERR_INUSE ? "  (sound in use)" : "  (no sound device)");
    printf("music: %s: %s, %ld s\n", name, info, rate ? total / rate : 0);

    int S = ui_scale();
    ui_window win;
    if (ui_open(&win, name, 460 * S, 210 * S) < 0) {
        printf("music: needs the desktop\n");
        return 1;
    }
    view_t *root = v_box(0, V_COLUMN);
    root->align = V_STRETCH;
    root->on_key = keys;
    view_t *top = v_box(root, V_ROW);
    top->pad = 0;
    top->gap = 14;
    v_icon(top, "music", 64);
    view_t *col = v_box(top, V_COLUMN);
    col->pad = 0;
    col->gap = 2;
    col->flex = 1;
    v_label(col, title)->style = UI_BOLD;
    if (artist[0]) v_label(col, artist);
    v_label(col, info)->color = UI_C_SECONDARY;
    slider = v_slider(root, 0, dragged);
    view_t *times = v_box(root, V_ROW);
    times->pad = 0;
    elapsed_lbl = v_label(times, "0:00");
    elapsed_lbl->color = UI_C_SECONDARY;
    v_spacer(times);
    left_lbl = v_label(times, "");
    left_lbl->color = UI_C_SECONDARY;
    view_t *buttons = v_box(root, V_ROW);
    buttons->pad = 0;
    v_spacer(buttons);
    v_button(buttons, "Back 10 s", back);
    play_btn = v_button(buttons, "Play", play_pause);
    play_btn->flags = UI_BTN_DEFAULT;
    v_button(buttons, "Forward 10 s", forward);
    v_spacer(buttons);
    seek(0);
    show_time();
    play_pause(0);                        /* it starts playing */
    r = v_run(&win, root, 40, tick);
    if (sound_on) sound_close();
    return r;
}
