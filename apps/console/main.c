// console - the system log, live (like macOS's Console)
//
// Everything the kernel and the programs have printed since power-on: the
// boot steps, drivers, which app crashed and where. It follows the end as
// lines come in; the wheel or Up/Down scroll back (then it stays put until
// you scroll to the end again). logd saves the same log to the boot stick.
#include <stdio.h>
#include <string.h>
#include <os.h>
#include <ui.h>

#define TEXT_MAX (512 * 1024)
#define LINES_MAX 16384
#define BG 0x1E1E24
#define FG 0xDADADF
#define WARN 0xFFCC66
#define ERR 0xFF7A70

static ui_window win;
static int S;
static char text[TEXT_MAX];
static int len;
static int starts[LINES_MAX], n_lines = 1;         /* where each line starts in text */
static int top, follow = 1;                        /* the first line shown; stuck to the end */
static unsigned long from;                         /* how far into the system log */

static int rows(void) { return (win.c.h - 8 * S) / (18 * S); }

static void reindex(void) {
    n_lines = 1;
    starts[0] = 0;
    for (int i = 0; i < len && n_lines < LINES_MAX; i++)
        if (text[i] == '\n') starts[n_lines++] = i + 1;
}

/* New log text: printable characters and line ends only (no colour codes). */
static int pull(void) {
    static char buf[16384];
    int got, any = 0;
    while ((got = syslog_read(&from, buf, sizeof(buf))) > 0) {
        if (len + got > TEXT_MAX) {                  /* full: drop the older half */
            int cut = TEXT_MAX / 2;
            while (cut < len && text[cut - 1] != '\n') cut++;
            memmove(text, text + cut, (size_t)(len - cut));
            len -= cut;
        }
        for (int i = 0; i < got; i++) {
            char c = buf[i];
            if (c == 27) {                           /* an escape sequence: skip it */
                while (i + 1 < got && !((buf[i + 1] >= 'A' && buf[i + 1] <= 'Z') || (buf[i + 1] >= 'a' && buf[i + 1] <= 'z'))) i++;
                i++;
                continue;
            }
            if (c == '\n' || c == '\t' || (c >= ' ' && c < 127)) text[len++] = c == '\t' ? ' ' : c;
        }
        any = 1;
    }
    if (any) reindex();
    return any;
}

static void clamp(void) {
    int max = n_lines - rows();
    if (max < 0) max = 0;
    if (follow || top > max) top = max;
    if (top < 0) top = 0;
    follow = top == max;
}

static void draw(void) {
    ui_canvas *c = &win.c;
    clamp();
    ui_fill(c, 0, 0, c->w, c->h, BG);
    char line[512];
    int lh = 18 * S, cols = (c->w - 12 * S) / (8 * S);
    for (int r = 0; r < rows() && top + r < n_lines; r++) {
        int i = top + r, a = starts[i], b = i + 1 < n_lines ? starts[i + 1] - 1 : len, n = b - a;
        if (n > cols) n = cols;
        if (n > (int)sizeof(line) - 1) n = (int)sizeof(line) - 1;
        if (n < 0) n = 0;
        memcpy(line, text + a, (size_t)n);
        line[n] = 0;
        unsigned color = strstr(line, "ERROR") || strstr(line, "terminated") || strstr(line, "PANIC") ||
                         strstr(line, "quit unexpectedly") ? ERR
                       : strstr(line, "WARN") ? WARN : FG;
        ui_text_style(c, 6 * S, 4 * S + r * lh, line, color, S, UI_MONO);
    }
    if (!follow) {                                   /* where the view is */
        int track = c->h - 8 * S, bar = track * rows() / (n_lines > 0 ? n_lines : 1);
        if (bar < 12 * S) bar = 12 * S;
        int span = n_lines - rows() > 0 ? n_lines - rows() : 1;
        ui_round(c, c->w - 6 * S, 4 * S + (track - bar) * top / span, 3 * S, bar, S, 0x8A8A96);
    }
    ui_present(&win);
}

int main(void) {
    S = ui_scale();
    if (ui_open(&win, "Console", 820 * S, 480 * S) < 0) {
        printf("console: the desktop is not running (type windowserver)\n");
        return 1;
    }
    ui_set_resizable(&win);
    pull();
    draw();
    for (;;) {
        struct ws_event e;
        if (!ui_wait(&win, &e, 500)) {               /* now and then: new lines */
            if (pull()) draw();
            continue;
        }
        if (e.type == WS_EV_CLOSE) return 0;
        if (e.type == WS_EV_WHEEL) {
            top += e.key * 3;
            follow = 0;
        } else if (e.type == WS_EV_KEY && (e.key == KEYCODE_UP || e.key == KEYCODE_DOWN)) {
            top += e.key == KEYCODE_UP ? -1 : 1;
            follow = 0;
        } else if (e.type == WS_EV_KEY && e.key == ' ') {
            top += rows();                           /* a page down */
            follow = 0;
        } else if (e.type != WS_EV_RESIZE) {
            continue;
        }
        if (top < 0) top = 0;
        int max = n_lines - rows();
        if (top >= max) follow = 1;                  /* back at the end: follow again */
        draw();
    }
}
