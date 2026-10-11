// textedit [file] - TextEdit: write and edit text files. Arrows move,
// clicking places the cursor, Ctrl+S saves; the name field opens another.
#include <stdio.h>
#include <string.h>
#include <os.h>
#include <ui.h>

#define MAX_TEXT 65536
#define KEY_SAVE 19                                   /* Ctrl+S */

static ui_window win;
static int S, CW, CH, TOP;
static char text[MAX_TEXT], path[128], status[96];
static int len, cur, first_line, editing_name, dirty;

static void load(void) {
    len = cur = first_line = 0;
    int fd = open(path, OPEN_READ);
    if (fd < 0) {
        snprintf(status, sizeof(status), "New file");
        return;
    }
    long n = read(fd, text, MAX_TEXT - 1);
    close(fd);
    len = n > 0 ? (int)n : 0;
    int k = 0;                                         /* drop the carriage returns */
    for (int i = 0; i < len; i++)
        if (text[i] != '\r') text[k++] = text[i];
    len = k;
    snprintf(status, sizeof(status), "%d bytes", len);
    dirty = 0;
}

static void save(void) {
    int fd = open(path, OPEN_WRITE);
    if (fd < 0) {
        snprintf(status, sizeof(status), "Cannot save: %s", os_strerror(fd));
        return;
    }
    long n = write(fd, text, (size_t)len);
    close(fd);
    snprintf(status, sizeof(status), n == len ? "Saved %d bytes" : "Save failed", len);
    dirty = 0;
}

/* The line and column of offset `at`. */
static void where(int at, int *line, int *col) {
    *line = *col = 0;
    for (int i = 0; i < at; i++) {
        if (text[i] == '\n') {
            (*line)++;
            *col = 0;
        } else {
            (*col)++;
        }
    }
}

static int offset_of(int line, int col) {
    int i = 0, l = 0;
    while (i < len && l < line) if (text[i++] == '\n') l++;
    for (int c = 0; c < col && i < len && text[i] != '\n'; c++) i++;
    return i;
}

static void insert(char ch) {
    if (len >= MAX_TEXT - 1) return;
    memmove(text + cur + 1, text + cur, (size_t)(len - cur));
    text[cur++] = ch;
    len++;
    dirty = 1;
}

static void erase(void) {
    if (!cur) return;
    memmove(text + cur - 1, text + cur, (size_t)(len - cur));
    cur--;
    len--;
    dirty = 1;
}

static void draw(void) {
    ui_canvas *c = &win.c;
    ui_fill(c, 0, 0, c->w, c->h, UI_WHITE);
    ui_fill(c, 0, 0, c->w, TOP, 0xF2F2F6);
    ui_fill(c, 0, TOP - 1, c->w, 1, 0xDADAE0);
    ui_textfield(c, 10 * S, 8 * S, c->w - 210 * S, 28 * S, path, S, editing_name);
    ui_button(c, c->w - 190 * S, 8 * S, 84 * S, 28 * S, "Open", S, 0);
    ui_button(c, c->w - 96 * S, 8 * S, 84 * S, 28 * S, dirty ? "Save*" : "Save", S, 0);
    int rows = (c->h - TOP - 28 * S) / CH, line, col;
    where(cur, &line, &col);
    if (line < first_line) first_line = line;
    if (line >= first_line + rows) first_line = line - rows + 1;
    int l = 0, x = 12 * S, y = TOP + 8 * S;
    for (int i = 0; i <= len; i++) {
        if (l >= first_line && l < first_line + rows) {
            int yy = y + (l - first_line) * CH;
            if (i == cur && !editing_name) ui_fill(c, x, yy + S, S + 1, CH - 2 * S, UI_ACCENT);
            if (i < len && text[i] != '\n') x += ui_char(c, x, yy, (unsigned char)text[i], UI_TEXT, S);
        }
        if (i < len && text[i] == '\n') {
            l++;
            x = 12 * S;
        }
    }
    char info[128];
    snprintf(info, sizeof(info), "Line %d, column %d    %s", line + 1, col + 1, status);
    ui_fill(c, 0, c->h - 24 * S, c->w, 24 * S, 0xF6F6F9);
    ui_text(c, 12 * S, c->h - 20 * S, info, UI_DIM, S);
    ui_present(&win);
}

static void key(int k) {
    int line, col;
    if (editing_name) {
        int n = (int)strlen(path);
        if (k == '\n' || k == '\r') {
            editing_name = 0;
            load();
        } else if (k == '\b' && n) {
            path[n - 1] = 0;
        } else if (k >= ' ' && k < 127 && n < (int)sizeof(path) - 1) {
            path[n] = (char)k;
            path[n + 1] = 0;
        }
        return;
    }
    where(cur, &line, &col);
    if (k == KEY_SAVE) save();
    else if (k == KEYCODE_LEFT) { if (cur) cur--; }
    else if (k == KEYCODE_RIGHT) { if (cur < len) cur++; }
    else if (k == KEYCODE_UP) { if (line) cur = offset_of(line - 1, col); }
    else if (k == KEYCODE_DOWN) cur = offset_of(line + 1, col);
    else if (k == '\b') erase();
    else if (k == '\r' || k == '\n') insert('\n');
    else if (k == '\t') { for (int i = 0; i < 4; i++) insert(' '); }
    else if (k >= ' ' && k < 127) insert((char)k);
}

int main(int argc, char **argv) {
    S = ui_scale();
    CW = 8 * S;
    CH = 18 * S;
    TOP = 44 * S;
    snprintf(path, sizeof(path), "%s", argc > 1 ? argv[1] : "notes.txt");
    if (ui_open(&win, "TextEdit", 640 * S, 460 * S) < 0) {
        printf("textedit: the desktop is not running\n");
        return 1;
    }
    load();
    draw();
    for (;;) {
        struct ws_event e;
        ui_wait(&win, &e, 0);
        if (e.type == WS_EV_CLOSE) return 0;
        if (e.type == WS_EV_KEY) key(e.key);
        else if (e.type == WS_EV_DOWN) {
            if (ui_inside(e.x, e.y, win.c.w - 190 * S, 8 * S, 84 * S, 28 * S)) {
                editing_name = 0;
                load();
            } else if (ui_inside(e.x, e.y, win.c.w - 96 * S, 8 * S, 84 * S, 28 * S)) {
                save();
            } else if (e.y < TOP) {
                editing_name = 1;
            } else {
                editing_name = 0;
                cur = offset_of(first_line + (e.y - TOP - 8 * S) / CH, (e.x - 12 * S + CW / 2) / CW);
            }
        } else if (e.type == WS_EV_WHEEL) {
            first_line += e.key * 3;
            if (first_line < 0) first_line = 0;
        } else {
            continue;
        }
        draw();
    }
}
