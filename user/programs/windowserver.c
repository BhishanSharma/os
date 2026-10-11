// windowserver [app]... - the desktop: owns the screen, the mouse and the
// keyboard, and draws the programs' windows (like macOS's WindowServer).
//
// Programs ask for a window on the "windowserver" port (ws.h, ui.h) and draw
// into a shared memory surface; the server composites the surfaces back to
// front, only where something changed, under a menu bar and above a Dock.
// Windows can be dragged by the title bar, closed and minimised. Clicking a
// Dock icon starts that app (or brings its window back). Ctrl+Q leaves.
#include <stdio.h>
#include <string.h>
#include <os.h>
#include <ui.h>

#define MAX_WINDOWS 16
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
} dock_app_t;

static dock_app_t apps[] = {
    { "Terminal", "terminal", ">_", 0x2B2B2B, 0 },
    { "Files", "finder", "F", 0x2F7BF5, 0 },
    { "About", "about", "i", 0x8E8E93, 0 },
};
#define DOCK_APPS ((int)(sizeof(apps) / sizeof(apps[0])))

static window_t wins[MAX_WINDOWS];
static int order[MAX_WINDOWS], norder;            /* back to front */
static mutex_t lock;
static int next_id = 1, placed;

static struct os_gfx_info g;
static int W, H, TOP, S, MENU_H, TITLE_H;
static ui_canvas scr;                             /* the back buffer */
static unsigned *wallpaper;                       /* one colour per row */
static int dx0, dy0, dx1, dy1;                    /* what needs drawing again */
static int mouse_x, mouse_y, buttons;
static int drag = -1, drag_dx, drag_dy, capture = -1;
static char clock_text[8];
static int menu_open, menu_hot = -1;

static const char *menu_items[] = { "About Terminal OS", "New Terminal", "Files", "Quit desktop" };
#define MENU_ITEMS 4

/* ---- Damage ------------------------------------------------------------ */

static void damage(int x, int y, int w, int h) {
    if (w <= 0 || h <= 0) return;
    if (dx1 <= dx0) {
        dx0 = x; dy0 = y; dx1 = x + w; dy1 = y + h;
        return;
    }
    if (x < dx0) dx0 = x;
    if (y < dy0) dy0 = y;
    if (x + w > dx1) dx1 = x + w;
    if (y + h > dy1) dy1 = y + h;
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
    damage(x, y, ww + 4 * S, hh + 4 * S);         /* and its shadow */
}

/* ---- Dock geometry ------------------------------------------------------- */

static int icon(void) { return 44 * S; }
static int pad(void) { return 10 * S; }
static int dock_w(void) { return DOCK_APPS * (icon() + pad()) + pad(); }
static int dock_h(void) { return icon() + 2 * pad(); }
static int dock_x(void) { return (W - dock_w()) / 2; }
static int dock_y(void) { return H - dock_h() - 6 * S; }

static int dock_hit(int x, int y) {
    if (!ui_inside(x, y, dock_x(), dock_y(), dock_w(), dock_h())) return -1;
    for (int i = 0; i < DOCK_APPS; i++)
        if (ui_inside(x, y, dock_x() + pad() + i * (icon() + pad()), dock_y() + pad(), icon(), icon())) return i;
    return -1;
}

/* ---- The menu (under "Terminal OS" in the menu bar) ------------------------ */

static int menu_title_w(void) { return ui_text_width("Terminal OS", S) + 28 * S; }
static int item_h(void) { return 24 * S; }
static int menu_x(void) { return 6 * S; }
static int menu_y(void) { return TOP + MENU_H; }
static int menu_w(void) { return 200 * S; }
static int menu_h(void) { return MENU_ITEMS * item_h() + 8 * S; }

static void damage_menu(void) {
    damage(menu_x(), menu_y(), menu_w() + 4 * S, menu_h() + 4 * S);
    damage(0, TOP, W, MENU_H);
}

static int menu_hit(int x, int y) {
    if (!ui_inside(x, y, menu_x(), menu_y() + 4 * S, menu_w(), MENU_ITEMS * item_h())) return -1;
    return (y - menu_y() - 4 * S) / item_h();
}

static void draw_menu(void) {
    if (!menu_open) return;
    int x = menu_x(), y = menu_y();
    ui_fill(&scr, x + 3 * S, y + 3 * S, menu_w(), menu_h(), 0x30305A);
    ui_fill(&scr, x, y, menu_w(), menu_h(), 0xF6F6F8);
    ui_frame(&scr, x, y, menu_w(), menu_h(), 0xB0B0B8);
    for (int i = 0; i < MENU_ITEMS; i++) {
        int iy = y + 4 * S + i * item_h();
        if (i == menu_hot) ui_fill(&scr, x + 4 * S, iy, menu_w() - 8 * S, item_h(), UI_ACCENT);
        ui_text(&scr, x + 14 * S, iy + (item_h() - 16 * S) / 2, menu_items[i], i == menu_hot ? UI_WHITE : UI_TEXT, S);
    }
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
    damage(0, TOP, W, MENU_H);                     /* the app's name */
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
    damage(0, TOP, W, MENU_H);
}

