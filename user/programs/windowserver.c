// windowserver [app]... - the desktop: owns the screen, the mouse and the
// keyboard, and draws the programs' windows (like macOS's WindowServer).
//
// Programs ask for a window on the "windowserver" port (ws.h, ui.h) and draw
// into a shared memory surface; the server composites the surfaces back to
// front, only where something changed (a list of damaged rectangles), with
// soft shadows and round corners, under a translucent menu bar and above a
// Dock that magnifies under the pointer. Windows can be dragged by the title
// bar, closed and minimised. Clicking a Dock icon starts that app (or brings
// its window back). Ctrl+Q leaves.
#include <stdio.h>
#include <string.h>
#include <os.h>
#include <ui.h>

#define MAX_WINDOWS 16
#define MAX_DAMAGE 32
#define KEY_QUIT 17                    /* Ctrl+Q */

typedef struct {
    int used, id, pid;
    int x, y, w, h;                    /* the content on the screen */
    int minimised;
    unsigned seen;                     /* the damage count drawn last */
    char title[48];
    struct ws_surface *s;
} window_t;

typedef struct {
    const char *name, *program, *glyph;
    unsigned color;
    int pid;
    int size;                          /* now, in pixels (it grows under the pointer) */
} dock_app_t;

static dock_app_t apps[] = {
    { "Terminal", "terminal", ">_", 0x30323A, 0, 0 },
    { "Files", "finder", "F", 0x2F7BF5, 0, 0 },
    { "About", "about", "i", 0x8E8E93, 0, 0 },
};
#define DOCK_APPS ((int)(sizeof(apps) / sizeof(apps[0])))

static window_t wins[MAX_WINDOWS];
static int order[MAX_WINDOWS], norder;            /* back to front */
static mutex_t lock;
static int next_id = 1, placed;

static struct os_gfx_info g;
static int W, H, S, MENU_H, TITLE_H, RADIUS, SHADOW;
static ui_canvas scr;                             /* the back buffer */
static unsigned *wallpaper;                       /* one colour per row */
static struct { int x0, y0, x1, y1; } dmg[MAX_DAMAGE];
static int ndmg;
static int mouse_x, mouse_y, buttons;
static int drag = -1, drag_dx, drag_dy, capture = -1;
static char clock_text[24];
static int menu_open, menu_hot = -1;

static const char *menu_items[] = { "About Terminal OS", "New Terminal", "Files", "Quit desktop" };
#define MENU_ITEMS 4

/* ---- Damage: a short list of rectangles, merged when they overlap -------- */

static void damage(int x, int y, int w, int h) {
    if (w <= 0 || h <= 0) return;
    int x0 = x < 0 ? 0 : x, y0 = y < 0 ? 0 : y, x1 = x + w > W ? W : x + w, y1 = y + h > H ? H : y + h;
    if (x1 <= x0 || y1 <= y0) return;
    for (int i = 0; i < ndmg; i++) {
        if (x0 > dmg[i].x1 || x1 < dmg[i].x0 || y0 > dmg[i].y1 || y1 < dmg[i].y0) continue;
        int nx0 = x0 < dmg[i].x0 ? x0 : dmg[i].x0, ny0 = y0 < dmg[i].y0 ? y0 : dmg[i].y0;
        int nx1 = x1 > dmg[i].x1 ? x1 : dmg[i].x1, ny1 = y1 > dmg[i].y1 ? y1 : dmg[i].y1;
        dmg[i] = dmg[--ndmg];                      /* take it out, add the union again */
        damage(nx0, ny0, nx1 - nx0, ny1 - ny0);
        return;
    }
    if (ndmg == MAX_DAMAGE) {                       /* too many: one big rectangle */
        for (int i = 1; i < ndmg; i++) {
            if (dmg[i].x0 < dmg[0].x0) dmg[0].x0 = dmg[i].x0;
            if (dmg[i].y0 < dmg[0].y0) dmg[0].y0 = dmg[i].y0;
            if (dmg[i].x1 > dmg[0].x1) dmg[0].x1 = dmg[i].x1;
            if (dmg[i].y1 > dmg[0].y1) dmg[0].y1 = dmg[i].y1;
        }
        ndmg = 1;
        damage(x0, y0, x1 - x0, y1 - y0);
        return;
    }
    dmg[ndmg].x0 = x0;
    dmg[ndmg].y0 = y0;
    dmg[ndmg].x1 = x1;
    dmg[ndmg].y1 = y1;
    ndmg++;
}

