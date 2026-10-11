// video <file.mpg> - play a video with its sound
//
// MPEG-1 video with MP2 sound (".mpg"), decoded by pl_mpeg
// (third_party/pl_mpeg, MIT). Any video can be turned into one on a PC with
// ffmpeg:
//     ffmpeg -i in.mp4 -c:v mpeg1video -q:v 4 -c:a mp2 -b:a 192k -f mpeg out.mpg
// (add -vf scale=1280:-2 for big videos: decoding is done on one core).
//
// The file is read as it plays (read/lseek), the picture is scaled to fit the
// screen, the sound goes to the speakers as a 48 kHz stream. Keys:
//   Space  pause      Left/Right  10 s back/forward      Q or Esc  stop
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <os.h>
#include <ui.h>

#define PLM_NO_STDIO
#define PL_MPEG_IMPLEMENTATION
#include <pl_mpeg.h>

#define OUT_RATE     48000
#define LEAD_TIME    0.30                 /* the sound stream's buffer: ~1/3 s */
#define SEEK_STEP    10.0

static int fd;

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
#define KEY_SEEK 0x200
static void fmt_time(char *buf, double t);

static void on_video(plm_t *plm, plm_frame_t *frame, void *user) {
    latest = frame;                       /* shown after plm_decode: only the newest one */
}

