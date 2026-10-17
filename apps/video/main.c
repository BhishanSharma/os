// video <file> - play a video with its sound
//
// MPEG-1 with MP2 sound (".mpg") is decoded by pl_mpeg (third_party/pl_mpeg,
// MIT); everything else (MP4, MOV, MKV, WebM, AVI: H.264, HEVC, VP9... with
// AAC, MP3, Opus...) by FFmpeg (ff.c). Decoding runs on one core.
//
// The file is read as it plays (read/lseek), the picture is scaled to fit the
// screen, the sound goes to the speakers as a 48 kHz stream. Keys:
//   Space  pause      Left/Right  10 s back/forward      Q or Esc  stop
//
// On the desktop it is built from views (view.h): the picture and the control
// bar are custom views; decoding runs from v_run's tick.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <os.h>
#include <view.h>

#define PLM_NO_STDIO
#define PL_MPEG_IMPLEMENTATION
#include <pl_mpeg.h>
#include "ff.h"

#define OUT_RATE     48000
#define LEAD_TIME    0.30                 /* the sound stream's buffer: ~1/3 s */
#define SEEK_STEP    10.0

static int fd;
static int use_ff;                        /* FFmpeg (ff.c) decodes this one, not pl_mpeg */

/* ---- Reading the file as it plays ----------------------------------------------- */

static void load_cb(plm_buffer_t *self, void *user) {
    plm_buffer_discard_read_bytes(self);
    size_t room = self->capacity - self->length;
    long n = read(fd, self->bytes + self->length, room);
    if (n <= 0) {
        self->has_ended = TRUE;
        return;
    }
    self->length += (size_t)n;
}

static void seek_cb(plm_buffer_t *self, size_t offset, void *user) {
    lseek(fd, (long)offset, SEEK_SET);
}

static size_t tell_cb(plm_buffer_t *self, void *user) {
    return (size_t)lseek(fd, 0, SEEK_CUR);
}

/* ---- The picture ------------------------------------------------------------------- */

static struct os_gfx_info gi;
static int vw, vh;                        /* the video's size */
static int dx, dy, dw, dh;                /* where it goes on the screen */
static unsigned int *frame_rgb, *scaled;
static int *xmap;
static plm_frame_t *latest;               /* the newest decoded frame, not shown yet */
static long frames_shown;
static ui_window win;                     /* on the desktop: the picture goes in here */
static int windowed;
static int ctrl_h, vs = 1, paused, seek_permille = -1;   /* the window's control bar */
static view_t *pic_view, *ctrl_view;
static double shown_t, shown_duration;    /* what the control bar shows */
#define KEY_SEEK 0x200
static void fmt_time(char *buf, double t);

static void on_video(plm_t *plm, plm_frame_t *frame, void *user) {
    latest = frame;                       /* shown after plm_decode: only the newest one */
}

static void show_frame(void) {
    if (use_ff) {
        ff_picture(scaled, dw, dh, xmap);
    } else {
        plm_frame_to_bgra(latest, (uint8_t *)frame_rgb, vw * 4);   /* bytes B,G,R,A = 0xAARRGGBB */
        for (int y = 0; y < dh; y++) {
            const unsigned int *src = frame_rgb + (long)(y * vh / dh) * vw;
            unsigned int *dst = scaled + (long)y * dw;
            for (int x = 0; x < dw; x++) dst[x] = src[xmap[x]] & 0xFFFFFF;
        }
    }
    if (windowed) v_dirty(pic_view);      /* drawn on the next pass */
    else gfx_blit(dx, dy, dw, dh, scaled, dw);
    latest = 0;
    frames_shown++;
}

static void clear_screen_area(void) {
    unsigned int *row = calloc((size_t)gi.width, sizeof(unsigned int));
    if (!row) return;
    for (int y = gi.top; y < gi.height; y++) gfx_blit(0, y, gi.width, 1, row, gi.width);
    free(row);
}

/* A thin bar under the picture (when there is room): how far it has played. */
static void draw_picture(view_t *v, ui_canvas *c) {
    ui_copy(c, v->x, v->y, scaled, dw, dh, dw);
}