static void outer(window_t *w, int *x, int *y, int *ww, int *hh) {
    *x = w->x - 1;
    *y = w->y - TITLE_H - 1;
    *ww = w->w + 2;
    *hh = w->h + TITLE_H + 2;
}

static void damage_window(window_t *w) {
    int x, y, ww, hh;
    outer(w, &x, &y, &ww, &hh);
    damage(x - SHADOW, y - SHADOW, ww + 2 * SHADOW, hh + 2 * SHADOW + SHADOW / 3);   /* and its shadow */
}

static void damage_menu_bar(void) { damage(0, 0, W, MENU_H); }

/* ---- The Dock ------------------------------------------------------------ */

static int icon(void) { return 50 * S; }
static int pad(void) { return 10 * S; }
static int dock_h(void) { return icon() + 2 * pad(); }
static int dock_bottom(void) { return H - 8 * S; }

static int dock_w(void) {
    int w = pad();
    for (int i = 0; i < DOCK_APPS; i++) w += apps[i].size + pad();
    return w;
}
static int dock_x(void) { return (W - dock_w()) / 2; }

static int icon_x(int k) {
    int x = dock_x() + pad();
    for (int i = 0; i < k; i++) x += apps[i].size + pad();
    return x;
}

static void damage_dock(void) {                   /* everything the Dock can cover */
    int widest = DOCK_APPS * (icon() * 17 / 10 + pad()) + pad();
    damage((W - widest) / 2 - 4 * S, dock_bottom() - icon() * 2 - 30 * S, widest + 8 * S, icon() * 2 + 30 * S + 8 * S);
}

static int dock_hit(int x, int y) {
    if (y < dock_bottom() - dock_h() - icon() || y >= dock_bottom()) return -1;
    for (int i = 0; i < DOCK_APPS; i++) {
        int ix = icon_x(i), iy = dock_bottom() - pad() - apps[i].size;
        if (ui_inside(x, y, ix, iy, apps[i].size, apps[i].size)) return i;
    }
    return -1;
}

/* Icons grow towards the pointer while it is over the Dock; one step per
 * frame, so it moves smoothly. -> 1 while still moving. */
static int animate_dock(void) {
    int over = mouse_y >= dock_bottom() - dock_h() - icon() / 2 && mouse_y < dock_bottom() &&
               mouse_x >= dock_x() - icon() && mouse_x < dock_x() + dock_w() + icon();
    int moving = 0;
    for (int i = 0; i < DOCK_APPS; i++) {
        int target = icon();
        if (over && !menu_open) {
            int centre = icon_x(i) + apps[i].size / 2, d = mouse_x - centre;
            if (d < 0) d = -d;
            int reach = icon() * 5 / 2;
            if (d < reach) target += icon() * 7 / 10 * (reach - d) / reach;
        }
        if (!apps[i].size) apps[i].size = target;
        int diff = target - apps[i].size;
        if (diff) {
            int step = diff / 3;
            if (!step) step = diff > 0 ? 1 : -1;
            apps[i].size += step;
            moving = 1;
        }
    }
    if (moving) damage_dock();
    return moving;
}

/* ---- The menu (under "Terminal OS" in the menu bar) ------------------------ */

static int menu_title_w(void) { return ui_text_style_width("Terminal OS", S, UI_BOLD) + 28 * S; }
static int item_h(void) { return 24 * S; }
static int menu_x(void) { return 6 * S; }
static int menu_y(void) { return MENU_H + 2 * S; }
static int menu_w(void) { return 220 * S; }
static int menu_h(void) { return MENU_ITEMS * item_h() + 10 * S; }

