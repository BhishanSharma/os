// terminal [command] - a shell in a window on the desktop
//
// The shell (`shell -t`) runs on a pseudo-terminal (pty(): this window has
// the master end, the shell and its programs the slave as 0-2). A second
// thread reads what they print into the scrollback, through a VT100 screen:
// cursor movement, erasing, colours, scroll regions, the alternate screen,
// so full-screen programs (an editor, a pager, bash's line editing) work.
//
// The pty's mode decides who edits the line. Cooked (the shell, most
// programs): the line being typed is kept here and drawn after the output,
// edited with Left/Right/Backspace and recalled with Up/Down, and sent whole
// with Enter. Raw (a program asked with tcsetattr): every key goes straight
// to it, the arrows as escape sequences, Ctrl+C through the pty (SIGINT).
// The wheel, or Shift+Up/Down, scrolls back through older output; the green
// button makes the window fill the screen and the programs are told its size.
// Dragging selects text and copies it (the clipboard every program shares);
// Ctrl+V pastes into the line being typed, a right click pastes in any mode.
// Output is UTF-8: each cell holds a character (accents, Greek, Cyrillic,
// box drawing...).
// With a command, it is typed first (Files opens programs with `terminal x`).
//
// Built from views (view.h): the screen is one custom view that draws the
// text and takes the keys; the reader thread marks it dirty.
#include <stdio.h>
#include <string.h>
#include <os.h>
#include <view.h>

#define MAXC 240                        /* the widest line kept */
#define LINES 1000                      /* the scrollback */
#define HISTORY 64                      /* commands remembered for Up/Down */
#define BG 0x1E1E24
#define FG 0xE6E6E6
#define CURSOR 0x7FD1FF
#define DEF 16                          /* a cell's colour: the default */
#define KEY_SCROLL_UP   1001            /* Shift+Up/Down (the keyboard driver) */
#define KEY_SCROLL_DOWN 1002

static const unsigned palette[16] = {
    0x000000, 0xCD3131, 0x0DBC79, 0xE5E510, 0x2472C8, 0xBC3FBC, 0x11A8CD, 0xE5E5E5,
    0x666666, 0xF14C4C, 0x23D18B, 0xF5F543, 0x3B8EEA, 0xD670D6, 0x29B8DB, 0xFFFFFF,
};

static ui_window win;
static view_t *screen;
static mutex_t lock;
static volatile int dirty = 1;
static int master, shell_pid;

/* The output: a ring of lines, each with its cells' colours. The screen is
 * the newest `rows` of them; the cursor is line `cl` (an index from the
 * oldest), column `col`. */
static unsigned short text[LINES][MAXC];   /* characters (code points) */
static unsigned char fgc[LINES][MAXC], bgc[LINES][MAXC];
static int first, count = 1, col, cl;   /* first: the oldest line in the ring */
static long dropped;                    /* lines that left the ring (absolute line = dropped + index) */
static int utf_need;                    /* UTF-8: continuation bytes still to come */
static unsigned utf_cp;

/* The selection (absolute lines, columns), while dragging and after. */
static long sel_l0, sel_l1;
static int sel_c0, sel_c1, selecting, has_selection;
static int cols = 80, rows = 25, back;  /* the window in characters; back: lines scrolled up */
static int told_cols, told_rows;        /* the size the programs were told */

/* The VT100 state. */
static int pen_fg = DEF, pen_bg = DEF, bold, reverse, cursor_on = 1, app_keys;
static int top_m, bot_m = -1;           /* the scroll region (screen rows); -1: the last */
static int alt_start = -1;              /* the alternate screen: where it began, -1 none */
static int saved_cl, saved_col, alt_cl, alt_col;
static int esc;                         /* 0, 1 after ESC, 2 in CSI, 3 in OSC, 4 one more byte */
static int args[16], n_args, private_mode;

