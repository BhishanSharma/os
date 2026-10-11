// ui.c - drawing, widgets, and windows from the window server (see ui.h)
#include <string.h>
#include <stdio.h>
#include <os.h>
#include <ui.h>

extern const unsigned char font8x16[256][16];

/* ---- Canvas -------------------------------------------------------------- */

void ui_canvas_init(ui_canvas *c, unsigned *px, int w, int h, int stride) {
    c->px = px;
    c->w = w;
    c->h = h;
    c->stride = stride;
    ui_noclip(c);
}

void ui_noclip(ui_canvas *c) {
    c->cx0 = c->cy0 = 0;
    c->cx1 = c->w;
    c->cy1 = c->h;
}

void ui_clip(ui_canvas *c, int x, int y, int w, int h) {
    c->cx0 = x < 0 ? 0 : x;
    c->cy0 = y < 0 ? 0 : y;
    c->cx1 = x + w > c->w ? c->w : x + w;
    c->cy1 = y + h > c->h ? c->h : y + h;
}

void ui_fill(ui_canvas *c, int x, int y, int w, int h, unsigned color) {
    int x0 = x < c->cx0 ? c->cx0 : x, y0 = y < c->cy0 ? c->cy0 : y;
    int x1 = x + w > c->cx1 ? c->cx1 : x + w, y1 = y + h > c->cy1 ? c->cy1 : y + h;
    for (int j = y0; j < y1; j++) {
        unsigned *row = c->px + (long)j * c->stride;
        for (int i = x0; i < x1; i++) row[i] = color;
    }
}

void ui_frame(ui_canvas *c, int x, int y, int w, int h, unsigned color) {
    ui_fill(c, x, y, w, 1, color);
    ui_fill(c, x, y + h - 1, w, 1, color);
    ui_fill(c, x, y, 1, h, color);
    ui_fill(c, x + w - 1, y, 1, h, color);
}

/* Filled, with corners of radius r cut round. */
void ui_round(ui_canvas *c, int x, int y, int w, int h, int r, unsigned color) {
    if (r * 2 > h) r = h / 2;
    if (r * 2 > w) r = w / 2;
    for (int j = 0; j < h; j++) {
        int inset = 0;
        int dy = j < r ? r - j : j >= h - r ? j - (h - r - 1) : 0;
        if (dy) {
            while (inset < r && (r - inset) * (r - inset) + dy * dy > r * r) inset++;
        }
        ui_fill(c, x + inset, y + j, w - 2 * inset, 1, color);
    }
}

void ui_circle(ui_canvas *c, int cx, int cy, int r, unsigned color) {
    ui_round(c, cx - r, cy - r, 2 * r, 2 * r, r, color);
}

void ui_copy(ui_canvas *c, int x, int y, const unsigned *src, int w, int h, int stride) {
    int x0 = x < c->cx0 ? c->cx0 : x, y0 = y < c->cy0 ? c->cy0 : y;
    int x1 = x + w > c->cx1 ? c->cx1 : x + w, y1 = y + h > c->cy1 ? c->cy1 : y + h;
    if (x1 <= x0) return;
    for (int j = y0; j < y1; j++)
        memcpy(c->px + (long)j * c->stride + x0, src + (long)(j - y) * stride + (x0 - x),
               (size_t)(x1 - x0) * 4);
}

int ui_char(ui_canvas *c, int x, int y, unsigned char ch, unsigned color, int scale) {
    if (x >= c->cx1 || y >= c->cy1 || x + 8 * scale <= c->cx0 || y + 16 * scale <= c->cy0) return 8 * scale;
    const unsigned char *g = font8x16[ch];
    for (int j = 0; j < 16; j++) {
        unsigned char bits = g[j];
        if (!bits) continue;
        for (int i = 0; i < 8; i++)
            if (bits & (0x80 >> i)) ui_fill(c, x + i * scale, y + j * scale, scale, scale, color);
    }
    return 8 * scale;
}

int ui_text(ui_canvas *c, int x, int y, const char *s, unsigned color, int scale) {
    int start = x;
    while (*s) x += ui_char(c, x, y, (unsigned char)*s++, color, scale);
    return x - start;
}

int ui_text_width(const char *s, int scale) { return (int)strlen(s) * 8 * scale; }

/* ---- Widgets ------------------------------------------------------------- */