static void damage_menu(void) {
    damage(menu_x() - SHADOW, menu_y() - SHADOW, menu_w() + 2 * SHADOW, menu_h() + 2 * SHADOW);
    damage_menu_bar();
}

static int menu_hit(int x, int y) {
    if (!ui_inside(x, y, menu_x(), menu_y() + 5 * S, menu_w(), MENU_ITEMS * item_h())) return -1;
    return (y - menu_y() - 5 * S) / item_h();
}

/* ---- Windows ------------------------------------------------------------- */

static window_t *front(void) {
    for (int i = norder - 1; i >= 0; i--)
        if (!wins[order[i]].minimised) return &wins[order[i]];
    return 0;
}

static void raise(int k) {
    window_t *old = front();
    int j = 0;
    for (int i = 0; i < norder; i++)
        if (order[i] != k) order[j++] = order[i];
    order[j++] = k;
    norder = j;
    if (old && old != &wins[k]) {
        old->s->focused = 0;
        damage_window(old);
    }
    wins[k].s->focused = 1;
    damage_window(&wins[k]);
    damage_menu_bar();                             /* the app's name */
}

static void send_event(window_t *w, int type, int x, int y, int key) {
    struct ws_surface *s = w->s;
    if (s->ev_head - s->ev_tail >= WS_EVENTS) return;     /* it is not keeping up */
    struct ws_event *e = &s->ev[s->ev_head % WS_EVENTS];
    e->type = type;
    e->x = x;
    e->y = y;
    e->key = key;
    e->buttons = buttons;
    s->ev_head++;
    futex(FUTEX_WAKE, (volatile int *)&s->ev_head, 0);   /* the program wakes at once */
}

static void remove_window(int k) {
    window_t *w = &wins[k];
    damage_window(w);
    munmap(w->s);
    w->used = 0;
    int j = 0;
    for (int i = 0; i < norder; i++)
        if (order[i] != k) order[j++] = order[i];
    norder = j;
    if (drag == k) drag = -1;
    if (capture == k) capture = -1;
    window_t *f = front();
    if (f) raise((int)(f - wins));
    damage_menu_bar();
}

static int create_window(const struct ws_request *q, struct ws_reply *r) {
    int max_w = W - 2, max_h = H - MENU_H - TITLE_H - 2;
    if (q->w < 1 || q->h < 1 || q->w > max_w || q->h > max_h) return SYSERR_BADCALL;
    int k = 0;
    while (k < MAX_WINDOWS && wins[k].used) k++;
    if (k == MAX_WINDOWS) return SYSERR_AGAIN;
    window_t *w = &wins[k];
    memset(w, 0, sizeof(*w));
    w->id = next_id++;
    snprintf(r->shm, sizeof(r->shm), "ws.%d", w->id);
    w->s = shm_map(r->shm, ws_surface_size(q->w, q->h));
    if (!w->s) return SYSERR_NOMEM;
    memset(w->s, 0, sizeof(*w->s));
    w->s->w = q->w;
    w->s->h = q->h;
    w->used = 1;
    w->pid = q->pid;
    w->w = q->w;
    w->h = q->h;
    snprintf(w->title, sizeof(w->title), "%s", q->title);
    int step = 28 * S;                             /* cascade from the top left */
    w->x = 80 * S + (placed % 8) * step;
    w->y = MENU_H + TITLE_H + 24 * S + (placed % 8) * step;
    placed++;
    if (w->x + w->w > W - 1) w->x = W - 1 - w->w;
    if (w->y + w->h > H - 1) w->y = H - 1 - w->h;
    if (w->x < 1) w->x = 1;
    if (w->y < MENU_H + TITLE_H + 1) w->y = MENU_H + TITLE_H + 1;
    order[norder++] = k;
    raise(k);
    r->id = w->id;
    r->x = w->x;
    r->y = w->y;
    return 0;
}

