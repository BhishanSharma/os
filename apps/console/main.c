// console - the system log, live (like macOS's Console)
//
// Everything the kernel and the programs have printed since power-on: the
// boot steps, drivers, which app crashed and where. It follows the end as
// lines come in; the wheel or Up/Down scroll back (then it stays put until
// you scroll to the end again). logd saves the same log to the boot stick.
//
// Built from views (view.h): the log is one custom view that draws its lines
// and takes the wheel and the keys.
#include <stdio.h>
#include <string.h>
#include <os.h>
#include <view.h>

#define TEXT_MAX (512 * 1024)
#define LINES_MAX 16384
#define BG 0x1E1E24
#define FG 0xDADADF
#define WARN 0xFFCC66
#define ERR 0xFF7A70

static int S;
static char text[TEXT_MAX];
static int len;
static int starts[LINES_MAX], n_lines = 1;         /* where each line starts in text */
static int top, follow = 1;                        /* the first line shown; stuck to the end */
static unsigned long from;                         /* how far into the system log */
static view_t *log_view;

static int rows(void) { return (log_view->h - 8 * S) / (18 * S); }

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

static void draw_log(view_t *v, ui_canvas *c) {
    clamp();
    ui_fill(c, v->x, v->y, v->w, v->h, BG);
    char line[512];
    int lh = 18 * S, cols = (v->w - 12 * S) / (8 * S);
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
        ui_text_style(c, v->x + 6 * S, v->y + 4 * S + r * lh, line, color, S, UI_MONO);
    }
    if (!follow) {                                   /* where the view is */
        int track = v->h - 8 * S, bar = track * rows() / (n_lines > 0 ? n_lines : 1);
        if (bar < 12 * S) bar = 12 * S;
        int span = n_lines - rows() > 0 ? n_lines - rows() : 1;
        ui_round(c, v->x + v->w - 6 * S, v->y + 4 * S + (track - bar) * top / span, 3 * S, bar, S, 0x8A8A96);
    }
}

static int log_event(view_t *v, const struct ws_event *e) {
    if (e->type == WS_EV_WHEEL) top += e->key * 3;
    else if (e->type == WS_EV_KEY && (e->key == KEYCODE_UP || e->key == KEYCODE_DOWN)) top += e->key == KEYCODE_UP ? -1 : 1;
    else if (e->type == WS_EV_KEY && e->key == ' ') top += rows();   /* a page down */
    else return 0;
    follow = 0;
    if (top < 0) top = 0;
    if (top >= n_lines - rows()) follow = 1;         /* back at the end: follow again */
    v_dirty(v);
    return 1;
}

static void tick(void) {                             /* now and then: new lines */
    if (pull()) v_dirty(log_view);
}

int main(void) {
    S = ui_scale();
    ui_window win;
    if (ui_open(&win, "Console", 820 * S, 480 * S) < 0) {
        printf("console: the desktop is not running (type windowserver)\n");
        return 1;
    }
    ui_set_resizable(&win);
    view_t *root = v_box(0, V_COLUMN);
    root->pad = 0;
    log_view = v_custom(root, draw_log, log_event);
    log_view->focusable = 1;
    pull();
    return v_run(&win, root, 500, tick);
}