static void draw_controls(view_t *v, ui_canvas *c) {
    double t = shown_t, duration = shown_duration;
    int top = v->y, mid = top + ctrl_h / 2, S = vs;
    ui_fill(c, v->x, top, v->w, ctrl_h, 0x1C1C22);
    ui_circle(c, 22 * S, mid, 14 * S, 0x3A3A44);   /* play / pause */
    if (paused) {
        for (int j = -7 * S; j <= 7 * S; j++) {   /* a triangle */
            int len = (7 * S - (j < 0 ? -j : j)) * 12 / 7;
            ui_fill(c, 18 * S, mid + j, len, 1, UI_WHITE);
        }
    } else {
        ui_fill(c, 16 * S, mid - 7 * S, 4 * S, 14 * S, UI_WHITE);
        ui_fill(c, 24 * S, mid - 7 * S, 4 * S, 14 * S, UI_WHITE);
    }
    char a[16], b[16], times[40];
    fmt_time(a, t);
    fmt_time(b, duration);
    snprintf(times, sizeof(times), "%s / %s", a, b);
    int tw = ui_text_width(times, S), tx = 46 * S, track = dw - tx - tw - 28 * S;
    if (track > 20 * S) {
        int done = duration > 0 ? (int)(track * (t / duration)) : 0;
        if (done > track) done = track;
        ui_round(c, tx, mid - 2 * S, track, 4 * S, 2 * S, 0x4A4A56);
        ui_round(c, tx, mid - 2 * S, done, 4 * S, 2 * S, UI_ACCENT);
        ui_circle(c, tx + done, mid, 6 * S, UI_WHITE);
    }
    ui_text(c, dw - tw - 12 * S, mid - 8 * S, times, 0xD8D8E0, S);
}

static void show_progress(double t, double duration) {
    int y = dy + dh + 4, w = gi.width - 40;
    if (windowed) {                       /* the control bar under the picture */
        shown_t = t;
        shown_duration = duration;
        v_dirty(ctrl_view);
        return;
    }
    if (y + 4 > gi.height || duration <= 0 || w <= 0) return;
    static unsigned int bar[4096];
    if (w > 4096) w = 4096;
    int done = (int)(w * (t / duration));
    for (int x = 0; x < w; x++) bar[x] = x < done ? 0xE040C0 : 0x303040;
    for (int k = 0; k < 3; k++) gfx_blit(20, y + k, w, 1, bar, w);
}

/* ---- The sound: resampled to 48 kHz ------------------------------------------------ */

static int sound_on;
static double src_rate, step, phase;      /* phase: position between `prev` and the next sample */
static float prev_l, prev_r;
static short out[PLM_AUDIO_SAMPLES_PER_FRAME * 4 * 2];

static short to16(float v) {
    int s = (int)(v * 32767.0f);
    return (short)(s > 32767 ? 32767 : s < -32768 ? -32768 : s);
}

static void on_audio(plm_t *plm, plm_samples_t *samples, void *user) {
    if (!sound_on) return;
    int n = 0, count = (int)samples->count;
    const float *in = samples->interleaved;
    /* Linear interpolation between consecutive input samples. */
    for (int i = 0; i < count; i++) {
        float l = in[2 * i], r = in[2 * i + 1];
        while (phase < 1.0 && n < (int)(sizeof(out) / sizeof(out[0]) / 2)) {
            out[2 * n] = to16(prev_l + (l - prev_l) * (float)phase);
            out[2 * n + 1] = to16(prev_r + (r - prev_r) * (float)phase);
            n++;
            phase += step;
        }
        phase -= 1.0;
        prev_l = l;
        prev_r = r;
    }
    if (n) sound_write(out, n);           /* waits while the stream's buffer is full */
}

/* The next key, 0 if none; closing the window is 'q'. In the window, the
 * play button is Space and a click on the bar is KEY_SEEK (seek_permille). */
static int next_key(void) { return getkey(); }    /* (full screen: the window's go to the views) */

/* ---- Playing ----------------------------------------------------------------------- */

static void fmt_time(char *buf, double t) {
    int s = (int)t;
    sprintf(buf, "%d:%02d", s / 60, s % 60);
}

static plm_t *plm;
static double duration;
static int quit, ff_over;
static unsigned long last, last_bar;

