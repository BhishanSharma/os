// terminal [command] - a shell in a window on the desktop
//
// The shell (`shell -t`) reads the lines typed here from a pipe and writes
// into another pipe, which a second thread reads into the scrollback. The
// line being typed is kept apart and drawn after the output, so it can be
// edited (Left/Right, Backspace) and recalled (Up/Down: the commands before).
// The wheel, or Shift+Up/Down, scrolls back through older output; the green
// button makes the window fill the screen and the text follows its size.
// With a command, it is typed first (Files opens programs with `terminal x`).
#include <stdio.h>
#include <string.h>
#include <os.h>
#include <ui.h>

#define MAXC 240                        /* the widest line kept */
#define LINES 1000                      /* the scrollback */
#define HISTORY 64                      /* commands remembered for Up/Down */
#define BG 0x1E1E24
#define FG 0xE6E6E6
#define CURSOR 0x7FD1FF
#define KEY_SCROLL_UP   1001            /* Shift+Up/Down (the keyboard driver) */
#define KEY_SCROLL_DOWN 1002

static ui_window win;
static mutex_t lock;
static volatile int dirty = 1;
static int to_shell, from_shell, shell_pid;

/* The output: a ring of lines; the cursor is on the newest one. */
static char text[LINES][MAXC];
static int first, count = 1, col;       /* first: the oldest line in the ring */
static int cols = 80, rows = 25, back;  /* the window in characters; back: lines scrolled up */

/* The line being typed. */
static char line[256];
static int line_len, cur, secret;       /* cur: the cursor in it; secret: after "Password: " */
static char history[HISTORY][256];
static int n_history, recall = -1;      /* recall: the entry Up/Down show, -1 the new line */
static char draft[256];                 /* the new line, kept while recalling */

static char *row_text(int i) { return text[(first + i) % LINES]; }

static void new_line(void) {
    if (count == LINES) first = (first + 1) % LINES;   /* the oldest line goes */
    else count++;
    memset(row_text(count - 1), ' ', MAXC);
    col = 0;
    if (back) back++;                                  /* scrolled up: the view stays put */
}

static void put(char c) {
    if (c == '\r') {
        col = 0;
    } else if (c == '\n') {
        new_line();
    } else if (c == '\b') {
        if (col > 0) row_text(count - 1)[--col] = ' ';
    } else if (c == '\t') {
        do put(' '); while (col % 4);
    } else if ((unsigned char)c >= ' ') {
        if (col >= cols || col >= MAXC) new_line();
        row_text(count - 1)[col++] = c;
    }
}

/* What the shell and its programs print. */
static void reader(void *arg) {
    char buf[256];
    for (;;) {
        long n = read(from_shell, buf, sizeof(buf));
        if (n <= 0) exit(0);                            /* the shell has ended */
        mutex_lock(&lock);
        for (long i = 0; i < n; i++) put(buf[i]);
        dirty = 1;
        mutex_unlock(&lock);
    }
}

static void fit(void) {                                 /* the window's size in characters */
    int S = win.scale;
    cols = (win.c.w - 8 * S) / (8 * S);
    rows = (win.c.h - 8 * S) / (16 * S);
    if (cols > MAXC) cols = MAXC;
    if (cols < 10) cols = 10;
    if (rows < 2) rows = 2;
}

static void draw(void) {
    int S = win.scale, cw = 8 * S, ch = 16 * S, m = 4 * S;
    ui_fill(&win.c, 0, 0, win.c.w, win.c.h, BG);
    /* The screen's lines from the bottom up: the typed line follows the
     * output on the newest line, wrapped at the window's width. */
    static char tail[MAXC + 256];
    int tlen = col;
    memcpy(tail, row_text(count - 1), (size_t)col);
    if (!secret) {
        memcpy(tail + tlen, line, (size_t)line_len);
        tlen += line_len;
    }
    int tail_rows = tlen / cols + 1;
    int total = count - 1 + tail_rows;
    int bottom = total - back;                            /* the line after the last one shown */
    for (int r = 0; r < rows; r++) {
        int i = bottom - rows + r;
        if (i < 0) continue;
        const char *s;
        int n;
        if (i < count - 1) {
            s = row_text(i);
            n = cols < MAXC ? cols : MAXC;
        } else {
            int k = (i - (count - 1)) * cols;
            s = tail + k;
            n = tlen - k < cols ? tlen - k : cols;
        }
        for (int c = 0; c < n; c++)
            if (s[c] != ' ') ui_char(&win.c, m + c * cw, m + r * ch, (unsigned char)s[c], FG, S);
    }
    if (!back) {                                           /* the cursor */
        int at = col + (secret ? 0 : cur);
        int r = rows - (tail_rows - at / cols);
        ui_fill(&win.c, m + (at % cols) * cw, m + r * ch, cw, ch, CURSOR);
        if (!secret && cur < line_len)
            ui_char(&win.c, m + (at % cols) * cw, m + r * ch, (unsigned char)line[cur], BG, S);
    } else {                                               /* where the view is in the scrollback */
        int track = win.c.h - 2 * m, bar = track * rows / total;
        if (bar < 12 * S) bar = 12 * S;
        int y = m + (track - bar) * (bottom - rows) / (total - rows > 0 ? total - rows : 1);
        ui_round(&win.c, win.c.w - 6 * S, y, 3 * S, bar, S, 0x8A8A96);
    }
    ui_present(&win);
}

