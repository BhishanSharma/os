// view.c - views: a window as a tree of components (see view.h)
//
// A pass: lay out (if the tree or the window changed), then draw the views
// marked dirty and nothing else. A box that is drawn paints its background,
// so everything inside it is drawn again with it; a view that is not dirty is
// left alone. A table compares each visible row's text with what it showed
// and draws only the rows that differ. ui_present then sends the server just
// the rectangles drawn (ui.c records them).
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <os.h>
#include <view.h>

static ui_window *win;
static view_t *the_root;                          /* v_run's */
static int S = 1, quit_code = -1, relayout = 1;
static view_t *hot, *pressed, *focus, *grab;      /* under the pointer, button held, keys, dragged */
static unsigned long last_click_ms;
static int last_click_row = -1;

/* ---- Building ------------------------------------------------------------ */

static view_t *make(view_t *parent, int kind) {
    view_t *v = calloc(1, sizeof(view_t));
    if (!v) return 0;
    v->kind = kind;
    v->bg = -1;
    v->color = UI_C_LABEL;
    v->scale = 1;
    v->selected = -1;
    v->dirty = 1;
    if (parent) {
        v->parent = parent;
        if (parent->last) parent->last->next = v;
        else parent->first = v;
        parent->last = v;
    }
    relayout = 1;
    return v;
}

view_t *v_box(view_t *parent, int dir) {
    view_t *v = make(parent, V_BOX);
    if (!v) return 0;
    v->dir = dir;
    v->gap = 8;
    v->align = V_STRETCH;
    if (!parent) {                                     /* the window: its margins and background */
        v->pad = 16;
        v->bg = UI_C_WINDOW;
    }
    return v;
}

static view_t *with_text(view_t *v, const char *text) {
    if (v && text) snprintf(v->text, sizeof(v->text), "%s", text);
    return v;
}

view_t *v_label(view_t *parent, const char *text) { return with_text(make(parent, V_LABEL), text); }
view_t *v_text(view_t *parent, const char *text) { return with_text(make(parent, V_TEXT), text); }

view_t *v_button(view_t *parent, const char *label, void (*on_click)(view_t *v)) {
    view_t *v = with_text(make(parent, V_BUTTON), label);
    if (v) v->on_click = on_click;
    return v;
}

view_t *v_icon(view_t *parent, const char *name, int size) {
    view_t *v = with_text(make(parent, V_ICON), name);
    if (v) v->want_w = v->want_h = size;
    return v;
}

view_t *v_spacer(view_t *parent) {
    view_t *v = make(parent, V_BOX);
    if (v) v->flex = 1;
    return v;
}

view_t *v_table(view_t *parent, int cols, const char *const *heads, const int *widths, v_cell_fn cell) {
    view_t *v = make(parent, V_TABLE);
    if (!v) return 0;
    v->cols = cols > V_MAX_COLS ? V_MAX_COLS : cols;
    for (int i = 0; i < v->cols; i++) {
        v->heads[i] = heads ? heads[i] : 0;
        v->col_w[i] = widths ? widths[i] : 100;
    }
    v->cell = cell;
    v->row_h = 24;
    v->flex = 1;
    v->bg = UI_C_CONTENT;
    return v;
}

view_t *v_field(view_t *parent, const char *text, void (*on_enter)(view_t *v)) {
    view_t *v = with_text(make(parent, V_FIELD), text);
    if (v) v->on_enter = on_enter;
    return v;
}

view_t *v_slider(view_t *parent, int value, void (*on_change)(view_t *v)) {
    view_t *v = make(parent, V_SLIDER);
    if (!v) return 0;
    v->value = value;
    v->on_change = on_change;
    return v;
}

view_t *v_custom(view_t *parent, void (*draw)(view_t *v, ui_canvas *c),
                 int (*event)(view_t *v, const struct ws_event *e)) {
    view_t *v = make(parent, V_CUSTOM);
    if (!v) return 0;
    v->draw = draw;
    v->event = event;
    v->flex = 1;
    return v;
}