static double now_time(void) { return use_ff ? ff_time() : plm_get_time(plm); }
static void seek_to(double t) {
    if (use_ff) ff_seek(t);
    else plm_seek(plm, t, FALSE);
}

static void do_key(int key) {
    if (key == 'q' || key == 'Q' || key == 27) quit = 1;
    else if (key == ' ') {
        paused = !paused;
        last_bar = 0;
    } else if (key == KEY_SEEK && duration > 0) {
        seek_to(duration * seek_permille / 1000.0);
        last_bar = 0;
    } else if (key == KEYCODE_LEFT || key == KEYCODE_RIGHT) {
        double t = now_time() + (key == KEYCODE_RIGHT ? SEEK_STEP : -SEEK_STEP);
        if (t < 0) t = 0;
        if (duration > 0 && t > duration - 1) t = duration - 1;
        seek_to(t);
        last_bar = 0;
    }
}

/* Decode what is due and show the newest frame. -> 0 when it is over. */
static int play_step(void) {
    unsigned long now = uptime_ms();
    double elapsed = (now - last) / 1000.0;
    last = now;
    if (elapsed > 0.25) elapsed = 0.25;   /* after a stall: slow down instead of skipping */
    int shown;
    if (use_ff) {
        if (!paused && !ff_decode(elapsed)) ff_over = 1;
        shown = ff_picture_ready();
    } else {
        if (!paused) plm_decode(plm, elapsed);
        shown = latest != 0;
    }
    if (shown) show_frame();
    if (now - last_bar > 500) {
        show_progress(now_time(), duration);
        last_bar = now;
    }
    int over = use_ff ? ff_over : plm_has_ended(plm);
    return !quit && !over ? (shown ? 1 : 2) : 0;   /* 2: nothing new (it may rest) */
}

/* ---- In a window: views ------------------------------------------------------------ */

static void tick(void) {
    if (!play_step()) v_quit(0);
}

static int controls_event(view_t *v, const struct ws_event *e) {
    if (e->type != WS_EV_DOWN) return 0;
    if (e->x < 40 * vs) do_key(' ');
    else {
        int tx = 46 * vs, track = dw - tx - 120 * vs;
        if (track > 0 && e->x >= tx && e->x < tx + track) {
            seek_permille = (e->x - tx) * 1000 / track;
            do_key(KEY_SEEK);
        }
    }
    v_dirty(v);
    return 1;
}

static int picture_event(view_t *v, const struct ws_event *e) {
    (void)v;
    if (e->type != WS_EV_DOWN) return 0;
    do_key(' ');                          /* a click on the picture */
    return 1;
}