static int create_window(const struct ws_request *q, struct ws_reply *r) {
    int max_w = W - 2, max_h = H - TOP - MENU_H - TITLE_H - 2;
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
    w->y = TOP + MENU_H + TITLE_H + 24 * S + (placed % 8) * step;
    placed++;
    if (w->x + w->w > W - 1) w->x = W - 1 - w->w;
    if (w->y + w->h > H - 1) w->y = H - 1 - w->h;
    if (w->x < 1) w->x = 1;
    if (w->y < TOP + MENU_H + TITLE_H + 1) w->y = TOP + MENU_H + TITLE_H + 1;
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
                    damage(0, TOP, W, MENU_H);
                }
            mutex_unlock(&lock);
        }
        if (reply >= 0) port_close(reply);
    }
}

/* ---- Drawing ------------------------------------------------------------- */

static const char *arrow[] = {
    "X          ", "XX         ", "X.X        ", "X..X       ", "X...X      ", "X....X     ",
    "X.....X    ", "X......X   ", "X.......X  ", "X........X ", "X.....XXXXX", "X..X..X    ",
    "X.X X..X   ", "XX  X..X   ", "X    X..X  ", "     XXX   ",
};
#define CURSOR_W 11
#define CURSOR_H 16

static void draw_cursor(void) {
    for (int j = 0; j < CURSOR_H; j++)
        for (int i = 0; i < CURSOR_W; i++) {
            char c = arrow[j][i];
            if (c == 'X') ui_fill(&scr, mouse_x + i * S, mouse_y + j * S, S, S, 0x000000);
            else if (c == '.') ui_fill(&scr, mouse_x + i * S, mouse_y + j * S, S, S, 0xFFFFFF);
        }
}

static void draw_window(window_t *w, int focused) {
    int x, y, ww, hh;
    outer(w, &x, &y, &ww, &hh);
    ui_fill(&scr, x + 4 * S, y + 4 * S, ww, hh, 0x30305A);   /* a hard shadow */
    ui_frame(&scr, x, y, ww, hh, focused ? 0x9A9A9A : UI_BORDER);
    ui_fill(&scr, w->x, w->y - TITLE_H, w->w, TITLE_H, focused ? 0xE3E3E3 : 0xF6F6F6);
    ui_fill(&scr, w->x, w->y - 1, w->w, 1, UI_BORDER);
    int cy = w->y - TITLE_H / 2, r = 6 * S;
    ui_circle(&scr, w->x + 14 * S, cy, r, focused ? 0xFF5F57 : 0xCFCFCF);
    ui_circle(&scr, w->x + 34 * S, cy, r, focused ? 0xFEBC2E : 0xCFCFCF);
    ui_circle(&scr, w->x + 54 * S, cy, r, 0xCFCFCF);
    int tw = ui_text_width(w->title, S), tx = w->x + (w->w - tw) / 2;
    if (tx < w->x + 66 * S) tx = w->x + 66 * S;
    ui_text(&scr, tx, w->y - TITLE_H + (TITLE_H - 16 * S) / 2, w->title, focused ? UI_TEXT : UI_DIM, S);
    ui_copy(&scr, w->x, w->y, (const unsigned *)((char *)w->s + WS_HEADER), w->w, w->h, w->w);
}

static void draw_menu_bar(void) {
    ui_fill(&scr, 0, TOP, W, MENU_H, 0xF4F4F6);
    ui_fill(&scr, 0, TOP + MENU_H - 1, W, 1, 0xC0C0C8);
    if (menu_open) ui_fill(&scr, menu_x(), TOP + 2 * S, menu_title_w() - 2 * S, MENU_H - 4 * S, 0xD4D4DC);
    int ty = TOP + (MENU_H - 16 * S) / 2, x = 14 * S;
    ui_text(&scr, x, ty, "Terminal OS", UI_TEXT, S);
    x += ui_text(&scr, x + 1, ty, "Terminal OS", UI_TEXT, S) + 24 * S;    /* bold: twice */
    window_t *f = front();
    if (f) ui_text(&scr, x, ty, f->title, UI_TEXT, S);
    ui_text(&scr, W - 14 * S - ui_text_width(clock_text, S), ty, clock_text, UI_TEXT, S);
}