static void forget(view_t *v) {                        /* the toolkit's pointers to a view going away */
    if (hot == v) hot = 0;
    if (pressed == v) pressed = 0;
    if (focus == v) focus = 0;
    if (grab == v) grab = 0;
}

static void free_tree(view_t *v) {
    for (view_t *c = v->first, *n; c; c = n) {
        n = c->next;
        free_tree(c);
    }
    forget(v);
    free(v);
}

void v_clear(view_t *v) {
    if (!v) return;
    for (view_t *c = v->first, *n; c; c = n) {
        n = c->next;
        free_tree(c);
    }
    v->first = v->last = 0;
    v->dirty = 1;
    relayout = 1;
}

/* ---- State --------------------------------------------------------------- */

void v_dirty(view_t *v) {
    if (v) v->dirty = 1;
}

void v_set_text(view_t *v, const char *text) {
    if (!v || !text || !strcmp(v->text, text)) return;   /* the same: nothing to draw */
    snprintf(v->text, sizeof(v->text), "%s", text);
    v->dirty = 1;
    if (v->kind == V_LABEL || v->kind == V_TEXT || v->kind == V_BUTTON) relayout = 1;   /* (its size) */
}

void v_set_flags(view_t *v, int flags) {
    if (!v || v->flags == flags) return;
    v->flags = flags;
    v->dirty = 1;
}

void v_set_color(view_t *v, int role) {
    if (!v || v->color == role) return;
    v->color = role;
    v->dirty = 1;
}

void v_set_value(view_t *v, int value) {
    if (!v || v->value == value) return;
    v->value = value;
    v->dirty = 1;
}

void v_show(view_t *v, int shown) {
    if (!v || v->hidden == !shown) return;
    v->hidden = !shown;
    if (v->hidden) forget(v);
    if (v->parent) v->parent->dirty = 1;               /* what was under it, or room for it */
    v->dirty = 1;
    relayout = 1;
}

void v_focus(view_t *v) {
    if (v == focus) return;
    if (focus) focus->dirty = 1;
    focus = v;
    if (focus) focus->dirty = 1;
}

void v_table_rows(view_t *t, int rows) {
    if (!t) return;
    t->rows = rows;
    if (t->selected >= rows) t->selected = -1;
    t->recheck = 1;
}

static int head_h(const view_t *t) { return t->heads[0] ? 28 * S : 0; }
static int visible_rows(const view_t *t) {
    int n = (t->h - head_h(t)) / (t->row_h * S);
    return n < 0 ? 0 : n > V_MAX_ROWS ? V_MAX_ROWS : n;
}

void v_table_select(view_t *t, int row) {
    if (!t || t->selected == row) return;
    t->selected = row;
    int vis = visible_rows(t);
    if (row >= 0 && vis > 0) {                         /* into view */
        if (row < t->first_row) t->first_row = row, t->dirty = 1;
        if (row >= t->first_row + vis) t->first_row = row - vis + 1, t->dirty = 1;
    }
    t->recheck = 1;
}

void v_quit(int code) { quit_code = code; }

/* ---- Layout -------------------------------------------------------------- */

static void natural(view_t *v, int avail, int *w, int *h) {
    int ts = S * v->scale;
    *w = *h = 0;
    switch (v->kind) {
    case V_LABEL:
        *w = ui_text_style_width(v->text, ts, v->style);
        *h = ui_leading(ts);
        break;
    case V_TEXT:
        *w = avail;
        *h = ui_text_block(0, 0, 0, avail, v->text, 0, ts, v->style);
        break;
    case V_BUTTON:
        *w = ui_text_width(v->text, S) + 32 * S;
        if (*w < 96 * S && v->text[1]) *w = 96 * S;   /* (one letter: a small square) */
        if (*w < 30 * S) *w = 30 * S;
        *h = 30 * S;
        break;
    case V_FIELD:
        *w = 160 * S;
        *h = 28 * S;
        break;
    case V_SLIDER:
        *w = 160 * S;
        *h = 28 * S;
        break;
    case V_TABLE:
        *w = avail;
        *h = 120 * S;
        break;
    case V_BOX: {
        int n = 0, col = v->dir == V_COLUMN, inner = avail - 2 * v->pad * S;
        for (view_t *c = v->first; c; c = c->next) {
            if (c->hidden) continue;
            int cw, ch;
            natural(c, inner, &cw, &ch);
            n++;
            if (col) {
                *h += ch;
                if (cw > *w) *w = cw;
            } else {
                *w += cw;
                if (ch > *h) *h = ch;
            }
        }
        if (n > 1) *(col ? h : w) += (n - 1) * v->gap * S;
        *w += 2 * v->pad * S;
        *h += 2 * v->pad * S;
        break;
    }
    }
    if (v->want_w) *w = v->want_w * S;
    if (v->want_h) *h = v->want_h * S;
}