/* The line being typed (cooked mode). */
static char line[256];
static int line_len, cur, secret;       /* cur: the cursor in it; secret: after "Password: " */
static char history[HISTORY][256];
static int n_history, recall = -1;      /* recall: the entry Up/Down show, -1 the new line */
static char draft[256];                 /* the new line, kept while recalling */

static int ring(int i) { return (first + i) % LINES; }
static unsigned short *row_text(int i) { return text[ring(i)]; }

static void spaces(unsigned short *t, int n) { for (int i = 0; i < n; i++) t[i] = ' '; }

static void blank(int i, int from, int to) {          /* cells [from, to) of line i */
    if (to > MAXC) to = MAXC;
    if (from >= to) return;
    int r = ring(i);
    spaces(text[r] + from, to - from);
    memset(fgc[r] + from, DEF, (size_t)(to - from));
    memset(bgc[r] + from, pen_bg, (size_t)(to - from));
}

static void new_line(void) {                          /* one more line at the bottom */
    if (count == LINES) {                             /* the oldest line goes */
        first = (first + 1) % LINES;
        dropped++;
        if (alt_start > 0) alt_start--;
    } else {
        count++;
    }
    int r = ring(count - 1);
    spaces(text[r], MAXC);
    memset(fgc[r], DEF, MAXC);
    memset(bgc[r], DEF, MAXC);
    if (back) back++;                                  /* scrolled up: the view stays put */
}

/* The screen's first line, making sure it has `rows` lines. */
static int screen_top(void) {
    while (count < rows) new_line();
    return count - rows;
}

static int region_bottom(void) { return bot_m < 0 || bot_m >= rows ? rows - 1 : bot_m; }

static void copy_line(int to, int from) {
    int a = ring(to), b = ring(from);
    memcpy(text[a], text[b], sizeof(text[0]));
    memcpy(fgc[a], fgc[b], MAXC);
    memcpy(bgc[a], bgc[b], MAXC);
}

/* Lines [t, b] of the screen move up (n > 0) or down by |n|, blanks coming in. */
static void scroll_region(int t, int b, int n) {
    int base = screen_top();
    if (n > 0) {
        for (int i = t; i <= b; i++)
            if (i + n <= b) copy_line(base + i, base + i + n);
            else blank(base + i, 0, MAXC);
    } else {
        n = -n;
        for (int i = b; i >= t; i--)
            if (i - n >= t) copy_line(base + i, base + i - n);
            else blank(base + i, 0, MAXC);
    }
}

static void line_feed(void) {
    if (cl == count - 1 && top_m == 0 && region_bottom() == rows - 1) {
        new_line();                                    /* the usual: into the scrollback */
        cl = count - 1;
        return;
    }
    int base = screen_top(), y = cl - base;
    if (y == region_bottom()) scroll_region(top_m, region_bottom(), 1);
    else if (cl < count - 1) cl++;
    else { new_line(); cl = count - 1; }
}

static void reverse_index(void) {                     /* ESC M: up, scrolling down at the top */
    int base = screen_top(), y = cl - base;
    if (y == top_m) scroll_region(top_m, region_bottom(), -1);
    else if (y > 0) cl--;
}

static void move_to(int y, int x) {                   /* screen coordinates */
    int base = screen_top();
    if (y < 0) y = 0;
    if (y >= rows) y = rows - 1;
    if (x < 0) x = 0;
    if (x >= cols) x = cols - 1;
    cl = base + y;
    col = x;
}

static void glyph(unsigned c) {
    if (col >= cols || col >= MAXC) {                 /* (wraps before the next one) */
        col = 0;
        line_feed();
    }
    int r = ring(cl), f = pen_fg, b = pen_bg;
    if (bold && f < 8) f += 8;
    if (reverse) {                                    /* (the default pair reversed: dark on light) */
        int t = f;
        f = b == DEF ? 17 : b;
        b = t == DEF ? 7 : t;
    }
    text[r][col] = (unsigned short)(c > 0xFFFF ? 0xFFFD : c);
    fgc[r][col] = (unsigned char)f;
    bgc[r][col] = (unsigned char)b;
    col++;
}