/* The window port: programs open windows and change their titles. */
static void serve(void *arg) {
    int port = *(int *)arg;
    for (;;) {
        struct ws_request q;
        int reply = -1;
        int n = port_receive(port, &q, sizeof(q), 0, &reply);
        if (n < (int)sizeof(q)) {
            if (reply >= 0) port_close(reply);
            continue;
        }
        q.title[sizeof(q.title) - 1] = 0;
        mutex_lock(&lock);
        if (q.op == WS_CREATE) {
            struct ws_reply r;
            memset(&r, 0, sizeof(r));
            int e = create_window(&q, &r);
            if (e < 0) r.id = e;
            mutex_unlock(&lock);
            if (reply >= 0) port_send(reply, &r, sizeof(r), -1);
        } else {
            for (int k = 0; k < MAX_WINDOWS; k++)
                if (wins[k].used && wins[k].id == q.id && wins[k].pid == q.pid) {
                    snprintf(wins[k].title, sizeof(wins[k].title), "%s", q.title);
                    damage_window(&wins[k]);
                    damage_menu_bar();
                }
            mutex_unlock(&lock);
        }
        if (reply >= 0) port_close(reply);
    }
}

/* ---- The pointer: an arrow, anti-aliased, made once ----------------------- */

#define CURSOR_W 14
#define CURSOR_H 22
static unsigned char cursor_black[CURSOR_W * 2 * CURSOR_H * 2], cursor_white[CURSOR_W * 2 * CURSOR_H * 2];

/* Points in 1/100 of a pixel at 1x. */
static const int arrow_out[][2] = { { 0, 0 }, { 0, 1750 }, { 410, 1330 }, { 720, 2050 }, { 1010, 1930 },
                                    { 710, 1230 }, { 1240, 1230 } };
static const int arrow_in[][2] = { { 130, 300 }, { 130, 1450 }, { 440, 1140 }, { 780, 1900 }, { 870, 1860 },
                                   { 540, 1100 }, { 960, 1100 } };

static int in_polygon(const int (*p)[2], int n, int x, int y) {
    int in = 0;
    for (int i = 0, j = n - 1; i < n; j = i++) {
        if ((p[i][1] > y) != (p[j][1] > y)) {
            long cross = (long)(p[j][0] - p[i][0]) * (y - p[i][1]) / (p[j][1] - p[i][1]) + p[i][0];
            if (x < cross) in = !in;
        }
    }
    return in;
}

static void make_cursor(void) {
    int cw = CURSOR_W * S, ch = CURSOR_H * S;
    for (int j = 0; j < ch; j++)
        for (int i = 0; i < cw; i++) {
            int black = 0, white = 0;
            for (int sy = 0; sy < 4; sy++)
                for (int sx = 0; sx < 4; sx++) {
                    int x = (i * 100 + sx * 25 + 12) / S, y = (j * 100 + sy * 25 + 12) / S;
                    if (in_polygon(arrow_out, 7, x, y)) black++;
                    if (in_polygon(arrow_in, 7, x, y)) white++;
                }
            cursor_black[j * cw + i] = (unsigned char)(black * 255 / 16);
            cursor_white[j * cw + i] = (unsigned char)(white * 255 / 16);
        }
}

static void draw_cursor(void) {
    int cw = CURSOR_W * S, ch = CURSOR_H * S;
    for (int j = 0; j < ch; j++)
        for (int i = 0; i < cw; i++) {
            ui_blend_pixel(&scr, mouse_x + i, mouse_y + j, 0x000000, cursor_black[j * cw + i]);
            ui_blend_pixel(&scr, mouse_x + i, mouse_y + j, 0xFFFFFF, cursor_white[j * cw + i]);
        }
}

static void damage_cursor(void) { damage(mouse_x - S, mouse_y - S, (CURSOR_W + 2) * S, (CURSOR_H + 2) * S); }

/* ---- Drawing ------------------------------------------------------------- */