static void layout(view_t *v, int x, int y, int w, int h) {
    if (v->x != x || v->y != y || v->w != w || v->h != h) {   /* moved: it and where it was */
        v->x = x;
        v->y = y;
        v->w = w;
        v->h = h;
        v->dirty = 1;
        if (v->parent) v->parent->dirty = 1;
    }
    if (v->kind != V_BOX) return;
    int pad = v->pad * S, gap = v->gap * S, col = v->dir == V_COLUMN;
    int inner_w = w - 2 * pad, inner_h = h - 2 * pad, n = 0, fixed = 0, flexes = 0;
    for (view_t *c = v->first; c; c = c->next) {
        if (c->hidden) continue;
        n++;
        int cw, ch;
        if (c->flex) {
            flexes += c->flex;
            continue;
        }
        natural(c, inner_w, &cw, &ch);
        fixed += col ? ch : cw;
    }
    int left = (col ? inner_h : inner_w) - fixed - (n > 1 ? (n - 1) * gap : 0);
    if (left < 0) left = 0;
    int pos = col ? y + pad : x + pad, across = col ? inner_w : inner_h;
    for (view_t *c = v->first; c; c = c->next) {
        if (c->hidden) continue;
        int cw, ch;
        natural(c, inner_w, &cw, &ch);
        int along = c->flex ? left * c->flex / flexes : (col ? ch : cw);
        int cross = col ? cw : ch;
        int keeps = c->kind == V_BUTTON || c->kind == V_ICON;      /* not stretched */
        if (v->align == V_STRETCH && !keeps) cross = across;
        if (cross > across) cross = across;
        int off = v->align == V_CENTER ? (across - cross) / 2 : v->align == V_END ? across - cross : 0;
        if (keeps && v->align == V_STRETCH && !col) off = (across - cross) / 2;   /* buttons in a row: centred */
        if (col) layout(c, x + pad + off, pos, cross, along);
        else layout(c, pos, y + pad + off, along, cross);
        pos += along + gap;
    }
}

void v_layout(ui_window *w, view_t *root) {
    win = w;
    S = ui_scale();
    relayout = 0;
    layout(root, 0, 0, w->c.w, w->c.h);
}

/* ---- Drawing ------------------------------------------------------------- */

static unsigned background(view_t *v) {
    while (v && v->bg < 0) v = v->parent;
    return ui_color(v ? v->bg : UI_C_WINDOW);
}

static unsigned row_hash(view_t *t, int r, int i, char cells[][64]) {
    unsigned h = 2166136261u ^ (unsigned)(r == t->selected) * 977u ^ (unsigned)(i & 1) * 31u;
    if (r >= t->rows) return h ^ 0x5A5A5A5Au;
    for (int c = 0; c < t->cols; c++) {
        cells[c][0] = 0;
        if (t->cell) t->cell(t, r, c, cells[c], 64);
        for (const char *s = cells[c]; *s; s++) h = (h ^ (unsigned char)*s) * 16777619u;
        h = (h ^ 0x1F) * 16777619u;
    }
    if (t->row_icon) {
        const char *ic = t->row_icon(t, r);
        for (const char *s = ic ? ic : ""; *s; s++) h = (h ^ (unsigned char)*s) * 16777619u;
    }
    return h | 1;                                      /* (0 is "not drawn yet") */
}