static void reply(const char *s) { write(master, s, strlen(s)); }

static int arg(int i, int dflt) { return i < n_args && args[i] ? args[i] : dflt; }

static void sgr(void) {
    if (n_args == 0) n_args = 1, args[0] = 0;
    for (int i = 0; i < n_args; i++) {
        int a = args[i];
        if (a == 0) pen_fg = pen_bg = DEF, bold = reverse = 0;
        else if (a == 1) bold = 1;
        else if (a == 22) bold = 0;
        else if (a == 7) reverse = 1;
        else if (a == 27) reverse = 0;
        else if (a >= 30 && a <= 37) pen_fg = a - 30;
        else if (a == 39) pen_fg = DEF;
        else if (a >= 40 && a <= 47) pen_bg = a - 40;
        else if (a == 49) pen_bg = DEF;
        else if (a >= 90 && a <= 97) pen_fg = a - 90 + 8;
        else if (a >= 100 && a <= 107) pen_bg = a - 100 + 8;
        else if ((a == 38 || a == 48) && i + 2 < n_args && args[i + 1] == 5) {   /* 256 colours */
            int n = args[i + 2], c = n < 16 ? n : n >= 232 ? (n < 244 ? 8 : 7) : 7;
            if (a == 38) pen_fg = c; else pen_bg = c;
            i += 2;
        } else if ((a == 38 || a == 48) && i + 4 < n_args && args[i + 1] == 2) {
            i += 4;                                    /* (true colour: kept as it was) */
        }
    }
}

static void set_mode(int on) {
    for (int i = 0; i < n_args; i++) {
        int m = args[i];
        if (!private_mode) continue;
        if (m == 25) cursor_on = on;
        else if (m == 1) app_keys = on;
        else if (m == 1049 || m == 1047 || m == 47) {
            if (on && alt_start < 0) {                 /* a fresh screen; the old one kept */
                alt_cl = cl, alt_col = col;
                alt_start = count;
                for (int r = 0; r < rows; r++) new_line();
                cl = count - rows;
                col = 0;
            } else if (!on && alt_start >= 0) {        /* back to it as it was */
                if (count > alt_start) count = alt_start;
                if (count < 1) count = 1;
                cl = alt_cl < count ? alt_cl : count - 1;
                col = alt_col;
                alt_start = -1;
                top_m = 0, bot_m = -1;
            }
        }
    }
}