static void draw_window(window_t *w, int focused) {
    int x, y, ww, hh;
    outer(w, &x, &y, &ww, &hh);
    ui_shadow(&scr, x + 2 * S, y + SHADOW / 3, ww - 4 * S, hh - 2 * S, RADIUS, SHADOW, focused ? 110 : 60);
    ui_shadow(&scr, x, y, ww, hh, RADIUS, 2 * S, 50);                              /* a tight edge */
    ui_round(&scr, x, y, ww, hh, RADIUS, focused ? 0x9C9CA4 : 0xC0C0C6);       /* the edge */
    ui_round_ex(&scr, w->x, w->y - TITLE_H, w->w, TITLE_H, RADIUS - 1, focused ? 0xE8E8EC : 0xF4F4F6,
                255, UI_TL | UI_TR);
    ui_fill(&scr, w->x, w->y - 1, w->w, 1, 0xD0D0D6);
    int cy = w->y - TITLE_H / 2, r = 6 * S;
    unsigned grey = 0xD2D2D6;
    ui_circle(&scr, w->x + 16 * S, cy, r, focused ? 0xFF5F57 : grey);
    ui_circle(&scr, w->x + 36 * S, cy, r, focused ? 0xFEBC2E : grey);
    ui_circle(&scr, w->x + 56 * S, cy, r, focused ? 0x28C840 : grey);
    int tw = ui_text_style_width(w->title, S, UI_BOLD), tx = w->x + (w->w - tw) / 2;
    if (tx < w->x + 70 * S) tx = w->x + 70 * S;
    ui_text_bold(&scr, tx, w->y - TITLE_H + (TITLE_H - 16 * S) / 2, w->title, focused ? 0x3A3A3E : 0xA0A0A6, S);
    ui_copy_round(&scr, w->x, w->y, (const unsigned *)((char *)w->s + WS_HEADER), w->w, w->h, w->w,
                  RADIUS - 1, UI_BL | UI_BR);
}

static void draw_menu_bar(void) {
    ui_fill_a(&scr, 0, 0, W, MENU_H, 0xF6F6F8, 190);           /* translucent */
    ui_fill_a(&scr, 0, MENU_H - 1, W, 1, 0x000000, 30);
    int ty = (MENU_H - 16 * S) / 2, x = 14 * S;
    if (menu_open) ui_round_ex(&scr, menu_x(), 3 * S, menu_title_w() - 2 * S, MENU_H - 6 * S, 5 * S, 0x000000, 40, UI_ALL);
    x += ui_text_bold(&scr, x, ty, "Terminal OS", UI_TEXT, S) + 24 * S;
    window_t *f = front();
    if (f) ui_text_bold(&scr, x, ty, f->title, UI_TEXT, S);
    ui_text(&scr, W - 16 * S - ui_text_width(clock_text, S), ty, clock_text, UI_TEXT, S);
}

static void draw_dock(void) {
    int x = dock_x(), w = dock_w(), y = dock_bottom() - dock_h();
    ui_shadow_ex(&scr, x, y + 2 * S, w, dock_h(), 18 * S, 10 * S, 60, 1);      /* not under the glass */
    ui_round_ex(&scr, x, y, w, dock_h(), 18 * S, 0xFFFFFF, 70, UI_ALL);          /* frosted glass */
    ui_round_ex(&scr, x + 1, y + 1, w - 2, dock_h() - 2, 18 * S - 1, 0xE8E8F0, 110, UI_ALL);
    int hot = dock_hit(mouse_x, mouse_y);
    for (int i = 0; i < DOCK_APPS; i++) {
        int size = apps[i].size, ix = icon_x(i), iy = dock_bottom() - pad() - size, r = size * 22 / 100;
        ui_shadow(&scr, ix, iy + size / 30, size, size, r, size / 12 + 1, 70);
        ui_round(&scr, ix, iy, size, size, r, apps[i].color);
        int ts = size / 22;
        if (ts < 1) ts = 1;
        if (ts > 4) ts = 4;
        int gw = ui_text_style_width(apps[i].glyph, ts, UI_BOLD);
        ui_text_bold(&scr, ix + (size - gw) / 2, iy + (size - 16 * ts) / 2, apps[i].glyph, 0xFFFFFF, ts);
        if (apps[i].pid > 0) ui_circle(&scr, ix + size / 2, dock_bottom() - pad() / 2 + S, 2 * S, 0x3A3A3E);
        if (i == hot && !menu_open) {                          /* the name, above the icon */
            int lw = ui_text_width(apps[i].name, S) + 20 * S, lh = 24 * S;
            int lx = ix + size / 2 - lw / 2, ly = iy - lh - 10 * S;
            ui_round_ex(&scr, lx, ly, lw, lh, 6 * S, 0x2A2A30, 220, UI_ALL);
            ui_text(&scr, lx + 10 * S, ly + (lh - 16 * S) / 2, apps[i].name, 0xFFFFFF, S);
        }
    }
}