void ui_button(ui_canvas *c, int x, int y, int w, int h, const char *label, int scale, int pressed) {
    ui_round(c, x, y, w, h, 5 * scale, UI_BORDER);
    ui_round(c, x + 1, y + 1, w - 2, h - 2, 5 * scale - 1, pressed ? UI_ACCENT : UI_WHITE);
    ui_text(c, x + (w - ui_text_width(label, scale)) / 2, y + (h - 16 * scale) / 2, label,
            pressed ? UI_WHITE : UI_TEXT, scale);
}

void ui_textfield(ui_canvas *c, int x, int y, int w, int h, const char *text, int scale, int focused) {
    ui_fill(c, x, y, w, h, UI_WHITE);
    ui_frame(c, x, y, w, h, focused ? UI_ACCENT : UI_BORDER);
    int room = (w - 8 * scale) / (8 * scale), len = (int)strlen(text);
    const char *shown = len > room ? text + len - room : text;      /* the end stays in view */
    int tx = x + 4 * scale, ty = y + (h - 16 * scale) / 2;
    tx += ui_text(c, tx, ty, shown, UI_TEXT, scale);
    if (focused) ui_fill(c, tx, ty, scale, 16 * scale, UI_ACCENT);
}

static int row_height(int scale) { return 20 * scale; }

int ui_list_rows(int h, int scale) { return h / row_height(scale); }
int ui_list_hit(int y, int scale) { return y < 0 ? -1 : y / row_height(scale); }

void ui_list(ui_canvas *c, int x, int y, int w, int h, const char *const *items, int count,
             int selected, int first, int scale) {
    ui_fill(c, x, y, w, h, UI_WHITE);
    int rows = ui_list_rows(h, scale), rh = row_height(scale);
    for (int i = 0; i < rows && first + i < count; i++) {
        int k = first + i, ry = y + i * rh;
        if (k == selected) ui_fill(c, x, ry, w, rh, UI_ACCENT);
        ui_text(c, x + 6 * scale, ry + (rh - 16 * scale) / 2, items[k], k == selected ? UI_WHITE : UI_TEXT, scale);
    }
}

/* ---- Windows ------------------------------------------------------------- */

int ui_desktop(void) {
    int p = port_lookup(WS_SERVICE);
    if (p < 0) return 0;
    port_close(p);
    return 1;
}

int ui_scale(void) {
    struct os_gfx_info g;
    if (gfx_info(&g) < 0) return 1;
    return g.cell_w >= 16 ? 2 : 1;
}

int ui_open(ui_window *w, const char *title, int width, int height) {
    int server = -1;
    for (int i = 0; i < 20 && (server = port_lookup(WS_SERVICE)) < 0; i++) sleep_ms(50);
    if (server < 0) return server;
    struct ws_request q;
    memset(&q, 0, sizeof(q));
    q.op = WS_CREATE;
    q.w = width;
    q.h = height;
    q.pid = getpid();
    snprintf(q.title, sizeof(q.title), "%s", title);
    struct ws_reply r;
    int n = port_call(server, &q, sizeof(q), &r, sizeof(r), 5000);
    port_close(server);
    if (n < (int)sizeof(r)) return n < 0 ? n : SYSERR_BADCALL;
    if (r.id < 0) return r.id;
    w->s = shm_map(r.shm, ws_surface_size(width, height));
    if (!w->s) return SYSERR_NOMEM;
    w->id = r.id;
    w->x = r.x;
    w->y = r.y;
    w->scale = ui_scale();
    ui_canvas_init(&w->c, (unsigned *)((char *)w->s + WS_HEADER), width, height, width);
    return 0;
}

void ui_set_title(ui_window *w, const char *title) {
    int server = port_lookup(WS_SERVICE);
    if (server < 0) return;
    struct ws_request q;
    memset(&q, 0, sizeof(q));
    q.op = WS_TITLE;
    q.id = w->id;
    q.pid = getpid();
    snprintf(q.title, sizeof(q.title), "%s", title);
    port_send(server, &q, sizeof(q), -1);
    port_close(server);
}

void ui_present(ui_window *w) { w->s->damage++; }

int ui_poll(ui_window *w, struct ws_event *e) {
    struct ws_surface *s = w->s;
    if (s->ev_tail == s->ev_head) return 0;
    *e = s->ev[s->ev_tail % WS_EVENTS];
    s->ev_tail++;
    return 1;
}

int ui_wait(ui_window *w, struct ws_event *e, int timeout_ms) {
    unsigned long until = uptime_ms() + (unsigned long)timeout_ms;
    while (!ui_poll(w, e)) {
        if (timeout_ms && uptime_ms() >= until) return 0;
        sleep_ms(10);
    }
    return 1;
}
