// textedit [file] - TextEdit: write and edit text files (UTF-8).
//
// Arrows, Home, End, Page Up and Page Down move (with Shift they select;
// Ctrl+Left/Right go a word, Ctrl+Home/End to the start or the end), Delete
// deletes forward; clicking places the cursor and dragging selects. Ctrl+A
// selects everything; Ctrl+C, Ctrl+X and Ctrl+V copy, cut and paste (the
// clipboard every program shares); Ctrl+Z undoes and Ctrl+Y redoes; Ctrl+F
// goes to the Find field and Ctrl+G finds the next match; Ctrl+S saves. The
// name field opens another file.
//
// Built from views (view.h): a toolbar (the file's name, Open, Save, Find),
// the text (a custom view: it draws the characters and takes the keys) and a
// status line.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <os.h>
#include <view.h>

#define MAX_TEXT 65536
#define UNDO 32
#define KEY_SELECT_ALL 1                               /* Ctrl+A */
#define KEY_COPY 3
#define KEY_FIND 6                                     /* Ctrl+F */
#define KEY_FIND_NEXT 7                                /* Ctrl+G */
#define KEY_SAVE 19                                    /* Ctrl+S */
#define KEY_PASTE 22
#define KEY_CUT 24
#define KEY_REDO 25                                    /* Ctrl+Y */
#define KEY_UNDO 26                                    /* Ctrl+Z */

static int S, CW, CH;
static char text[MAX_TEXT];
static int len, cur, anchor = -1, first_line, changed, dragging;
static view_t *name, *save_btn, *editor, *info, *find;
static char status[96];

/* Undo: whole copies of the text before each change (typing in a row is one). */
typedef struct { char *text; int len, cur; } snap_t;
static snap_t undo_stack[UNDO], redo_stack[UNDO];
static int n_undo, n_redo, last_kind;                  /* last_kind: the change before (1 typing) */

static int is_cont(int i) { return i > 0 && i < len && ((unsigned char)text[i] & 0xC0) == 0x80; }
static int prev_char(int i) { if (i > 0) i--; while (is_cont(i)) i--; return i; }
static int next_char(int i) { if (i < len) i++; while (is_cont(i)) i++; return i; }
static int sel_lo(void) { return anchor < 0 ? cur : anchor < cur ? anchor : cur; }
static int sel_hi(void) { return anchor < 0 ? cur : anchor > cur ? anchor : cur; }
static int has_sel(void) { return anchor >= 0 && anchor != cur; }

static void show_status(void) {
    int line = 0, col = 0;
    for (int i = 0; i < cur; i = next_char(i)) {
        if (text[i] == '\n') {
            line++;
            col = 0;
        } else {
            col++;
        }
    }
    char s[160], sel[32] = "";
    if (has_sel()) {
        int n = 0;
        for (int i = sel_lo(); i < sel_hi(); i = next_char(i)) n++;
        snprintf(sel, sizeof(sel), "%d selected    ", n);
    }
    snprintf(s, sizeof(s), "Line %d, column %d    %s%s", line + 1, col + 1, sel, status);
    v_set_text(info, s);
    v_set_text(save_btn, changed ? "Save*" : "Save");
}

static void free_snaps(snap_t *st, int *n) {
    for (int i = 0; i < *n; i++) free(st[i].text);
    *n = 0;
}

static void push(snap_t *st, int *n) {
    if (*n == UNDO) {                                  /* the oldest goes */
        free(st[0].text);
        memmove(st, st + 1, (UNDO - 1) * sizeof(snap_t));
        (*n)--;
    }
    char *copy = malloc((size_t)len + 1);
    if (!copy) return;
    memcpy(copy, text, (size_t)len);
    st[*n].text = copy;
    st[*n].len = len;
    st[*n].cur = cur;
    (*n)++;
}

/* Before a change of `kind` (1: typing, 2: anything else). */
static void will_change(int kind) {
    if (kind != 1 || last_kind != 1) push(undo_stack, &n_undo);
    last_kind = kind;
    free_snaps(redo_stack, &n_redo);
    changed = 1;
}

static void restore(snap_t *from, int *nfrom, snap_t *to, int *nto) {
    if (!*nfrom) return;
    push(to, nto);
    snap_t *s = &from[--*nfrom];
    memcpy(text, s->text, (size_t)s->len);
    len = s->len;
    cur = s->cur <= len ? s->cur : len;
    anchor = -1;
    free(s->text);
    changed = 1;
    last_kind = 0;
}