static void draw_menu(void) {
    if (!menu_open) return;
    int x = menu_x(), y = menu_y();
    ui_shadow(&scr, x, y + 4 * S, menu_w(), menu_h(), 8 * S, 14 * S, 90);
    ui_round_ex(&scr, x, y, menu_w(), menu_h(), 8 * S, 0xF7F7F9, 245, UI_ALL);
    for (int i = 0; i < MENU_ITEMS; i++) {
        int iy = y + 5 * S + i * item_h();
        if (i == menu_hot) ui_round(&scr, x + 5 * S, iy, menu_w() - 10 * S, item_h(), 5 * S, UI_ACCENT);
        ui_text(&scr, x + 16 * S, iy + (item_h() - 16 * S) / 2, menu_items[i], i == menu_hot ? UI_WHITE : UI_TEXT, S);
    }
}

static void compose(void) {
    window_t *f = front();
    for (int d = 0; d < ndmg; d++) {
        int x0 = dmg[d].x0, y0 = dmg[d].y0, x1 = dmg[d].x1, y1 = dmg[d].y1;
        ui_clip(&scr, x0, y0, x1 - x0, y1 - y0);
        for (int y = y0; y < y1; y++) ui_fill(&scr, x0, y, x1 - x0, 1, wallpaper[y]);
        for (int i = 0; i < norder; i++) {
            window_t *w = &wins[order[i]];
            if (!w->minimised) draw_window(w, w == f);
        }
        draw_menu_bar();
        draw_dock();
        draw_menu();
        draw_cursor();
        gfx_blit(x0, y0, x1 - x0, y1 - y0, scr.px + (long)y0 * scr.stride + x0, scr.stride);
    }
    ndmg = 0;
}

/* ---- Input --------------------------------------------------------------- */

static int window_at(int x, int y) {
    for (int i = norder - 1; i >= 0; i--) {
        window_t *w = &wins[order[i]];
        int ox, oy, ow, oh;
        outer(w, &ox, &oy, &ow, &oh);
        if (!w->minimised && ui_inside(x, y, ox, oy, ow, oh)) return order[i];
    }
    return -1;
}

static void launch(int i) {
    for (int k = 0; k < MAX_WINDOWS; k++)            /* running: bring its window back */
        if (wins[k].used && apps[i].pid > 0 && wins[k].pid == apps[i].pid) {
            if (wins[k].minimised) {
                wins[k].minimised = 0;
                damage_window(&wins[k]);
            }
            raise(k);
            return;
        }
    char *argv[] = { (char *)apps[i].program, 0 };
    int pid = spawn(apps[i].program, argv, 0);
    if (pid > 0) apps[i].pid = pid;
    damage_dock();
}

static void quit(void);

static void menu_choose(int i) {
    if (i == 0) launch(2);
    else if (i == 1) {
        char *argv[] = { "terminal", 0 };
        spawn("terminal", argv, 0);
    } else if (i == 2) launch(1);
    else if (i == 3) quit();
}