static void csi(char final) {
    int base = screen_top(), y = cl - base;
    switch (final) {
    case 'A': move_to(y - arg(0, 1), col); break;
    case 'B': case 'e': move_to(y + arg(0, 1), col); break;
    case 'C': case 'a': move_to(y, col + arg(0, 1)); break;
    case 'D': move_to(y, col - arg(0, 1)); break;
    case 'E': move_to(y + arg(0, 1), 0); break;
    case 'F': move_to(y - arg(0, 1), 0); break;
    case 'G': case '`': move_to(y, arg(0, 1) - 1); break;
    case 'd': move_to(arg(0, 1) - 1, col); break;
    case 'H': case 'f': move_to(arg(0, 1) - 1, arg(1, 1) - 1); break;
    case 'J': {
        int a = n_args ? args[0] : 0;
        if (a == 0) { blank(cl, col, MAXC); for (int i = cl + 1; i < count; i++) blank(i, 0, MAXC); }
        else if (a == 1) { for (int i = base; i < cl; i++) blank(i, 0, MAXC); blank(cl, 0, col + 1); }
        else for (int i = base; i < count; i++) blank(i, 0, MAXC);
        break;
    }
    case 'K': {
        int a = n_args ? args[0] : 0;
        if (a == 0) blank(cl, col, MAXC);
        else if (a == 1) blank(cl, 0, col + 1);
        else blank(cl, 0, MAXC);
        break;
    }
    case 'L': if (y >= top_m && y <= region_bottom()) scroll_region(y, region_bottom(), -arg(0, 1)); break;
    case 'M': if (y >= top_m && y <= region_bottom()) scroll_region(y, region_bottom(), arg(0, 1)); break;
    case 'S': scroll_region(top_m, region_bottom(), arg(0, 1)); break;
    case 'T': scroll_region(top_m, region_bottom(), -arg(0, 1)); break;
    case '@': case 'P': {                              /* insert / delete characters */
        int n = arg(0, 1), r = ring(cl);
        if (n > MAXC - col) n = MAXC - col;
        int keep = MAXC - col - n;
        if (final == '@') {
            memmove(text[r] + col + n, text[r] + col, (size_t)keep * sizeof(text[0][0]));
            memmove(fgc[r] + col + n, fgc[r] + col, (size_t)keep);
            memmove(bgc[r] + col + n, bgc[r] + col, (size_t)keep);
            blank(cl, col, col + n);
        } else {
            memmove(text[r] + col, text[r] + col + n, (size_t)keep * sizeof(text[0][0]));
            memmove(fgc[r] + col, fgc[r] + col + n, (size_t)keep);
            memmove(bgc[r] + col, bgc[r] + col + n, (size_t)keep);
            blank(cl, MAXC - n, MAXC);
        }
        break;
    }
    case 'X': blank(cl, col, col + arg(0, 1)); break;
    case 'm': sgr(); break;
    case 'h': set_mode(1); break;
    case 'l': set_mode(0); break;
    case 'r':                                          /* the scroll region */
        top_m = arg(0, 1) - 1;
        bot_m = arg(1, rows) - 1;
        if (top_m < 0 || top_m >= rows) top_m = 0;
        if (bot_m <= top_m || bot_m >= rows) bot_m = -1;
        move_to(0, 0);
        break;
    case 's': saved_cl = y, saved_col = col; break;
    case 'u': move_to(saved_cl, saved_col); break;
    case 'n':
        if (arg(0, 0) == 6) {                          /* where is the cursor? */
            char s[32];
            snprintf(s, sizeof(s), "\x1b[%d;%dR", y + 1, col + 1);
            reply(s);
        }
        break;
    case 'c': if (!private_mode) reply("\x1b[?1;2c"); break;
    }
}

/* What the shell and its programs print, a byte at a time. */
static void put(char c) {
    unsigned char u = (unsigned char)c;
    if (esc == 1) {                                    /* after ESC */
        esc = 0;
        if (c == '[') { esc = 2; n_args = 0; args[0] = 0; private_mode = 0; }
        else if (c == ']') esc = 3;
        else if (c == '(' || c == ')' || c == '#') esc = 4;
        else if (c == '7') { saved_cl = cl - screen_top(); saved_col = col; }
        else if (c == '8') move_to(saved_cl, saved_col);
        else if (c == 'M') reverse_index();
        else if (c == 'D') line_feed();
        else if (c == 'E') { col = 0; line_feed(); }
        else if (c == 'c') { pen_fg = pen_bg = DEF; bold = reverse = 0; top_m = 0; bot_m = -1; }
        return;
    }
    if (esc == 2) {                                    /* CSI: numbers; a letter ends it */
        if (c >= '0' && c <= '9') {
            if (n_args == 0) n_args = 1;
            args[n_args - 1] = args[n_args - 1] * 10 + (c - '0');
        } else if (c == ';') {
            if (n_args == 0) n_args = 1;
            if (n_args < 16) args[n_args++] = 0;
        } else if (c == '?' || c == '>' || c == '=') {
            private_mode = 1;
        } else if (u >= 0x40 && u <= 0x7E) {
            esc = 0;
            csi(c);
        } else if (u < ' ') {
            esc = 0;
        }
        return;
    }
    if (esc == 3) {                                    /* OSC (the title): to BEL or ESC \ */
        if (c == '\a') esc = 0;
        else if (c == 0x1b) esc = 4;
        return;
    }
    if (esc == 4) { esc = 0; return; }
    if (u >= 0x80) {                                   /* UTF-8 */
        if ((u & 0xC0) == 0x80 && utf_need) {
            utf_cp = utf_cp << 6 | (u & 0x3F);
            if (--utf_need == 0) glyph(utf_cp);
        } else if ((u & 0xE0) == 0xC0) { utf_cp = u & 0x1F; utf_need = 1; }
        else if ((u & 0xF0) == 0xE0) { utf_cp = u & 0x0F; utf_need = 2; }
        else if ((u & 0xF8) == 0xF0) { utf_cp = u & 0x07; utf_need = 3; }
        else { utf_need = 0; glyph(u); }               /* (not UTF-8: as Latin-1) */
        return;
    }
    utf_need = 0;
    if (c == 0x1b) esc = 1;
    else if (c == '\r') col = 0;
    else if (c == '\n') { col = 0; line_feed(); }      /* (a new line starts at the left, as before) */
    else if (c == '\b') { if (col > 0) col--; }
    else if (c == '\t') { do glyph(' '); while (col % 8 && col < cols); }
    else if (u >= ' ' && u != 0x7F) glyph(c);
}