static void load(void) {
    len = cur = first_line = 0;
    anchor = -1;
    free_snaps(undo_stack, &n_undo);
    free_snaps(redo_stack, &n_redo);
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

/* Line and column (in characters) of byte offset `at`, and back. */
static void where(int at, int *line, int *col) {
    *line = *col = 0;
    for (int i = 0; i < at; i = next_char(i)) {
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
    for (int c = 0; c < col && i < len && text[i] != '\n'; c++) i = next_char(i);
    return i;
}

static int is_word(int i) {
    unsigned char c = (unsigned char)text[i];
    return c >= 0x80 || c == '_' || (c >= '0' && c <= '9') || ((c | 0x20) >= 'a' && (c | 0x20) <= 'z');
}
static int word_left(int i) {
    while (i > 0 && !is_word(prev_char(i))) i = prev_char(i);
    while (i > 0 && is_word(prev_char(i))) i = prev_char(i);
    return i;
}
static int word_right(int i) {
    while (i < len && !is_word(i)) i = next_char(i);
    while (i < len && is_word(i)) i = next_char(i);
    return i;
}
static int editor_rows(void);

static void delete_range(int lo, int hi) {
    memmove(text + lo, text + hi, (size_t)(len - hi));
    len -= hi - lo;
    cur = lo;
    anchor = -1;
}

static void delete_selection(void) {
    if (has_sel()) delete_range(sel_lo(), sel_hi());
    anchor = -1;
}

static void insert_bytes(const char *s, int n) {
    if (len + n > MAX_TEXT - 1) n = MAX_TEXT - 1 - len;
    if (n <= 0) return;
    memmove(text + cur + n, text + cur, (size_t)(len - cur));
    memcpy(text + cur, s, (size_t)n);
    cur += n;
    len += n;
}

static void type_char(char ch) {
    will_change(1);
    delete_selection();
    insert_bytes(&ch, 1);
}

static void erase(void) {
    if (has_sel()) {
        will_change(2);
        delete_selection();
    } else if (cur) {
        will_change(1);
        delete_range(prev_char(cur), cur);
    }
}

static void copy_selection(int cut) {
    if (!has_sel()) return;
    ui_clip_set(text + sel_lo(), sel_hi() - sel_lo());
    if (cut) {
        will_change(2);
        delete_selection();
    }
    snprintf(status, sizeof(status), cut ? "Cut" : "Copied");
}

static void paste(void) {
    static char clip[MAX_TEXT];
    int n = ui_clip_get(clip, sizeof(clip));
    if (!n) return;
    will_change(2);
    delete_selection();
    int k = 0;
    for (int i = 0; i < n; i++)
        if (clip[i] != '\r') clip[k++] = clip[i];
    insert_bytes(clip, k);
}

/* The next place the Find field's text is, after the cursor (round to the top). */
static void find_next(void) {
    const char *q = find->text;
    int n = (int)strlen(q);
    if (!n) return;
    for (int pass = 0; pass < 2; pass++) {
        int from = pass ? 0 : (has_sel() ? sel_hi() : cur);
        for (int i = from; i + n <= len; i++)
            if (!memcmp(text + i, q, (size_t)n)) {
                anchor = i;
                cur = i + n;
                snprintf(status, sizeof(status), pass ? "Found (from the top)" : "Found");
                v_dirty(editor);
                show_status();
                return;
            }
    }
    snprintf(status, sizeof(status), "\"%s\" is not in the text", q);
    show_status();
}

static int editor_rows(void) {
    int rows = editor && CH ? (editor->h - 8 * S) / CH : 10;
    return rows > 1 ? rows - 1 : 1;
}

static void draw_text(view_t *v, ui_canvas *c) {
    ui_fill(c, v->x, v->y, v->w, v->h, ui_color(UI_C_CONTENT));
    int rows = (v->h - 8 * S) / CH, line, col;
    where(cur, &line, &col);
    if (line < first_line) first_line = line;
    if (line >= first_line + rows) first_line = line - rows + 1;
    int l = 0, x = v->x + 12 * S, y = v->y + 8 * S, lo = sel_lo(), hi = sel_hi(), sel = has_sel();
    unsigned fg = ui_color(UI_C_LABEL), accent = ui_color(UI_C_ACCENT);
    for (int i = 0; i <= len;) {
        int shown = l >= first_line && l < first_line + rows, yy = y + (l - first_line) * CH;
        if (shown && i == cur) ui_fill(c, x, yy + S, S + 1, CH - 2 * S, accent);
        if (i == len) break;
        if (text[i] == '\n') {
            if (shown && sel && i >= lo && i < hi) ui_fill_a(c, x, yy, CW / 2, CH, accent, 90);
            l++;
            x = v->x + 12 * S;
            i++;
            continue;
        }
        const char *p = text + i;
        unsigned cp = ui_utf8(&p);
        if (shown) {
            if (sel && i >= lo && i < hi) ui_fill_a(c, x, yy, CW, CH, accent, 90);
            if (cp == '\t') cp = ' ';
            ui_glyph(c, x, yy + S, cp, fg, S);
        }
        x += CW;
        i = (int)(p - text);
    }
}

static int text_event(view_t *v, const struct ws_event *e) {
    int line, col;
    if (e->type == WS_EV_DOWN || (e->type == WS_EV_MOVE && dragging)) {
        int at = offset_of(first_line + (e->y - 8 * S) / CH, (e->x - 12 * S + CW / 2) / CW);
        if (e->type == WS_EV_DOWN) {
            anchor = at;
            dragging = 1;
        }
        cur = at;
        last_kind = 0;
    } else if (e->type == WS_EV_UP) {
        dragging = 0;
        if (anchor == cur) anchor = -1;
    } else if (e->type == WS_EV_WHEEL) {
        first_line += e->key * 3;
        if (first_line < 0) first_line = 0;
    } else if (e->type == WS_EV_KEY) {
        int k = e->key;
        status[0] = 0;
        where(cur, &line, &col);
        int base = KEYCODE_BASE(k), shift = k & KEYMOD_SHIFT, ctrl = k & KEYMOD_CTRL;
        if (base >= KEYCODE_UP && base <= KEYCODE_PGDN) {
            int had = has_sel(), lo = sel_lo(), hi = sel_hi(), rows = editor_rows();
            if (shift && anchor < 0) anchor = cur;              /* Shift: the selection grows */
            last_kind = 0;
            if (base == KEYCODE_LEFT) cur = had && !shift ? lo : ctrl ? word_left(cur) : prev_char(cur);
            else if (base == KEYCODE_RIGHT) cur = had && !shift ? hi : ctrl ? word_right(cur) : next_char(cur);
            else if (base == KEYCODE_UP) cur = line ? offset_of(line - 1, col) : 0;
            else if (base == KEYCODE_DOWN) cur = offset_of(line + 1, col);
            else if (base == KEYCODE_HOME) cur = ctrl ? 0 : offset_of(line, 0);
            else if (base == KEYCODE_END) cur = ctrl ? len : offset_of(line, MAX_TEXT);
            else if (base == KEYCODE_PGUP) cur = offset_of(line > rows ? line - rows : 0, col);
            else cur = offset_of(line + rows, col);
            if (!shift) anchor = -1;
        }
        else if (base == KEYCODE_DELETE) {                       /* forward: the selection, or the next character */
            if (has_sel()) erase();
            else if (cur < len) {
                cur = next_char(cur);
                erase();
            }
        }
        else if (k == KEY_SAVE) save();
        else if (k == KEY_SELECT_ALL) { anchor = 0; cur = len; }
        else if (k == KEY_COPY) copy_selection(0);
        else if (k == KEY_CUT) copy_selection(1);
        else if (k == KEY_PASTE) paste();
        else if (k == KEY_UNDO) restore(undo_stack, &n_undo, redo_stack, &n_redo);
        else if (k == KEY_REDO) restore(redo_stack, &n_redo, undo_stack, &n_undo);
        else if (k == KEY_FIND) { v_focus(find); return 1; }
        else if (k == KEY_FIND_NEXT) find_next();
        else if (k == '\b') erase();
        else if (k == '\r' || k == '\n') type_char('\n');
        else if (k == '\t') { for (int i = 0; i < 4; i++) type_char(' '); }
        else if (k == 27) anchor = -1;
        else if (k >= ' ' && k < 127) type_char((char)k);
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

static void find_entered(view_t *f) {
    (void)f;
    find_next();
    v_focus(editor);
}

static int window_key(view_t *v, int k) {             /* Ctrl+S, Ctrl+G from the fields too */
    (void)v;
    if (k == KEY_SAVE) save();
    else if (k == KEY_FIND_NEXT) find_next();
    else if (k == KEY_FIND) v_focus(find);
    else return 0;
    return 1;
}

/* Closing with changes not saved: the first time it says so (Ctrl+S saves);
 * closed again within 5 seconds, the changes go. */
static int may_close(void) {
    static unsigned long warned;
    unsigned long now = uptime_ms();
    if (!changed || (warned && now - warned < 5000)) return 1;
    warned = now;
    snprintf(status, sizeof(status), "Not saved: Ctrl+S saves it; close again to throw the changes away");
    show_status();
    v_dirty(info);
    return 0;
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
    find = v_field(bar, "", find_entered);
    find->want_w = 160;
    snprintf(find->hint, sizeof(find->hint), "Find");
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
    v_on_close(may_close);
    return v_run(&win, root, 0, 0);
}