static void scroll_by(int n) {
    int max = count - 1 + (col + line_len) / cols + 1 - rows;
    back += n;
    if (back > max) back = max;
    if (back < 0) back = 0;
}

static void send_line(const char *s) {
    write(to_shell, s, strlen(s));
    write(to_shell, "\n", 1);
}

/* Ctrl+C: interrupt the programs the shell is running -> how many. */
static int interrupt(void) {
    struct os_task t;
    int n = 0;
    for (int i = 0, used; (used = taskinfo(i, &t)) >= 0; i++)
        if (used && t.pid > 0 && t.parent == shell_pid && t.pid != shell_pid && sigsend(t.pid, SIGINT) == 0) n++;
    return n;
}

static int start_shell(void) {
    int in[2], out[2];
    if (pipe(in) < 0 || pipe(out) < 0) return -1;
    int saved[3] = { dup(0), dup(1), dup(2) };
    dup2(in[0], 0);
    dup2(out[1], 1);
    dup2(out[1], 2);
    char *argv[] = { "shell", "-t", 0 };
    int pid = shell_pid = spawn("shell", argv, 0);
    for (int i = 0; i < 3; i++) {
        dup2(saved[i], i);
        close(saved[i]);
    }
    close(in[0]);
    close(out[1]);
    to_shell = in[1];
    from_shell = out[0];
    return pid;
}

static void set_line(const char *s) {
    snprintf(line, sizeof(line), "%s", s);
    line_len = cur = (int)strlen(line);
}

static void key(int k) {
    if (k == KEY_SCROLL_UP || k == KEY_SCROLL_DOWN) {
        scroll_by(k == KEY_SCROLL_UP ? 1 : -1);
        return;
    }
    back = 0;                                           /* typing: back to the bottom */
    if (k == 3) {                                       /* Ctrl+C */
        put('^');
        put('C');
        put('\n');
        line_len = cur = secret = 0;
        recall = -1;
        if (interrupt() == 0) send_line("\x03");       /* a kernel command's question, or a new prompt */
    } else if (k == '\n' || k == '\r') {
        line[line_len] = 0;
        if (!secret) {
            for (int i = 0; i < line_len; i++) put(line[i]);
            if (line_len && (!n_history || strcmp(history[(n_history - 1) % HISTORY], line)))
                snprintf(history[n_history++ % HISTORY], sizeof(history[0]), "%s", line);
        }
        put('\n');
        send_line(line);
        line_len = cur = secret = 0;
        recall = -1;
    } else if (k == KEYCODE_UP || k == KEYCODE_DOWN) {
        if (secret || !n_history) return;
        int oldest = n_history > HISTORY ? n_history - HISTORY : 0;
        if (k == KEYCODE_UP) {
            if (recall < 0) {
                memcpy(draft, line, sizeof(draft));
                draft[line_len] = 0;
                recall = n_history - 1;
            } else if (recall > oldest) {
                recall--;
            }
            set_line(history[recall % HISTORY]);
        } else if (recall >= 0) {
            if (++recall >= n_history) {
                recall = -1;
                set_line(draft);
            } else {
                set_line(history[recall % HISTORY]);
            }
        }
    } else if (k == KEYCODE_LEFT) {
        if (cur > 0) cur--;
    } else if (k == KEYCODE_RIGHT) {
        if (cur < line_len) cur++;
    } else if (k == '\b') {
        if (cur > 0) {
            memmove(line + cur - 1, line + cur, (size_t)(line_len - cur));
            line_len--;
            cur--;
        }
    } else if (k >= ' ' && k < 127 && line_len < (int)sizeof(line) - 1) {
        if (!line_len) {                                /* after "Password: " it is not shown */
            const char *now = row_text(count - 1);
            secret = col >= 9 && !memcmp(now + col - 9, "assword: ", 9);
        }
        memmove(line + cur + 1, line + cur, (size_t)(line_len - cur));
        line[cur++] = (char)k;
        line_len++;
    }
}

int main(int argc, char **argv) {
    int S = ui_scale();
    if (ui_open(&win, "Terminal", 80 * 8 * S + 8 * S, 25 * 16 * S + 8 * S) < 0) {
        printf("terminal: the desktop is not running (type windowserver)\n");
        return 1;
    }
    ui_set_resizable(&win);
    fit();
    memset(text[0], ' ', MAXC);
    if (start_shell() < 0) {
        printf("terminal: cannot start the shell\n");
        return 1;
    }
    thread_create(reader, 0);
    if (argc > 1) {
        char cmd[256] = "";
        for (int i = 1; i < argc; i++) {
            if (i > 1) strcat(cmd, " ");
            if (strlen(cmd) + strlen(argv[i]) + 2 < sizeof(cmd)) strcat(cmd, argv[i]);
        }
        send_line(cmd);
    }
    for (;;) {
        struct ws_event e;
        while (ui_poll(&win, &e)) {
            if (e.type == WS_EV_CLOSE) exit(0);
            mutex_lock(&lock);
            if (e.type == WS_EV_KEY) key(e.key);
            else if (e.type == WS_EV_WHEEL) scroll_by(-e.key * 3);
            else if (e.type == WS_EV_RESIZE) fit();
            dirty = 1;
            mutex_unlock(&lock);
        }
        if (dirty) {
            mutex_lock(&lock);
            dirty = 0;
            draw();
            mutex_unlock(&lock);
        }
        sleep_ms(15);
    }
}