/* All of it, or (all = 0) only the rows whose text, stripe or selection changed. */
static void draw_table(view_t *t, int all) {
    ui_canvas *c = &win->c;
    int rh = t->row_h * S, top = t->y + head_h(t), vis = visible_rows(t), inset = t->flags & V_INSET;
    if (t->first_row > t->rows - vis) t->first_row = t->rows - vis;
    if (t->first_row < 0) t->first_row = 0;
    unsigned base = background(t);
    if (all) {
        ui_clip(c, t->x, t->y, t->w, t->h);
        ui_fill(c, t->x, t->y, t->w, t->h, base);
        if (t->heads[0]) {
            ui_fill(c, t->x, t->y, t->w, head_h(t), ui_color(UI_C_WINDOW));
            ui_fill(c, t->x, top - 1, t->w, 1, ui_color(UI_C_SEPARATOR));
            for (int k = 0, x = t->x + 12 * S; k < t->cols; x += t->col_w[k++] * S)
                ui_text_bold(c, x, t->y + (head_h(t) - 16 * S) / 2, t->heads[k] ? t->heads[k] : "",
                             ui_color(UI_C_SECONDARY), S);
        }
        memset(t->row_hash, 0, sizeof(t->row_hash));
    }
    char cells[V_MAX_COLS][64];
    for (int i = 0; i < vis; i++) {
        int r = t->first_row + i, y = top + i * rh;
        unsigned h = row_hash(t, r, i, cells);
        if (t->row_hash[i] == h) continue;             /* as it is on the screen */
        t->row_hash[i] = h;
        int header = r < t->rows && t->row_header && t->row_header(t, r);
        int sel = r == t->selected && r < t->rows && !header;
        ui_clip(c, t->x, y, t->w, rh);
        unsigned row_bg = inset || (i & 1) == 0 ? base : ui_color(UI_C_ROW_ALT);
        ui_fill(c, t->x, y, t->w, rh, row_bg);
        if (sel && inset) ui_round(c, t->x + 8 * S, y + 2 * S, t->w - 16 * S, rh - 4 * S, 7 * S, ui_color(UI_C_ACCENT));
        else if (sel) ui_fill(c, t->x, y, t->w, rh, ui_color(UI_C_ACCENT));
        if (r >= t->rows) continue;
        unsigned fg = sel ? UI_WHITE : ui_color(header ? UI_C_SECONDARY : UI_C_LABEL);
        int x = t->x + (inset ? 16 : 12) * S, ty = y + (rh - 16 * S) / 2;
        if (header) {
            ui_text_bold(c, x, ty, cells[0], fg, S);
            continue;
        }
        if (t->row_icon) {
            const char *ic = t->row_icon(t, r);
            int is = rh - 8 * S > 22 * S ? 22 * S : rh - 8 * S;
            if (ic) ui_icon(c, x, y + (rh - is) / 2, is, ic, 255);
            x += is + 8 * S;
        }
        int col_x = t->x + (inset ? 16 : 12) * S;      /* (the first column's width includes the icon) */
        for (int k = 0; k < t->cols; col_x += t->col_w[k++] * S)
            ui_text(c, k == 0 ? x : col_x, ty, cells[k], k == 0 ? fg : (sel ? UI_WHITE : ui_color(UI_C_SECONDARY)), S);
    }
    ui_noclip(c);
    t->recheck = 0;
}

static void draw_slider(view_t *v, ui_canvas *c) {
    int x = v->x + 11 * S, w = v->w - 22 * S, y = v->y + (v->h - 6 * S) / 2, value = v->value;
    if (w < 1) w = 1;
    ui_round(c, x, y, w, 6 * S, 3 * S, ui_color(UI_C_CONTROL_OFF));
    ui_round(c, x, y, w * value / 100, 6 * S, 3 * S, ui_color(UI_C_ACCENT));
    int kx = x + w * value / 100, ky = v->y + v->h / 2;
    ui_shadow(c, kx - 11 * S, ky - 11 * S, 22 * S, 22 * S, 11 * S, 3 * S, 60);
    ui_circle(c, kx, ky, 11 * S, UI_WHITE);
}