static int window_key(view_t *v, int k) {
    (void)v;
    do_key(k);
    if (quit) v_quit(0);
    return 1;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        printf("Usage: video <file>   (MP4, MOV, MKV, WebM, AVI or MPG)\n");
        return 1;
    }
    fd = open(argv[1], OPEN_READ);
    if (fd < 0) {
        printf("video: %s: %s\n", argv[1], os_strerror(fd));
        return 1;
    }
    long size = lseek(fd, 0, SEEK_END);
    lseek(fd, 0, SEEK_SET);
    /* An MPEG-1 program stream starts 00 00 01 BA: pl_mpeg. Anything else: FFmpeg. */
    unsigned char head[4] = { 0 };
    read(fd, head, 4);
    lseek(fd, 0, SEEK_SET);
    use_ff = !(head[0] == 0 && head[1] == 0 && head[2] == 1 && (head[3] == 0xBA || head[3] == 0xB3));
    if (!use_ff) {
        plm_buffer_t *buffer = plm_buffer_create_with_callbacks(load_cb, seek_cb, tell_cb, (size_t)size, 0);
        plm = buffer ? plm_create_with_buffer(buffer, TRUE) : 0;
        if (!plm || !plm_probe(plm, 5000 * 1024) || plm_get_num_video_streams(plm) == 0) use_ff = 1;
    }
    if (use_ff) {
        close(fd);
        const char *why = "";
        if (ff_open(argv[1], &why) != 0) {
            const char *name = strrchr(argv[1], '/') ? strrchr(argv[1], '/') + 1 : argv[1];
            char title[96];
            snprintf(title, sizeof(title), "Can't play %s", name);
            v_alert("video", title, why);
            return 1;
        }
        vw = ff_w;
        vh = ff_h;
        duration = ff_duration;
    } else {
        vw = plm_get_width(plm);
        vh = plm_get_height(plm);
        duration = plm_get_duration(plm);
    }
    char len[16];
    fmt_time(len, duration);
    if (use_ff) printf("%s: %dx%d, %s, %s", argv[1], vw, vh, ff_codecs(), len);
    else printf("%s: %dx%d, %d fps, %s", argv[1], vw, vh, (int)(plm_get_framerate(plm) + 0.5), len);

    if (gfx_info(&gi) != 0) {
        printf("\nvideo: the screen is in text mode; video needs the graphics console.\n");
        return 1;
    }
    if (use_ff ? ff_has_audio() : plm_get_num_audio_streams(plm) > 0) {
        int r = sound_open();
        if (r == 0 && use_ff && ff_sound() != 0) {
            sound_close();
            r = SYSERR_NOENT;
        }
        if (r == 0) {
            sound_on = 1;
            src_rate = use_ff ? ff_rate : plm_get_samplerate(plm);
            step = src_rate / OUT_RATE;
            printf(", sound %d Hz", (int)src_rate);
        } else {
            printf(", no sound (%s)", r == SYSERR_INUSE ? "in use" : "no sound device");
        }
    }
    printf("\n");
    if (!use_ff) {
        plm_set_audio_enabled(plm, sound_on);
        plm_set_audio_lead_time(plm, LEAD_TIME);
        plm_set_video_decode_callback(plm, on_video, 0);
        plm_set_audio_decode_callback(plm, on_audio, 0);
    }

    /* Fit the picture to the screen below the status bar, keeping its shape. */
    int sw = gi.width, sh = gi.height - gi.top;
    windowed = ui_desktop();
    if (windowed) {
        sw = sw * 2 / 3;
        sh = sh * 2 / 3;
    }
    if ((long)vw * sh <= (long)vh * sw) {
        dh = sh;
        dw = (int)((long)vw * sh / vh);
    } else {
        dw = sw;
        dh = (int)((long)vh * sw / vw);
    }
    if (dw > sw) dw = sw;
    if (dh > sh) dh = sh;
    dx = (sw - dw) / 2;
    dy = gi.top + (sh - dh) / 2;
    frame_rgb = use_ff ? malloc(4) : malloc((size_t)vw * vh * 4);   /* (FFmpeg's pictures go straight to `scaled`) */
    if (windowed) {
        const char *name = strrchr(argv[1], '/') ? strrchr(argv[1], '/') + 1 : argv[1];
        vs = ui_scale();
        ctrl_h = 44 * vs;
        if (ui_open(&win, name, dw, dh + ctrl_h) != 0) windowed = 0;   /* full screen after all */
    }
    scaled = calloc((size_t)dw * dh, 4);
    xmap = malloc((size_t)dw * sizeof(int));
    if (!frame_rgb || !scaled || !xmap) {
        printf("video: not enough memory\n");
        return 1;
    }
    for (int x = 0; x < dw; x++) xmap[x] = x * vw / dw;

    last = uptime_ms();
    if (windowed) {
        view_t *root = v_box(0, V_COLUMN);
        root->pad = root->gap = 0;
        root->on_key = window_key;
        pic_view = v_custom(root, draw_picture, picture_event);
        ctrl_view = v_custom(root, draw_controls, controls_event);
        ctrl_view->flex = 0;
        ctrl_view->want_h = ctrl_h / vs;
        show_progress(0, duration);
        v_run(&win, root, 4, tick);
    } else {
        gfx_begin();
        clear_screen_area();
        int r;
        while ((r = play_step()) != 0) {
            int key;
            while ((key = next_key()) != 0) do_key(key);
            if (r == 2) sleep_ms(paused ? 30 : 4);
        }
    }
    if (!windowed) gfx_end();
    if (sound_on) sound_close();
    char at[16];
    fmt_time(at, now_time());
    if (use_ff) {
        ff_close();
    } else {
        plm_destroy(plm);
        close(fd);
    }
    printf("Stopped at %s, %ld frames shown.\n", at, frames_shown);
    return 0;
}