static void reader(void *arg) {
    char buf[512];
    for (;;) {
        long n = read(master, buf, sizeof(buf));
        if (n <= 0) exit(0);                            /* the shell has ended */
        mutex_lock(&lock);
        for (long i = 0; i < n; i++) put(buf[i]);
        dirty = 1;
        mutex_unlock(&lock);
    }
}

static int raw_mode(void) {                             /* the program reads keys, not lines */
    struct os_termios t;
    return pty_mode(master, &t) == 0 && !(t.lflag & PTY_ICANON);
}

static void fit(void) {                                 /* the view's size in characters */
    int S = win.scale;
    cols = (screen->w - 8 * S) / (8 * S);
    rows = (screen->h - 8 * S) / (16 * S);
    if (cols > MAXC) cols = MAXC;
    if (cols < 10) cols = 10;
    if (rows < 2) rows = 2;
    if (rows > LINES / 2) rows = LINES / 2;
    if (master && (cols != told_cols || rows != told_rows)) {
        told_cols = cols, told_rows = rows;
        pty_size(master, cols, rows);
    }
}

static unsigned colour(int c, unsigned dflt) {
    return c == DEF ? dflt : c == 17 ? BG : palette[c & 15];
}

/* Line i of the ring, from column 0, at y. */
static void draw_line(ui_canvas *c, int x0, int y, int i, int n) {
    int S = win.scale, cw = 8 * S, ch = 16 * S, r = ring(i);
    for (int k = 0; k < n; k++) {
        int b = bgc[r][k];
        if (b != DEF) ui_fill(c, x0 + k * cw, y, cw, ch, colour(b, BG));
        unsigned g = text[r][k];
        if (g != ' ') ui_glyph(c, x0 + k * cw, y, g, colour(fgc[r][k], FG), S);
    }
}