static void draw_view(view_t *v) {
    ui_canvas *c = &win->c;
    if (v->kind == V_TABLE) {
        draw_table(v, 1);
        return;
    }
    ui_clip(c, v->x, v->y, v->w, v->h);
    if (v->kind != V_CUSTOM || !v->draw) ui_fill(c, v->x, v->y, v->w, v->h, background(v));
    int ts = S * v->scale;
    switch (v->kind) {
    case V_LABEL:
        ui_text_style(c, v->x, v->y + (v->h - 16 * ts) / 2, v->text, ui_color(v->color), ts, v->style);
        break;
    case V_TEXT:
        ui_text_block(c, v->x, v->y, v->w, v->text, ui_color(v->color), ts, v->style);
        break;
    case V_BUTTON:
        ui_button_ex(c, v->x, v->y, v->w, v->h, v->text, S,
                     (v->flags & ~(V_SECRET | V_INSET | V_TOGGLED)) | (v->pressed || (v->flags & V_TOGGLED) ? UI_BTN_PRESSED : 0) |
                     (v->hot ? UI_BTN_HOVER : 0));
        break;
    case V_ICON:
        ui_icon(c, v->x, v->y, v->w, v->text, 255);
        break;
    case V_FIELD: {
        char shown[128];
        int n = (int)strlen(v->text);
        if (v->flags & V_SECRET) {
            for (int i = 0; i < n && i < 127; i++) shown[i] = '*';
            shown[n < 127 ? n : 127] = 0;
        } else {
            snprintf(shown, sizeof(shown), "%s", v->text);
        }
        ui_textfield(c, v->x, v->y, v->w, v->h, shown, S, v == focus);
        if (!n && v->hint[0] && v != focus)
            ui_text(c, v->x + 8 * S, v->y + (v->h - 16 * S) / 2, v->hint, ui_color(UI_C_TERTIARY), S);
        break;
    }
    case V_SLIDER:
        draw_slider(v, c);
        break;
    case V_CUSTOM:
        if (v->draw) v->draw(v, c);
        break;
    }
    ui_noclip(c);
}

/* A view drawn draws everything inside it again (its background covered it). */
static void render(view_t *v, int force) {
    if (v->hidden) return;
    if (force || v->dirty) {
        draw_view(v);
        v->dirty = 0;
        force = 1;
    } else if (v->kind == V_TABLE && v->recheck) {
        draw_table(v, 0);
    }
    for (view_t *c = v->first; c; c = c->next) render(c, force);
}

static void all_dirty(view_t *v) {
    v->dirty = 1;
    for (view_t *c = v->first; c; c = c->next) all_dirty(c);
}

/* ---- Events -------------------------------------------------------------- */

static view_t *hit(view_t *v, int x, int y) {
    if (v->hidden || !ui_inside(x, y, v->x, v->y, v->w, v->h)) return 0;
    for (view_t *c = v->first; c; c = c->next) {
        view_t *h = hit(c, x, y);
        if (h) return h;
    }
    return v;
}

static view_t *default_button(view_t *v) {
    if (v->hidden) return 0;
    if (v->kind == V_BUTTON && (v->flags & UI_BTN_DEFAULT) && !(v->flags & UI_BTN_DISABLED)) return v;
    for (view_t *c = v->first; c; c = c->next) {
        view_t *d = default_button(c);
        if (d) return d;
    }
    return 0;
}

static int clickable(view_t *v) { return v && v->kind == V_BUTTON && !(v->flags & UI_BTN_DISABLED); }
static int takes_keys(view_t *v) {
    return v && (v->kind == V_FIELD || v->kind == V_TABLE || (v->kind == V_CUSTOM && v->focusable));
}

static void set_hot(view_t *v) {
    if (v == hot) return;
    if (hot) {
        hot->hot = 0;
        hot->dirty = 1;
    }
    hot = v;
    if (hot) {
        hot->hot = 1;
        hot->dirty = 1;
    }
}