static void show_frame(void) {
    plm_frame_to_bgra(latest, (uint8_t *)frame_rgb, vw * 4);   /* bytes B,G,R,A = 0xAARRGGBB */
    for (int y = 0; y < dh; y++) {
        const unsigned int *src = frame_rgb + (long)(y * vh / dh) * vw;
        unsigned int *dst = scaled + (long)y * dw;
        for (int x = 0; x < dw; x++) dst[x] = src[xmap[x]] & 0xFFFFFF;
    }
    if (windowed) ui_present(&win);       /* `scaled` is the window's own pixels */
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
static void show_progress(double t, double duration) {
    int y = dy + dh + 4, w = gi.width - 40;
    if (windowed) {                       /* the control bar under the picture */
        ui_canvas *c = &win.c;
        int top = dh, mid = top + ctrl_h / 2, S = vs;
        ui_fill(c, 0, top, dw, ctrl_h, 0x1C1C22);
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
        ui_present(&win);
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
static int next_key(void) {
    if (!windowed) return getkey();
    struct ws_event e;
    while (ui_poll(&win, &e)) {
        if (e.type == WS_EV_CLOSE) return 'q';
        if (e.type == WS_EV_KEY) return e.key;
        if (e.type == WS_EV_DOWN && e.y >= dh) {
            if (e.x < 40 * vs) return ' ';
            int tx = 46 * vs, track = dw - tx - 120 * vs;
            if (track > 0 && e.x >= tx && e.x < tx + track) {
                seek_permille = (e.x - tx) * 1000 / track;
                return KEY_SEEK;
            }
        }
        if (e.type == WS_EV_DOWN && e.y < dh) return ' ';   /* a click on the picture */
    }
    return 0;
}

/* ---- Playing ----------------------------------------------------------------------- */

static void fmt_time(char *buf, double t) {
    int s = (int)t;
    sprintf(buf, "%d:%02d", s / 60, s % 60);
}

int main(int argc, char **argv) {
    if (argc < 2) {
        printf("Usage: video <file.mpg>\n"
               "Plays MPEG-1 video with MP2 sound. To make one on a PC:\n"
               "  ffmpeg -i in.mp4 -c:v mpeg1video -q:v 4 -c:a mp2 -b:a 192k -f mpeg out.mpg\n");
        return 1;
    }
    fd = open(argv[1], OPEN_READ);
    if (fd < 0) {
        printf("video: %s: %s\n", argv[1], os_strerror(fd));
        return 1;
    }
    long size = lseek(fd, 0, SEEK_END);
    lseek(fd, 0, SEEK_SET);
    plm_buffer_t *buffer = plm_buffer_create_with_callbacks(load_cb, seek_cb, tell_cb, (size_t)size, 0);
    plm_t *plm = buffer ? plm_create_with_buffer(buffer, TRUE) : 0;
    if (!plm || !plm_probe(plm, 5000 * 1024) || plm_get_num_video_streams(plm) == 0) {
        printf("video: %s is not an MPEG-1 video (.mpg). Convert it on a PC with:\n"
               "  ffmpeg -i in.mp4 -c:v mpeg1video -q:v 4 -c:a mp2 -b:a 192k -f mpeg out.mpg\n", argv[1]);
        return 1;
    }
    vw = plm_get_width(plm);
    vh = plm_get_height(plm);
    double duration = plm_get_duration(plm);
    char len[16];
    fmt_time(len, duration);
    printf("%s: %dx%d, %d fps, %s", argv[1], vw, vh, (int)(plm_get_framerate(plm) + 0.5), len);

    if (gfx_info(&gi) != 0) {
        printf("\nvideo: the screen is in text mode; video needs the graphics console.\n");
        return 1;
    }
    if (plm_get_num_audio_streams(plm) > 0) {
        int r = sound_open();
        if (r == 0) {
            sound_on = 1;
            src_rate = plm_get_samplerate(plm);
            step = src_rate / OUT_RATE;
            printf(", sound %d Hz", (int)src_rate);
        } else {
            printf(", no sound (%s)", r == SYSERR_INUSE ? "in use" : "no sound device");
        }
    }
    printf("\n");
    plm_set_audio_enabled(plm, sound_on);
    plm_set_audio_lead_time(plm, LEAD_TIME);
    plm_set_video_decode_callback(plm, on_video, 0);
    plm_set_audio_decode_callback(plm, on_audio, 0);

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
    frame_rgb = malloc((size_t)vw * vh * 4);
    if (windowed) {
        const char *name = strrchr(argv[1], '/') ? strrchr(argv[1], '/') + 1 : argv[1];
        vs = ui_scale();
        ctrl_h = 44 * vs;
        if (ui_open(&win, name, dw, dh + ctrl_h) == 0) {
            ui_fill(&win.c, 0, 0, dw, dh + ctrl_h, 0);
            scaled = win.c.px;
        } else {
            windowed = 0;                 /* full screen after all */
        }
    }
    if (!windowed) scaled = malloc((size_t)dw * dh * 4);
    xmap = malloc((size_t)dw * sizeof(int));
    if (!frame_rgb || !scaled || !xmap) {
        printf("video: not enough memory\n");
        return 1;
    }
    for (int x = 0; x < dw; x++) xmap[x] = x * vw / dw;

    if (!windowed) {
        gfx_begin();
        clear_screen_area();
    }
    int quit = 0;
    unsigned long last = uptime_ms(), last_bar = 0;
    while (!quit && !plm_has_ended(plm)) {
        int key;
        while ((key = next_key()) != 0) {
            if (key == 'q' || key == 'Q' || key == 27) quit = 1;
            else if (key == ' ') {
                paused = !paused;
                last_bar = 0;
            } else if (key == KEY_SEEK && duration > 0) {
                plm_seek(plm, duration * seek_permille / 1000.0, FALSE);
                last_bar = 0;
            }
            else if (key == KEYCODE_LEFT || key == KEYCODE_RIGHT) {
                double t = plm_get_time(plm) + (key == KEYCODE_RIGHT ? SEEK_STEP : -SEEK_STEP);
                if (t < 0) t = 0;
                if (duration > 0 && t > duration - 1) t = duration - 1;
                plm_seek(plm, t, FALSE);
                last_bar = 0;
            }
        }
        unsigned long now = uptime_ms();
        double elapsed = (now - last) / 1000.0;
        last = now;
        if (elapsed > 0.25) elapsed = 0.25;   /* after a stall: slow down instead of skipping */
        if (!paused) plm_decode(plm, elapsed);
        if (latest) show_frame();
        if (now - last_bar > 500) {
            show_progress(plm_get_time(plm), duration);
            last_bar = now;
        }
        if (!latest) sleep_ms(paused ? 30 : 4);
    }
    if (!windowed) gfx_end();
    if (sound_on) sound_close();
    char at[16];
    fmt_time(at, plm_get_time(plm));
    plm_destroy(plm);
    close(fd);
    printf("Stopped at %s, %ld frames shown.\n", at, frames_shown);
    return 0;
}