static void draw(view_t *v, ui_canvas *c) {
    mutex_lock(&lock);
    fit();
    int S = win.scale, cw = 8 * S, ch = 16 * S, m = 4 * S, ox = v->x, oy = v->y;
    ui_fill(c, ox, oy, v->w, v->h, BG);
    /* The screen's lines from the bottom up. In the cooked mode the typed line
     * follows the output on the cursor's line, wrapped at the window's width. */
    int typed = !secret ? line_len : 0;
    int at_end = cl == count - 1;
    int tail_rows = at_end ? (col + typed) / cols + 1 : 1;
    int total = count - 1 + tail_rows;
    int bottom = total - back;                            /* the line after the last one shown */
    int n = cols < MAXC ? cols : MAXC;
    for (int r = 0; r < rows; r++) {
        int i = bottom - rows + r, y = oy + m + r * ch;
        if (i < 0) continue;
        if (i < count - 1 || (!at_end && i < count)) {
            draw_line(c, ox + m, y, i, n);
            if (i == cl && typed) {                       /* the typed text over it (cursor moved up) */
                ui_fill(c, ox + m + col * cw, y, (cols - col) * cw, ch, BG);
                for (int k = 0; k < typed && col + k < cols; k++)
                    ui_char(c, ox + m + (col + k) * cw, y, (unsigned char)line[k], FG, S);
            }
        } else if (at_end) {                              /* the cursor's line and the typed text */
            int k = (i - (count - 1)) * cols;
            for (int x = 0; x < cols; x++, k++) {
                if (k < col) {
                    int rr = ring(count - 1);
                    if (bgc[rr][k] != DEF) ui_fill(c, ox + m + x * cw, y, cw, ch, colour(bgc[rr][k], BG));
                    if (text[rr][k] != ' ')
                        ui_glyph(c, ox + m + x * cw, y, text[rr][k], colour(fgc[rr][k], FG), S);
                } else if (k - col < typed) {
                    ui_char(c, ox + m + x * cw, y, (unsigned char)line[k - col], FG, S);
                }
            }
        }
    }
    if (has_selection || selecting) {                      /* the selection */
        long a = sel_l0, b = sel_l1;
        int ca = sel_c0, cb = sel_c1;
        if (a > b || (a == b && ca > cb)) { long t = a; a = b; b = t; int u = ca; ca = cb; cb = u; }
        for (int r = 0; r < rows; r++) {
            long abs = dropped + bottom - rows + r;
            if (abs < a || abs > b) continue;
            int from = abs == a ? ca : 0, to = abs == b ? cb : cols;
            if (to > from) ui_fill_a(c, ox + m + from * cw, oy + m + r * ch, (to - from) * cw, ch, CURSOR, 80);
        }
    }
    if (!back && cursor_on) {                              /* the cursor */
        int at = col + (secret ? 0 : cur), r;
        if (at_end) {
            r = rows - (tail_rows - at / cols);
        } else {
            r = rows - (total - cl);
            if (at >= cols) at = cols - 1;
        }
        if (r >= 0 && r < rows) {
            int x = ox + m + (at % cols) * cw, y = oy + m + r * ch;
            ui_fill(&win.c, x, y, cw, ch, CURSOR);
            unsigned under = !secret && cur < line_len ? (unsigned char)line[cur]
                           : !at_end || !line_len ? row_text(cl)[col < MAXC ? col : MAXC - 1] : ' ';
            if (under != ' ') ui_glyph(&win.c, x, y, under, BG, S);
        }
    } else if (back) {                                     /* where the view is in the scrollback */
        int track = v->h - 2 * m, bar = track * rows / total;
        if (bar < 12 * S) bar = 12 * S;
        int y = m + (track - bar) * (bottom - rows) / (total - rows > 0 ? total - rows : 1);
        ui_round(&win.c, ox + v->w - 6 * S, oy + y, 3 * S, bar, S, 0x8A8A96);
    }
    mutex_unlock(&lock);
}

/* Where the pointer is: an absolute line and a column (draw's layout). */
static void cell_at(int px, int py, long *line, int *c) {
    int S = win.scale, m = 4 * S, typed = !secret ? line_len : 0;
    int at_end = cl == count - 1, tail_rows = at_end ? (col + typed) / cols + 1 : 1;
    int bottom = count - 1 + tail_rows - back, r = (py - m) / (16 * S);
    if (r < 0) r = 0;
    if (r >= rows) r = rows - 1;
    long i = bottom - rows + r;
    if (i < 0) i = 0;
    if (i > count - 1) i = count - 1;
    *line = dropped + i;
    *c = (px - m + 4 * S) / (8 * S);
    if (*c < 0) *c = 0;
    if (*c > cols) *c = cols;
}