static void press(void) {
    printf("windowserver: click at %d,%d\n", mouse_x, mouse_y);   /* (for the tests) */
    if (menu_open) {                                   /* any click closes the menu */
        int i = menu_hit(mouse_x, mouse_y);
        menu_open = 0;
        menu_hot = -1;
        damage_menu();
        if (i >= 0) menu_choose(i);
        return;
    }
    if (ui_inside(mouse_x, mouse_y, 0, 0, menu_title_w(), MENU_H)) {
        menu_open = 1;
        damage_menu();
        return;
    }
    int d = dock_hit(mouse_x, mouse_y);
    if (d >= 0) {
        launch(d);
        return;
    }
    int k = window_at(mouse_x, mouse_y);
    if (k < 0) return;
    window_t *w = &wins[k];
    raise(k);
    if (mouse_y < w->y) {                              /* the title bar */
        int cy = w->y - TITLE_H / 2, r = 8 * S;
        if (ui_inside(mouse_x, mouse_y, w->x + 16 * S - r, cy - r, 2 * r, 2 * r)) {
            if (sigsend(w->pid, 0) < 0) remove_window(k);
            else send_event(w, WS_EV_CLOSE, 0, 0, 0);
            return;
        }
        if (ui_inside(mouse_x, mouse_y, w->x + 36 * S - r, cy - r, 2 * r, 2 * r)) {
            w->minimised = 1;
            damage_window(w);
            window_t *f = front();
            if (f) raise((int)(f - wins));
            damage_menu_bar();
            return;
        }
        drag = k;
        drag_dx = mouse_x - w->x;
        drag_dy = mouse_y - w->y;
        return;
    }
    capture = k;
    send_event(w, WS_EV_DOWN, mouse_x - w->x, mouse_y - w->y, 0);
}

static void handle_mouse(void) {
    struct os_mouse m;
    if (getmouse(&m) <= 0) return;
    int was = buttons;
    if (m.x != mouse_x || m.y != mouse_y) {
        damage_cursor();
        int old_hot = dock_hit(mouse_x, mouse_y);
        mouse_x = m.x;
        mouse_y = m.y;
        damage_cursor();
        if (dock_hit(mouse_x, mouse_y) != old_hot) damage_dock();   /* the name label */
        if (menu_open) {
            int hot = menu_hit(mouse_x, mouse_y);
            if (hot != menu_hot) {
                menu_hot = hot;
                damage_menu();
            }
        }
        if (drag >= 0) {
            window_t *w = &wins[drag];
            damage_window(w);
            w->x = mouse_x - drag_dx;
            w->y = mouse_y - drag_dy;
            if (w->y < MENU_H + TITLE_H) w->y = MENU_H + TITLE_H;
            damage_window(w);
        } else if (capture >= 0 && (m.buttons & MOUSE_BUTTON_LEFT)) {
            send_event(&wins[capture], WS_EV_MOVE, mouse_x - wins[capture].x, mouse_y - wins[capture].y, 0);
        }
    }
    buttons = m.buttons;
    if ((buttons & MOUSE_BUTTON_LEFT) && !(was & MOUSE_BUTTON_LEFT)) press();
    if (!(buttons & MOUSE_BUTTON_LEFT) && (was & MOUSE_BUTTON_LEFT)) {
        if (capture >= 0)
            send_event(&wins[capture], WS_EV_UP, mouse_x - wins[capture].x, mouse_y - wins[capture].y, 0);
        drag = capture = -1;
    }
    if (m.wheel) {
        int k = window_at(mouse_x, mouse_y);
        if (k >= 0) send_event(&wins[k], WS_EV_WHEEL, mouse_x - wins[k].x, mouse_y - wins[k].y, m.wheel);
    }
}

/* ---- Housekeeping -------------------------------------------------------- */