/* The event for a custom view: x, y inside it. */
static int to_custom(view_t *v, const struct ws_event *e) {
    if (!v->event) return 0;
    struct ws_event local = *e;
    local.x -= v->x;
    local.y -= v->y;
    return v->event(v, &local);
}

static void slide(view_t *v, int x) {
    int w = v->w - 22 * S, value = w > 0 ? (x - v->x - 11 * S) * 100 / w : 0;
    value = value < 0 ? 0 : value > 100 ? 100 : value;
    if (value == v->value) return;
    v_set_value(v, value);
    if (v->on_change) v->on_change(v);
}

static int row_selectable(view_t *t, int r) { return r >= 0 && r < t->rows && !(t->row_header && t->row_header(t, r)); }

static void pick_row(view_t *t, int r) {
    v_table_select(t, r);
    if (t->on_select) t->on_select(t, r);
}

static int table_key(view_t *t, int key) {
    int step = key == KEYCODE_UP ? -1 : key == KEYCODE_DOWN ? 1 : 0;
    if (step) {
        for (int r = t->selected + step; r >= 0 && r < t->rows; r += step)
            if (row_selectable(t, r)) {
                pick_row(t, r);
                break;
            }
        return 1;
    }
    if ((key == '\n' || key == '\r') && t->on_open && row_selectable(t, t->selected)) {
        t->on_open(t, t->selected);
        return 1;
    }
    return 0;
}

static int field_key(view_t *f, int key) {
    int n = (int)strlen(f->text);
    if (key == '\n' || key == '\r') {
        if (!f->on_enter) return 0;
        f->on_enter(f);
        return 1;
    }
    if (key == '\b') {
        if (!n) return 1;
        f->text[n - 1] = 0;
    } else if (key >= ' ' && key < 127) {
        if (n >= (int)sizeof(f->text) - 1) return 1;
        f->text[n] = (char)key;
        f->text[n + 1] = 0;
    } else {
        return 0;                                      /* not for a field: let others have it */
    }
    f->dirty = 1;
    if (f->on_change) f->on_change(f);
    return 1;
}

static void key(view_t *root, int k) {
    int used = 0;
    if (focus && !focus->hidden) {
        if (focus->kind == V_FIELD) used = field_key(focus, k);
        else if (focus->kind == V_TABLE) used = table_key(focus, k);
        else if (focus->kind == V_CUSTOM) {
            struct ws_event e = { WS_EV_KEY, 0, 0, k, 0 };
            used = to_custom(focus, &e);
        }
    }
    for (view_t *v = focus ? focus : root; v && !used; v = v->parent)   /* then whoever wants it */
        if (v->on_key) used = v->on_key(v, k);
    if (!used && (k == '\n' || k == '\r')) {          /* Return: the default button */
        view_t *d = default_button(root);
        if (d && d->on_click) d->on_click(d);
    }
}

static void handle(view_t *root, const struct ws_event *e) {
    view_t *v = hit(root, e->x, e->y);
    switch (e->type) {
    case WS_EV_CLOSE:
        quit_code = 0;
        break;
    case WS_EV_RESIZE:                                 /* a new canvas: everything again */
        relayout = 1;
        all_dirty(root);
        break;
    case WS_EV_PREFS:                                  /* Dark mode etc.: new colours */
        all_dirty(root);
        break;
    case WS_EV_MOVE:
        if (grab) {                                    /* dragging: it keeps the pointer */
            if (grab->kind == V_SLIDER) slide(grab, e->x);
            else to_custom(grab, e);
            break;
        }
        set_hot(clickable(v) ? v : 0);
        if (v && v->kind == V_CUSTOM) to_custom(v, e);
        break;
    case WS_EV_DOWN:
        if (takes_keys(v)) v_focus(v);
        if (clickable(v)) {
            pressed = v;
            v->pressed = 1;
            v->dirty = 1;
        } else if (v && v->kind == V_SLIDER) {
            grab = v;
            slide(v, e->x);
        } else if (v && v->kind == V_CUSTOM) {
            grab = v;
            to_custom(v, e);
        } else if (v && v->kind == V_TABLE && e->y >= v->y + head_h(v)) {
            int row = v->first_row + (e->y - v->y - head_h(v)) / (v->row_h * S);
            if (row_selectable(v, row)) {
                unsigned long now = uptime_ms();
                int twice = row == last_click_row && now - last_click_ms < 450;
                last_click_ms = now;
                last_click_row = twice ? -1 : row;
                pick_row(v, row);
                if (twice && v->on_open) v->on_open(v, row);
            }
        }
        break;
    case WS_EV_UP:
        if (grab) {
            view_t *g = grab;
            grab = 0;
            if (g->kind == V_CUSTOM) to_custom(g, e);
        }
        if (pressed) {
            view_t *b = pressed;
            pressed = 0;
            b->pressed = 0;
            b->dirty = 1;
            if (b == v && b->on_click) b->on_click(b);
        }
        break;
    case WS_EV_WHEEL:
        if (v && v->kind == V_CUSTOM && to_custom(v, e)) break;
        for (; v && v->kind != V_TABLE; v = v->parent) { }
        if (v) {
            v->first_row += e->key * 3;
            v->dirty = 1;                              /* every row moved */
        }
        break;
    case WS_EV_KEY:
        key(root, e->key);
        break;
    }
}