/* The selected text, UTF-8, lines without their trailing spaces, to the clipboard. */
static void copy_selection(void) {
    long a = sel_l0, b = sel_l1;
    int ca = sel_c0, cb = sel_c1;
    if (a > b || (a == b && ca > cb)) { long t = a; a = b; b = t; int u = ca; ca = cb; cb = u; }
    static char out[WS_CLIP_MAX];
    int n = 0;
    for (long l = a; l <= b && n < WS_CLIP_MAX - 8; l++) {
        long i = l - dropped;
        if (i < 0 || i >= count) continue;
        int from = l == a ? ca : 0, to = l == b ? cb : MAXC, end = to;
        while (end > from && row_text((int)i)[end - 1] == ' ') end--;
        for (int k = from; k < end && n < WS_CLIP_MAX - 8; k++) n += ui_utf8_put(out + n, row_text((int)i)[k]);
        if (l < b) out[n++] = '\n';
    }
    if (n) ui_clip_set(out, n);
}

/* Paste: typed into the line (cooked), or sent as keys (raw). */
static void paste(int raw) {
    static char clip[4096];
    int n = ui_clip_get(clip, sizeof(clip));
    if (raw) {
        write(master, clip, (size_t)n);
        return;
    }
    for (int i = 0; i < n && line_len < (int)sizeof(line) - 1; i++) {
        unsigned char ch = (unsigned char)clip[i];
        if (ch == '\n' || ch == '\r') break;           /* (the first line) */
        if (ch < ' ') continue;
        memmove(line + cur + 1, line + cur, (size_t)(line_len - cur));
        line[cur++] = (char)ch;
        line_len++;
    }
}

static void scroll_by(int n) {
    int max = count - 1 + (col + line_len) / cols + 1 - rows;
    back += n;
    if (back > max) back = max;
    if (back < 0) back = 0;
}

static void send_line(const char *s) {
    write(master, s, strlen(s));
    write(master, "\n", 1);
}

/* Ctrl+C in the cooked mode: interrupt what the shell runs, and what that
 * runs (bash's commands too) -> how many. */
static int interrupt(void) {
    static int pid[256], parent[256];
    struct os_task t;
    int n = 0, hit = 0;
    for (int i = 0, used; (used = taskinfo(i, &t)) >= 0 && n < 256; i++)
        if (used && t.pid > 0) pid[n] = t.pid, parent[n++] = t.parent;
    for (int i = 0; i < n; i++) {
        if (pid[i] == shell_pid) continue;
        int p = parent[i];
        for (int hops = 0; p && p != shell_pid && hops < 16; hops++) {
            int up = 0;
            for (int j = 0; j < n; j++)
                if (pid[j] == p) up = parent[j];
            p = up;
        }
        if (p == shell_pid && sigsend(pid[i], SIGINT) == 0) hit++;
    }
    return hit;
}

static int start_shell(void) {
    int fds[2];
    if (pty(fds) < 0) return -1;
    int saved[3] = { dup(0), dup(1), dup(2) };
    for (int i = 0; i < 3; i++) dup2(fds[1], i);
    char *argv[] = { "shell", "-t", 0 };
    int pid = shell_pid = spawn("shell", argv, 0);
    for (int i = 0; i < 3; i++) {
        dup2(saved[i], i);
        close(saved[i]);
    }
    close(fds[1]);
    master = fds[0];
    return pid;
}

static void set_line(const char *s) {
    snprintf(line, sizeof(line), "%s", s);
    line_len = cur = (int)strlen(line);
}

/* A key for a program in raw mode: as a terminal sends it. */
static void raw_key(int k) {
    char s[4] = { 0 };
    if (k == KEYCODE_UP || k == KEYCODE_DOWN || k == KEYCODE_RIGHT || k == KEYCODE_LEFT) {
        s[0] = 0x1b;
        s[1] = app_keys ? 'O' : '[';
        s[2] = "ABCD"[k - KEYCODE_UP];
        write(master, s, 3);
    } else if (k == '\n' || k == '\r') {
        write(master, "\r", 1);
    } else if (k == '\b') {
        write(master, "\x7f", 1);
    } else if (k > 0 && k < 256) {
        s[0] = (char)k;
        write(master, s, 1);
    }
}

