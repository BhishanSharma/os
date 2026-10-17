// textedit [file] - TextEdit: write and edit text files. Arrows move,
// clicking places the cursor, Ctrl+S saves; the name field opens another.
//
// Built from views (view.h): a toolbar (the file's name, Open, Save), the
// text (a custom view: it draws the characters and takes the keys) and a
// status line.
#include <stdio.h>
#include <string.h>
#include <os.h>
#include <view.h>

#define MAX_TEXT 65536
#define KEY_SAVE 19                                   /* Ctrl+S */

static int S, CW, CH;
static char text[MAX_TEXT];
static int len, cur, first_line, changed;
static view_t *name, *save_btn, *editor, *info;
static char status[96];

static void show_status(void) {
    int line = 0, col = 0;
    for (int i = 0; i < cur; i++) {
        if (text[i] == '\n') {
            line++;
            col = 0;
        } else {
            col++;
        }
    }
    char s[128];
    snprintf(s, sizeof(s), "Line %d, column %d    %s", line + 1, col + 1, status);
    v_set_text(info, s);
    v_set_text(save_btn, changed ? "Save*" : "Save");
}

static void load(void) {
    len = cur = first_line = 0;
    int fd = open(name->text, OPEN_READ);
    if (fd < 0) {
        snprintf(status, sizeof(status), "New file");
    } else {
        long n = read(fd, text, MAX_TEXT - 1);
        close(fd);
        len = n > 0 ? (int)n : 0;
        int k = 0;                                     /* drop the carriage returns */
        for (int i = 0; i < len; i++)
            if (text[i] != '\r') text[k++] = text[i];
        len = k;
        snprintf(status, sizeof(status), "%d bytes", len);
    }
    changed = 0;
    v_dirty(editor);
    show_status();
}

static void save(void) {
    int fd = open(name->text, OPEN_WRITE);
    if (fd < 0) {
        snprintf(status, sizeof(status), "Cannot save: %s", os_strerror(fd));
    } else {
        long n = write(fd, text, (size_t)len);
        close(fd);
        snprintf(status, sizeof(status), n == len ? "Saved %d bytes" : "Save failed", len);
        changed = 0;
    }
    show_status();
}

static void open_clicked(view_t *b) {
    (void)b;
    load();
    v_focus(editor);
}

static void save_clicked(view_t *b) {
    (void)b;
    save();
}

/* ---- The text ---------------------------------------------------------------- */

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
    changed = 1;
}

static void erase(void) {
    if (!cur) return;
    memmove(text + cur - 1, text + cur, (size_t)(len - cur));
    cur--;
    len--;
    changed = 1;
}

static void draw_text(view_t *v, ui_canvas *c) {
    ui_fill(c, v->x, v->y, v->w, v->h, ui_color(UI_C_CONTENT));
    int rows = (v->h - 8 * S) / CH, line, col;
    where(cur, &line, &col);
    if (line < first_line) first_line = line;
    if (line >= first_line + rows) first_line = line - rows + 1;
    int l = 0, x = v->x + 12 * S, y = v->y + 8 * S, focused = 1;
    for (int i = 0; i <= len; i++) {
        if (l >= first_line && l < first_line + rows) {
            int yy = y + (l - first_line) * CH;
            if (i == cur && focused) ui_fill(c, x, yy + S, S + 1, CH - 2 * S, ui_color(UI_C_ACCENT));
            if (i < len && text[i] != '\n') x += ui_char(c, x, yy, (unsigned char)text[i], ui_color(UI_C_LABEL), S);
        }
        if (i < len && text[i] == '\n') {
            l++;
            x = v->x + 12 * S;
        }
    }
}

static int text_event(view_t *v, const struct ws_event *e) {
    int line, col;
    if (e->type == WS_EV_DOWN) {
        cur = offset_of(first_line + (e->y - 8 * S) / CH, (e->x - 12 * S + CW / 2) / CW);
    } else if (e->type == WS_EV_WHEEL) {
        first_line += e->key * 3;
        if (first_line < 0) first_line = 0;
    } else if (e->type == WS_EV_KEY) {
        int k = e->key;
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
        else return 0;
    } else {
        return 0;
    }
    v_dirty(v);
    show_status();
    return 1;
}

static void name_entered(view_t *f) {
    (void)f;
    load();
    v_focus(editor);
}

static int window_key(view_t *v, int k) {             /* Ctrl+S from the name field too */
    (void)v;
    if (k != KEY_SAVE) return 0;
    save();
    return 1;
}

int main(int argc, char **argv) {
    S = ui_scale();
    CW = 8 * S;
    CH = 18 * S;
    ui_window win;
    if (ui_open(&win, "TextEdit", 640 * S, 460 * S) < 0) {
        printf("textedit: the desktop is not running\n");
        return 1;
    }
    ui_set_resizable(&win);

    view_t *root = v_box(0, V_COLUMN);
    root->pad = root->gap = 0;
    root->on_key = window_key;
    view_t *bar = v_box(root, V_ROW);
    bar->pad = 8;
    bar->bg = UI_C_TOOLBAR;
    bar->align = V_CENTER;
    name = v_field(bar, argc > 1 ? argv[1] : "notes.txt", name_entered);
    name->flex = 1;
    v_button(bar, "Open", open_clicked);
    save_btn = v_button(bar, "Save", save_clicked);
    editor = v_custom(root, draw_text, text_event);
    editor->focusable = 1;
    view_t *foot = v_box(root, V_ROW);
    foot->pad = 4;
    foot->bg = UI_C_ROW_ALT;
    info = v_label(foot, "");
    info->color = UI_C_SECONDARY;
    info->flex = 1;

    load();
    v_focus(editor);
    return v_run(&win, root, 0, 0);
}