static void tidy(void) {
    int status, pid;
    while ((pid = wait(-1, &status, WAIT_NOHANG)) > 0)
        for (int i = 0; i < DOCK_APPS; i++)
            if (apps[i].pid == pid) {
                apps[i].pid = 0;
                damage_dock();
            }
    for (int k = 0; k < MAX_WINDOWS; k++)            /* windows of programs that ended */
        if (wins[k].used && sigsend(wins[k].pid, 0) < 0) remove_window(k);
    static const char *months[] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                    "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };
    struct os_time t;
    char now[24];
    gettime(&t);
    snprintf(now, sizeof(now), "%d %s  %02d:%02d", t.day, months[(t.month + 11) % 12], t.hour, t.minute);
    if (strcmp(now, clock_text)) {
        strcpy(clock_text, now);
        damage_menu_bar();
    }
}

static void quit(void) {
    for (int k = 0; k < MAX_WINDOWS; k++)
        if (wins[k].used) sigsend(wins[k].pid, SIGTERM);
    for (int i = 0; i < DOCK_APPS; i++)
        if (apps[i].pid > 0) sigsend(apps[i].pid, SIGTERM);
    gfx_end();
    exit(0);
}

int main(int argc, char **argv) {
    if (gfx_info(&g) < 0) {
        printf("windowserver: the desktop needs a graphics screen\n");
        return 1;
    }
    int port = port_create();
    if (port < 0 || port_register(WS_SERVICE, port) < 0) {
        printf("windowserver: already running\n");
        return 1;
    }
    W = g.width;
    H = g.height;
    S = g.cell_w >= 16 ? 2 : 1;
    MENU_H = 26 * S;
    TITLE_H = 30 * S;
    RADIUS = 11 * S;
    SHADOW = 22 * S;
    unsigned *px = mmap((size_t)W * H * 4, PROT_READ | PROT_WRITE, -1);
    wallpaper = mmap((size_t)H * 4, PROT_READ | PROT_WRITE, -1);
    if (!px || !wallpaper) {
        printf("windowserver: out of memory\n");
        return 1;
    }
    ui_canvas_init(&scr, px, W, H, W);
    for (int y = 0; y < H; y++) {                      /* a blue to violet evening sky */
        int t = (y * 256) / H;
        int r = 0x23 + ((0x9B - 0x23) * t >> 8), gr = 0x4A + ((0x5C - 0x4A) * t >> 8), b = 0xC4 + ((0xC9 - 0xC4) * t >> 8);
        wallpaper[y] = (unsigned)(r << 16 | gr << 8 | b);
    }
    make_cursor();
    for (int i = 0; i < DOCK_APPS; i++) apps[i].size = icon();
    ctrlc(CTRLC_KEY);
    if (gfx_begin_full() < 0) {
        printf("windowserver: cannot take the screen\n");
        return 1;
    }
    printf("windowserver: %dx%d, scale %d, cells %dx%d\n", W, H, S, g.cell_w, g.cell_h);
    mouse_x = W / 2;
    mouse_y = H / 2;
    thread_create(serve, &port);
    for (int i = 1; i < argc; i++)
        for (int k = 0; k < DOCK_APPS; k++)
            if (!strcmp(argv[i], apps[k].program)) launch(k);

    unsigned long last_tidy = 0;
    damage(0, 0, W, H);
    for (;;) {
        mutex_lock(&lock);
        handle_mouse();
        int key;
        while ((key = getkey()) != 0) {
            if (key == KEY_QUIT) quit();
            if (key == 27 && menu_open) {             /* Esc closes the menu */
                menu_open = 0;
                damage_menu();
                continue;
            }
            window_t *f = front();
            if (f) send_event(f, WS_EV_KEY, 0, 0, key);
        }
        for (int k = 0; k < MAX_WINDOWS; k++) {
            window_t *w = &wins[k];
            if (w->used && w->s->damage != w->seen) {
                w->seen = w->s->damage;
                if (!w->minimised) damage(w->x, w->y, w->w, w->h);
            }
        }
        animate_dock();
        if (uptime_ms() - last_tidy >= 500) {
            last_tidy = uptime_ms();
            tidy();
        }
        compose();
        mutex_unlock(&lock);
        sleep_ms(5);
    }
}