static void key(int k) {
    if (k == KEY_SCROLL_UP || k == KEY_SCROLL_DOWN) {
        scroll_by(k == KEY_SCROLL_UP ? 1 : -1);
        return;
    }
    back = 0;                                           /* typing: back to the bottom */
    if (raw_mode()) {
        raw_key(k);
        return;
    }
    if (k == 3) {                                       /* Ctrl+C */
        put('^');
        put('C');
        put('\n');
        line_len = cur = secret = 0;
        recall = -1;
        if (pty_signal(master, SIGINT) > 0) return;     /* the job in front (bash gave it the terminal) */
        if (interrupt() == 0) send_line("\x03");       /* a kernel command's question, or a new prompt */
    } else if (k == 26) {                               /* Ctrl+Z: stop the job in front (bash: fg, bg) */
        if (pty_signal(master, SIGTSTP) > 0) {
            put('^');
            put('Z');
            put('\n');
            line_len = cur = secret = 0;
            recall = -1;
        }
    } else if (k == 4 && !line_len) {                   /* Ctrl+D: the end of what is typed */
        write(master, "\x04", 1);
    } else if (k == 22) {                               /* Ctrl+V */
        paste(0);
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
            const unsigned short *now = row_text(cl);
            secret = col >= 9;
            for (int i = 0; secret && i < 9; i++) secret = now[col - 9 + i] == (unsigned char)"assword: "[i];
        }
        memmove(line + cur + 1, line + cur, (size_t)(line_len - cur));
        line[cur++] = (char)k;
        line_len++;
    }
}

/* Keys and the wheel, from the view. */
static int screen_event(view_t *v, const struct ws_event *e) {
    mutex_lock(&lock);
    if (e->type == WS_EV_KEY) {
        key(e->key);
    } else if (e->type == WS_EV_WHEEL) {
        scroll_by(-e->key * 3);
    } else if (e->type == WS_EV_DOWN && (e->buttons & 2)) {   /* a right click: paste */
        paste(raw_mode());
    } else if (e->type == WS_EV_DOWN) {
        cell_at(e->x, e->y, &sel_l0, &sel_c0);
        sel_l1 = sel_l0, sel_c1 = sel_c0;
        selecting = 1;
        has_selection = 0;
    } else if (e->type == WS_EV_MOVE && selecting) {
        cell_at(e->x, e->y, &sel_l1, &sel_c1);
    } else if (e->type == WS_EV_UP && selecting) {
        selecting = 0;
        has_selection = sel_l0 != sel_l1 || sel_c0 != sel_c1;
        if (has_selection) copy_selection();
    } else {
        mutex_unlock(&lock);
        return 0;
    }
    mutex_unlock(&lock);
    v_dirty(v);
    return 1;
}

static void tick(void) {                                /* the reader printed: show it */
    if (!dirty) return;
    dirty = 0;
    v_dirty(screen);
}

int main(int argc, char **argv) {
    int S = ui_scale();
    if (ui_open(&win, "Terminal", 80 * 8 * S + 8 * S, 25 * 16 * S + 8 * S) < 0) {
        printf("terminal: the desktop is not running (type windowserver)\n");
        return 1;
    }
    ui_set_resizable(&win);
    view_t *root = v_box(0, V_COLUMN);
    root->pad = 0;
    screen = v_custom(root, draw, screen_event);
    screen->focusable = 1;
    v_layout(&win, root);
    spaces(text[0], MAXC);
    memset(fgc[0], DEF, MAXC);
    memset(bgc[0], DEF, MAXC);
    if (start_shell() < 0) {
        printf("terminal: cannot start the shell\n");
        return 1;
    }
    fit();
    thread_create(reader, 0);
    if (argc > 1) {
        char cmd[256] = "";
        for (int i = 1; i < argc; i++) {
            if (i > 1) strcat(cmd, " ");
            if (strlen(cmd) + strlen(argv[i]) + 2 < sizeof(cmd)) strcat(cmd, argv[i]);
        }
        send_line(cmd);
    }
    return v_run(&win, root, 15, tick);
}