static void draw_dock(void) {
    int x = dock_x(), y = dock_y();
    ui_round(&scr, x, y, dock_w(), dock_h(), 14 * S, 0xD9DCE6);
    for (int i = 0; i < DOCK_APPS; i++) {
        int ix = x + pad() + i * (icon() + pad()), iy = y + pad();
        ui_round(&scr, ix, iy, icon(), icon(), 10 * S, apps[i].color);
        int gw = ui_text_width(apps[i].glyph, S * 2);
        ui_text(&scr, ix + (icon() - gw) / 2, iy + (icon() - 32 * S) / 2, apps[i].glyph, 0xFFFFFF, S * 2);
        if (apps[i].pid > 0) ui_circle(&scr, ix + icon() / 2, iy + icon() + pad() / 2, 2 * S, 0x333333);
    }
}

static void compose(void) {
    if (dx0 < 0) dx0 = 0;
    if (dy0 < TOP) dy0 = TOP;
    if (dx1 > W) dx1 = W;
    if (dy1 > H) dy1 = H;
    if (dx1 <= dx0 || dy1 <= dy0) {
        dx0 = dx1 = 0;
        return;
    }
    ui_clip(&scr, dx0, dy0, dx1 - dx0, dy1 - dy0);
    for (int y = dy0; y < dy1; y++) ui_fill(&scr, dx0, y, dx1 - dx0, 1, wallpaper[y]);
    window_t *f = front();
    for (int i = 0; i < norder; i++) {
        window_t *w = &wins[order[i]];
        if (!w->minimised) draw_window(w, w == f);
    }
    draw_menu_bar();
    draw_dock();
    draw_menu();
    draw_cursor();
    gfx_blit(dx0, dy0, dx1 - dx0, dy1 - dy0, scr.px + (long)dy0 * scr.stride + dx0, scr.stride);
    dx0 = dx1 = 0;
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
    damage(dock_x(), dock_y(), dock_w(), dock_h());
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
    if (ui_inside(mouse_x, mouse_y, 0, TOP, menu_title_w(), MENU_H)) {
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
        int cy = w->y - TITLE_H / 2, r = 7 * S;
        if (ui_inside(mouse_x, mouse_y, w->x + 14 * S - r, cy - r, 2 * r, 2 * r)) {
            if (sigsend(w->pid, 0) < 0) remove_window(k);
            else send_event(w, WS_EV_CLOSE, 0, 0, 0);
            return;
        }
        if (ui_inside(mouse_x, mouse_y, w->x + 34 * S - r, cy - r, 2 * r, 2 * r)) {
            w->minimised = 1;
            damage_window(w);
            window_t *f = front();
            if (f) raise((int)(f - wins));
            damage(0, TOP, W, MENU_H);
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
        damage(mouse_x, mouse_y, CURSOR_W * S, CURSOR_H * S);
        mouse_x = m.x;
        mouse_y = m.y;
        damage(mouse_x, mouse_y, CURSOR_W * S, CURSOR_H * S);
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
            if (w->y < TOP + MENU_H + TITLE_H) w->y = TOP + MENU_H + TITLE_H;
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
                damage(dock_x(), dock_y(), dock_w(), dock_h());
            }
    for (int k = 0; k < MAX_WINDOWS; k++)            /* windows of programs that ended */
        if (wins[k].used && sigsend(wins[k].pid, 0) < 0) remove_window(k);
    struct os_time t;
    char now[8];
    gettime(&t);
    snprintf(now, sizeof(now), "%02d:%02d", t.hour, t.minute);
    if (strcmp(now, clock_text)) {
        strcpy(clock_text, now);
        damage(0, TOP, W, MENU_H);
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
    TOP = g.top;
    S = g.cell_w >= 16 ? 2 : 1;
    MENU_H = 16 * S + 8 * S;
    TITLE_H = 16 * S + 10 * S;
    unsigned *px = mmap((size_t)W * H * 4, PROT_READ | PROT_WRITE, -1);
    wallpaper = mmap((size_t)H * 4, PROT_READ | PROT_WRITE, -1);
    if (!px || !wallpaper) {
        printf("windowserver: out of memory\n");
        return 1;
    }
    ui_canvas_init(&scr, px, W, H, W);
    for (int y = 0; y < H; y++) {                      /* a blue to purple evening sky */
        int t = (y * 256) / H;
        int r = 0x2A + ((0x8E - 0x2A) * t >> 8), gr = 0x4F + ((0x5A - 0x4F) * t >> 8), b = 0xC8 + ((0xB8 - 0xC8) * t >> 8);
        wallpaper[y] = (unsigned)(r << 16 | gr << 8 | b);
    }
    ctrlc(CTRLC_KEY);
    if (gfx_begin() < 0) {
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
    damage(0, TOP, W, H - TOP);
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
        if (uptime_ms() - last_tidy >= 500) {
            last_tidy = uptime_ms();
            tidy();
        }
        compose();
        mutex_unlock(&lock);
        sleep_ms(10);
    }
}