/* ---- The loop ------------------------------------------------------------- */

void v_update(void) {
    if (!win || !the_root) return;
    if (relayout) {
        relayout = 0;
        layout(the_root, 0, 0, win->c.w, win->c.h);
    }
    render(the_root, 0);
    if (win->c.dx1 > win->c.dx0) ui_present(win);
}

int v_run(ui_window *w, view_t *root, int tick_ms, void (*tick)(void)) {
    win = w;
    the_root = root;
    S = ui_scale();
    quit_code = -1;
    relayout = 1;
    if (!focus) {                                      /* the first that takes keys */
        view_t *stack[64];
        int n = 0;
        stack[n++] = root;
        while (n && !focus) {
            view_t *v = stack[--n];
            if (takes_keys(v) && !v->hidden) focus = v;
            view_t *kids[64];
            int k = 0;
            for (view_t *c = v->first; c && k < 64; c = c->next) kids[k++] = c;
            while (k && n < 64) stack[n++] = kids[--k];
        }
    }
    unsigned long next = uptime_ms() + (unsigned long)tick_ms;
    for (;;) {
        if (relayout) {
            relayout = 0;
            layout(root, 0, 0, w->c.w, w->c.h);
        }
        render(root, 0);
        if (w->c.dx1 > w->c.dx0) ui_present(w);         /* only when something was drawn */
        if (quit_code >= 0) return quit_code;
        int wait = 0;
        if (tick_ms) {
            unsigned long now = uptime_ms();
            wait = next > now ? (int)(next - now) : 1;
        }
        struct ws_event e;
        if (!ui_wait(w, &e, wait)) {
            if (tick) tick();
            next = uptime_ms() + (unsigned long)tick_ms;
            continue;
        }
        do handle(root, &e);                           /* every event waiting, then one pass */
        while (quit_code < 0 && ui_poll(w, &e));
    }
}

static void alert_ok(view_t *b) {
    (void)b;
    v_quit(0);
}

void v_alert(const char *icon, const char *title, const char *message) {
    if (!ui_desktop()) {
        printf("%s: %s\n", title, message);
        return;
    }
    int s = ui_scale();
    ui_window w;
    if (ui_open(&w, "", 460 * s, 200 * s) < 0) return;
    view_t *root = v_box(0, V_ROW);
    root->gap = 16;
    root->align = V_START;
    v_icon(root, icon, 56);
    view_t *col = v_box(root, V_COLUMN);
    col->pad = 0;
    col->flex = 1;
    v_label(col, title)->style = UI_BOLD;
    v_text(col, message);
    v_spacer(col);
    view_t *row = v_box(col, V_ROW);
    row->pad = 0;
    v_spacer(row);
    v_button(row, "OK", alert_ok)->flags = UI_BTN_DEFAULT;
    v_run(&w, root, 0, 0);
}
