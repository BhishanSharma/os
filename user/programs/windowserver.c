// windowserver [app]... - the desktop: owns the screen, the mouse and the
// keyboard, and draws the programs' windows (like macOS's WindowServer).
//
// Programs ask for a window on the "windowserver" port (ws.h, ui.h) and draw
// into a shared memory surface; the server composites the surfaces back to
// front, only where something changed (a list of damaged rectangles).
//
// Design follows the macOS HIG specification:
//   * semantic colour tokens with a light and a dark appearance (theme_t),
//   * an 8-point spacing grid and a radius scale (xsmall 2 ... xxlarge 16),
//   * spring motion (response 0.55 s, damping 0.75 as the SwiftUI default;
//     a stiffer spring for the Dock), with Reduce Motion turning it off,
//   * a blur material (backdrop sampled at 1/4 resolution, 10 pt bleed,
//     saturation boost, tint) for the Dock and menus; Reduce Transparency
//     replaces it with a solid colour,
//   * traffic lights 12 pt wide, 8 pt apart, with glyphs on hover,
//   * menus with separators, shortcuts, disabled items and key navigation.
//
// Clicking a Dock icon starts that app (or brings its window back).
// Ctrl+W closes the front window. Ctrl+Q leaves.
#include <stdio.h>
#include <string.h>
#include <os.h>
#include <ui.h>

#define MAX_WINDOWS 16
#define MAX_DAMAGE 32
#define KEY_QUIT 17                    /* Ctrl+Q */
#define KEY_CLOSE 23                   /* Ctrl+W */

/* ---- Design tokens ---------------------------------------------------------
 * Sizes are in points; multiply by S (1, or 2 on a large screen) to draw.
 * Spacing: 4, 8, 16, 24, 32 (the 8-point grid). Radii: */
#define R_XSMALL 2
#define R_SMALL  4
#define R_MEDIUM 6
#define R_LARGE  8
#define R_WINDOW 10
#define R_XXL    16

typedef struct {
    unsigned label, secondary, accent;
    unsigned bar_tint; int bar_alpha;                   /* menu bar */
    unsigned hl; int hl_alpha;                          /* the open menu's title */
    unsigned menu_tint; int menu_alpha; unsigned menu_solid;
    unsigned dock_tint; int dock_alpha; unsigned dock_solid;
    unsigned hair; int hair_alpha;                      /* hairline round a menu */
    unsigned dock_rim; int dock_rim_alpha;
    unsigned sep; int sep_alpha;                        /* separators */
    unsigned title_on, title_off, title_text_on, title_text_off;
    unsigned edge_on, edge_off;                         /* window outline */
    unsigned light_off;                                 /* an inactive traffic light */
    unsigned tooltip;
    unsigned sky_top, sky_bottom;
    int shadow_on, shadow_off;
} theme_t;

static const theme_t themes[2] = {
    [0] = {                                             /* light */
        .label = 0x1D1D1F, .secondary = 0x6E6E73, .accent = 0x007AFF,
        .bar_tint = 0xF6F6F8, .bar_alpha = 175,
        .hl = 0x000000, .hl_alpha = 30,
        .menu_tint = 0xF6F6F8, .menu_alpha = 185, .menu_solid = 0xF2F2F4,
        .dock_tint = 0xFFFFFF, .dock_alpha = 70, .dock_solid = 0xE6E6EA,
        .hair = 0x000000, .hair_alpha = 45,
        .dock_rim = 0xFFFFFF, .dock_rim_alpha = 110,
        .sep = 0x000000, .sep_alpha = 40,
        .title_on = 0xE9E9EB, .title_off = 0xF3F3F5, .title_text_on = 0x3A3A3C, .title_text_off = 0xA0A0A6,
        .edge_on = 0x9C9CA2, .edge_off = 0xC6C6CA,
        .light_off = 0xD2D2D6, .tooltip = 0x2A2A30,
        .sky_top = 0x234AC4, .sky_bottom = 0x9B5CC9,
        .shadow_on = 110, .shadow_off = 60,
    },
    [1] = {                                             /* dark */
        .label = 0xF5F5F7, .secondary = 0x98989D, .accent = 0x0A84FF,
        .bar_tint = 0x1E1E20, .bar_alpha = 150,
        .hl = 0xFFFFFF, .hl_alpha = 40,
        .menu_tint = 0x2A2A2C, .menu_alpha = 190, .menu_solid = 0x2C2C2E,
        .dock_tint = 0x2C2C2E, .dock_alpha = 95, .dock_solid = 0x38383A,
        .hair = 0xFFFFFF, .hair_alpha = 38,
        .dock_rim = 0xFFFFFF, .dock_rim_alpha = 50,
        .sep = 0xFFFFFF, .sep_alpha = 38,
        .title_on = 0x3A3A3C, .title_off = 0x2C2C2E, .title_text_on = 0xE5E5E7, .title_text_off = 0x7A7A7E,
        .edge_on = 0x636366, .edge_off = 0x48484A,
        .light_off = 0x4A4A4E, .tooltip = 0x1C1C1E,
        .sky_top = 0x0F1F5C, .sky_bottom = 0x4A2B72,
        .shadow_on = 150, .shadow_off = 90,
    },
};

static int dark, reduce_motion, reduce_transp;
#define TH (themes[dark])

/* ---- Springs ---------------------------------------------------------------
 * Fixed point (no floating point in programs): positions in 1/1024 pixel.
 * a = k (target - x) - c v, with k = (2 pi / response)^2 and
 * c = 2 * damping * (2 pi / response). Default: response 0.55, damping 0.75
 * -> k 130, c 17. The Dock uses a stiffer one (response 0.21): k 900, c 45. */
#define FP 1024
typedef struct { long pos, vel; } spring_t;

static int spring_step(spring_t *s, long target, int dt_ms, int k, int c) {
    while (dt_ms > 0) {
        int d = dt_ms > 4 ? 4 : dt_ms;
        dt_ms -= d;
        long acc = (long)k * (target - s->pos) - (long)c * s->vel;
        s->vel += acc * d / 1000;
        s->pos += s->vel * d / 1000;
    }
    long diff = target - s->pos, v = s->vel;
    if (diff < 0) diff = -diff;
    if (v < 0) v = -v;
    if (diff < FP / 2 && v < 2 * FP) {
        s->pos = target;
        s->vel = 0;
        return 0;
    }
    return 1;
}

/* ---- Types ------------------------------------------------------------------ */

typedef struct {
    int used, id, pid;
    int x, y, w, h;                    /* the content on the screen */
    int minimised;
    unsigned seen;                     /* the damage count drawn last */
    char title[48];
    struct ws_surface *s;
} window_t;

typedef struct {
    const char *name, *program, *glyph, *icon;
    unsigned color;
    int pid;
    int size;                          /* now, in pixels (it grows under the pointer) */
    spring_t sp;                       /* the size, as a spring */
    long bounce, bvel;                 /* the launch bounce, 1/1024 px */
    int launching;
    unsigned long launch_at;
} dock_app_t;

static dock_app_t apps[] = {
    { "Files", "finder", "F", "folder", 0x2F7BF5, 0, 0, { 0, 0 }, 0, 0, 0, 0 },
    { "Terminal", "terminal", ">_", "terminal", 0x30323A, 0, 0, { 0, 0 }, 0, 0, 0, 0 },
    { "TextEdit", "textedit", "T", "textedit", 0xF2F2F2, 0, 0, { 0, 0 }, 0, 0, 0, 0 },
    { "Paint", "paint", "P", "paint", 0xFF9500, 0, 0, { 0, 0 }, 0, 0, 0, 0 },
    { "Activity Monitor", "activity", "A", "activity", 0x34C759, 0, 0, { 0, 0 }, 0, 0, 0, 0 },
    { "System Settings", "settings", "S", "gear", 0x8E8E93, 0, 0, { 0, 0 }, 0, 0, 0, 0 },
};
#define DOCK_APPS ((int)(sizeof(apps) / sizeof(apps[0])))

enum { M_ABOUT = 1, M_SETTINGS, M_ACTIVITY, M_APPEARANCE, M_MOTION, M_TRANSP, M_TERMINAL,
       M_RESTART, M_SHUTDOWN, M_LOGOUT, W_MINIMIZE, W_CLOSE, W_FRONT = 100 };
#define MI_SEP 1
#define MI_OFF 2
#define MI_MAX 24
typedef struct {
    char label[44], key[10];
    int id, flags;
} mitem_t;

/* ---- State ------------------------------------------------------------------ */

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
static int unpresented;                          /* blitted, not yet shown (page flipping) */
static int tl_hover = -1;                        /* window whose traffic lights show glyphs */
static int last_title_win = -1;
static unsigned long last_title_click;

static mitem_t mitems[MI_MAX];
static int n_mitems, menu_open, menu_hot = -1, menu_wpx;   /* menu_open: 0 none, 1 Terminal OS, 2 Window */
static int bar_x0[3], bar_x1[3];                  /* the menu titles in the bar */

/* The menu bar's right side: battery, Wi-Fi, sound (they open Settings). */
static int bat = -1, wifi_on = -1, vol_level = -1;
static int status_x[3];                           /* where each one starts */

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

static void damage_title(window_t *w) { damage(w->x, w->y - TITLE_H, w->w, TITLE_H); }
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
    damage((W - widest) / 2 - 4 * S, dock_bottom() - icon() * 2 - 40 * S, widest + 8 * S, icon() * 2 + 40 * S + 8 * S);
}

static int dock_hit(int x, int y) {
    if (y < dock_bottom() - dock_h() - icon() || y >= dock_bottom()) return -1;
    for (int i = 0; i < DOCK_APPS; i++) {
        int ix = icon_x(i), iy = dock_bottom() - pad() - apps[i].size;
        if (ui_inside(x, y, ix, iy, apps[i].size, apps[i].size)) return i;
    }
    return -1;
}

static int app_has_window(int i) {
    if (apps[i].pid <= 0) return 0;
    for (int k = 0; k < MAX_WINDOWS; k++)
        if (wins[k].used && wins[k].pid == apps[i].pid) return 1;
    return 0;
}

/* Icons grow towards the pointer on a spring (response ~0.2 s); a launching
 * app bounces until its window appears. Reduce Motion: no spring, no bounce.
 * -> 1 while still moving. */
static int animate_dock(int dt) {
    int over = mouse_y >= dock_bottom() - dock_h() - icon() / 2 && mouse_y < dock_bottom() &&
               mouse_x >= dock_x() - icon() && mouse_x < dock_x() + dock_w() + icon();
    int moving = 0;
    unsigned long now = uptime_ms();
    for (int i = 0; i < DOCK_APPS; i++) {
        dock_app_t *a = &apps[i];
        int target = icon();
        if (over && !menu_open) {
            int centre = icon_x(i) + a->size / 2, d = mouse_x - centre;
            if (d < 0) d = -d;
            int reach = icon() * 5 / 2;
            if (d < reach) target += icon() * 7 / 10 * (reach - d) / reach;
        }
        if (reduce_motion) {
            a->sp.pos = (long)target * FP;
            a->sp.vel = 0;
        } else if (spring_step(&a->sp, (long)target * FP, dt, 900, 45)) {
            moving = 1;
        }
        int size = (int)(a->sp.pos / FP);
        if (size != a->size) {
            a->size = size;
            moving = 1;
        }
        if (a->launching) {
            if (reduce_motion || now - a->launch_at > 8000 || app_has_window(i)) {
                a->launching = 0;
                a->bounce = a->bvel = 0;
            } else {                                   /* hops 20 pt high, about 4 per second */
                a->bvel -= 2400L * S * FP * dt / 1000;
                a->bounce += a->bvel * dt / 1000;
                if (a->bounce <= 0) {
                    a->bounce = 0;
                    a->bvel = 310L * S * FP;
                }
            }
            moving = 1;
        }
    }
    if (moving) damage_dock();
    return moving;
}

/* ---- Menus (the Terminal OS menu and the Window menu) ----------------------- */

static int mi_h(int i) { return (mitems[i].flags & MI_SEP) ? 9 * S : 22 * S; }
static int menu_x(void) { return bar_x0[menu_open ? menu_open : 1]; }
static int menu_y(void) { return MENU_H; }
static int menu_w(void) { return menu_wpx; }
static int menu_h(void) {
    int h = 10 * S;
    for (int i = 0; i < n_mitems; i++) h += mi_h(i);
    return h;
}
static int menu_item_y(int i) {
    int y = menu_y() + 5 * S;
    for (int k = 0; k < i; k++) y += mi_h(k);
    return y;
}

static void add_item(const char *label, const char *key, int id, int flags) {
    if (n_mitems == MI_MAX) return;
    mitem_t *m = &mitems[n_mitems++];
    snprintf(m->label, sizeof(m->label), "%s", label);
    snprintf(m->key, sizeof(m->key), "%s", key ? key : "");
    m->id = id;
    m->flags = flags;
}
static void add_sep(void) { add_item("", 0, 0, MI_SEP); }

static window_t *front(void) {
    for (int i = norder - 1; i >= 0; i--)
        if (!wins[order[i]].minimised) return &wins[order[i]];
    return 0;
}

static void build_menu(int kind) {
    char text[44];
    n_mitems = 0;
    if (kind == 1) {
        add_item("About Terminal OS", 0, M_ABOUT, 0);
        add_sep();
        add_item("System Settings...", 0, M_SETTINGS, 0);
        add_item("Activity Monitor", 0, M_ACTIVITY, 0);
        add_sep();
        snprintf(text, sizeof(text), "Appearance: %s", dark ? "Dark" : "Light");
        add_item(text, 0, M_APPEARANCE, 0);
        snprintf(text, sizeof(text), "Reduce Motion: %s", reduce_motion ? "On" : "Off");
        add_item(text, 0, M_MOTION, 0);
        snprintf(text, sizeof(text), "Reduce Transparency: %s", reduce_transp ? "On" : "Off");
        add_item(text, 0, M_TRANSP, 0);
        add_sep();
        add_item("New Terminal", 0, M_TERMINAL, 0);
        add_sep();
        add_item("Restart...", 0, M_RESTART, 0);
        add_item("Shut Down...", 0, M_SHUTDOWN, 0);
        add_sep();
        add_item("Log Out", "Ctrl+Q", M_LOGOUT, 0);
    } else {
        int has = front() != 0;
        add_item("Minimize", 0, W_MINIMIZE, has ? 0 : MI_OFF);
        add_item("Close Window", "Ctrl+W", W_CLOSE, has ? 0 : MI_OFF);
        int listed = 0;
        for (int k = 0; k < MAX_WINDOWS; k++) {
            if (!wins[k].used) continue;
            if (!listed++) add_sep();
            add_item(wins[k].title, 0, W_FRONT + k, 0);
        }
    }
    int maxl = 0, maxk = 0;
    for (int i = 0; i < n_mitems; i++) {
        if (mitems[i].flags & MI_SEP) continue;
        int lw = ui_text_width(mitems[i].label, S), kw = ui_text_width(mitems[i].key, S);
        if (lw > maxl) maxl = lw;
        if (kw > maxk) maxk = kw;
    }
    menu_wpx = 16 * S + maxl + (maxk ? 24 * S + maxk : 0) + 16 * S;
    if (menu_wpx < 190 * S) menu_wpx = 190 * S;
}

static void damage_menu(void) {
    damage(menu_x() - SHADOW, menu_y() - SHADOW, menu_w() + 2 * SHADOW, menu_h() + 2 * SHADOW);
    damage_menu_bar();
}

static void open_menu(int kind) {
    build_menu(kind);
    menu_open = kind;
    menu_hot = -1;
    damage_menu();
}

static void close_menu(void) {
    if (!menu_open) return;
    damage_menu();
    menu_open = 0;
    menu_hot = -1;
}

static int menu_hit(int x, int y) {
    if (!menu_open || !ui_inside(x, y, menu_x(), menu_y(), menu_w(), menu_h())) return -1;
    for (int i = 0; i < n_mitems; i++) {
        int iy = menu_item_y(i);
        if (y >= iy && y < iy + mi_h(i)) return (mitems[i].flags & MI_SEP) ? -1 : i;
    }
    return -1;
}

static int bar_title_at(int x) {
    for (int k = 1; k <= 2; k++)
        if (x >= bar_x0[k] && x < bar_x1[k]) return k;
    return 0;
}

/* ---- Windows ------------------------------------------------------------- */

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
    if (menu_open == 2) close_menu();
    damage_window(w);
    munmap(w->s);
    w->used = 0;
    int j = 0;
    for (int i = 0; i < norder; i++)
        if (order[i] != k) order[j++] = order[i];
    norder = j;
    if (drag == k) drag = -1;
    if (capture == k) capture = -1;
    if (tl_hover == k) tl_hover = -1;
    if (last_title_win == k) last_title_win = -1;
    window_t *f = front();
    if (f) raise((int)(f - wins));
    damage_menu_bar();
}

static void close_window_request(int k) {
    window_t *w = &wins[k];
    if (sigsend(w->pid, 0) < 0) remove_window(k);
    else send_event(w, WS_EV_CLOSE, 0, 0, 0);
}

static void minimise_window(int k) {
    window_t *w = &wins[k];
    w->minimised = 1;
    if (tl_hover == k) tl_hover = -1;
    damage_window(w);
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
    if (menu_open == 2) close_menu();
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

/* The name the menu bar shows: the Dock app the window belongs to, else its title. */
static const char *app_name(const window_t *w) {
    for (int i = 0; i < DOCK_APPS; i++)
        if (apps[i].pid > 0 && apps[i].pid == w->pid) return apps[i].name;
    return w->title;
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

static int kernel_cursor;             /* the kernel draws the pointer (smooth, 1 kHz) */

/* The pointer as one 0xAARRGGBB picture with a soft shadow, for the kernel:
 * it then moves the pointer itself, the moment the mouse reports. */
static void hand_over_cursor(void) {
    static unsigned argb[64 * 64];
    int cw = CURSOR_W * S, ch = CURSOR_H * S, pw = cw + 3 * S, ph = ch + 4 * S;
    if (pw > 64 || ph > 64) return;
    for (int j = 0; j < ph; j++)
        for (int i = 0; i < pw; i++) {
            /* the shadow: the arrow's coverage, blurred, 1 down and 2 right */
            int sum = 0, n = 0;
            for (int dj = -S; dj <= S; dj++)
                for (int di = -S; di <= S; di++) {
                    int si = i - S + di, sj = j - 2 * S + dj;
                    if (si >= 0 && sj >= 0 && si < cw && sj < ch) sum += cursor_black[sj * cw + si];
                    n++;
                }
            unsigned shadow = (unsigned)(sum / n) * 70 / 255;
            unsigned b = i < cw && j < ch ? cursor_black[j * cw + i] : 0;
            unsigned w = i < cw && j < ch ? cursor_white[j * cw + i] : 0;
            /* black over the shadow, then white over that (grey levels) */
            unsigned a = b + shadow * (255 - b) / 255;
            unsigned grey = a ? 255 * w / (a > w ? a : w) : 0;
            if (w > a) a = w;
            argb[j * pw + i] = a << 24 | grey << 16 | grey << 8 | grey;
        }
    kernel_cursor = gfx_cursor(pw, ph, 0, 0, argb) == 0;
}

static void damage_cursor(void) {
    if (!kernel_cursor) damage(mouse_x - S, mouse_y - S, (CURSOR_W + 2) * S, (CURSOR_H + 2) * S);
}

/* ---- Wallpaper ------------------------------------------------------------ */

static void make_wallpaper(void) {
    unsigned a = TH.sky_top, b = TH.sky_bottom;
    for (int y = 0; y < H; y++) {
        int t = (y * 256) / H;
        int r = (int)(a >> 16 & 255) + ((int)(b >> 16 & 255) - (int)(a >> 16 & 255)) * t / 256;
        int gr = (int)(a >> 8 & 255) + ((int)(b >> 8 & 255) - (int)(a >> 8 & 255)) * t / 256;
        int bl = (int)(a & 255) + ((int)(b & 255) - (int)(a & 255)) * t / 256;
        wallpaper[y] = (unsigned)(r << 16 | gr << 8 | bl);
    }
}

/* ---- The blur material -------------------------------------------------------
 * Like NSVisualEffectView's backdrop layer: the picture behind is sampled at a
 * quarter of its resolution (scale 0.25), with a 10 pt bleed so the edges have
 * context, blurred (four 3x3 box passes), saturated and tinted, then scaled up.
 * Only the wallpaper and the windows are sampled (not the Dock itself). */
#define SAMP_W 512
#define SAMP_H 320
#define MAT_MAX (1 << 19)
static unsigned samp[SAMP_W * SAMP_H], samp2[SAMP_W * SAMP_H];
static unsigned mat[MAT_MAX];
static int xi[2048];
static unsigned xf[2048];

static int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

static unsigned backdrop_at(int x, int y) {
    for (int i = norder - 1; i >= 0; i--) {
        window_t *w = &wins[order[i]];
        if (w->minimised) continue;
        if (ui_inside(x, y, w->x, w->y, w->w, w->h))
            return ((const unsigned *)((char *)w->s + WS_HEADER))[(long)(y - w->y) * w->w + (x - w->x)];
        if (ui_inside(x, y, w->x, w->y - TITLE_H, w->w, TITLE_H)) return TH.title_on;
    }
    return wallpaper[clampi(y, 0, H - 1)];
}

static void box3(const unsigned *src, unsigned *dst, int bw, int bh, int horizontal) {
    for (int y = 0; y < bh; y++)
        for (int x = 0; x < bw; x++) {
            unsigned r = 0, gr = 0, b = 0;
            for (int d = -1; d <= 1; d++) {
                int xx = clampi(x + (horizontal ? d : 0), 0, bw - 1);
                int yy = clampi(y + (horizontal ? 0 : d), 0, bh - 1);
                unsigned c = src[yy * bw + xx];
                r += c >> 16 & 255;
                gr += c >> 8 & 255;
                b += c & 255;
            }
            dst[y * bw + x] = (r / 3) << 16 | (gr / 3) << 8 | (b / 3);
        }
}

static unsigned lerp_rgb(unsigned a, unsigned b, unsigned f) {
    unsigned r = (((a >> 16) & 255) * (256 - f) + ((b >> 16) & 255) * f) >> 8;
    unsigned gr = (((a >> 8) & 255) * (256 - f) + ((b >> 8) & 255) * f) >> 8;
    unsigned bl = ((a & 255) * (256 - f) + (b & 255) * f) >> 8;
    return r << 16 | gr << 8 | bl;
}

static unsigned saturate_tint(unsigned c, int sat, unsigned tint, int alpha) {
    int r = (int)(c >> 16 & 255), gr = (int)(c >> 8 & 255), b = (int)(c & 255);
    int gray = (r * 30 + gr * 59 + b * 11) / 100;
    r = clampi(gray + (r - gray) * sat / 100, 0, 255);
    gr = clampi(gray + (gr - gray) * sat / 100, 0, 255);
    b = clampi(gray + (b - gray) * sat / 100, 0, 255);
    r = (r * (255 - alpha) + (int)(tint >> 16 & 255) * alpha) / 255;
    gr = (gr * (255 - alpha) + (int)(tint >> 8 & 255) * alpha) / 255;
    b = (b * (255 - alpha) + (int)(tint & 255) * alpha) / 255;
    return (unsigned)(r << 16 | gr << 8 | b);
}

/* A frosted rounded rectangle. Reduce Transparency (or a size too big to
 * sample): the solid colour instead. */
static void material(int x, int y, int w, int h, int r, int corners, unsigned tint, int tint_a, unsigned solid) {
    if (x + w <= scr.cx0 || x >= scr.cx1 || y + h <= scr.cy0 || y >= scr.cy1) return;
    int bleed = 10 * S, bw = (w + 2 * bleed + 3) / 4, bh = (h + 2 * bleed + 3) / 4;
    if (reduce_transp || w > 2048 || bw > SAMP_W || bh > SAMP_H || (long)w * h > MAT_MAX) {
        ui_round_ex(&scr, x, y, w, h, r, solid, 255, corners);
        return;
    }
    for (int cy = 0; cy < bh; cy++)
        for (int cx = 0; cx < bw; cx++)
            samp[cy * bw + cx] = backdrop_at(x - bleed + cx * 4 + 2, y - bleed + cy * 4 + 2);
    box3(samp, samp2, bw, bh, 1);
    box3(samp2, samp, bw, bh, 0);
    box3(samp, samp2, bw, bh, 1);
    box3(samp2, samp, bw, bh, 0);
    for (int i = 0; i < bw * bh; i++) samp[i] = saturate_tint(samp[i], 180, tint, tint_a);
    for (int i = 0; i < w; i++) {                          /* bilinear, columns once */
        int u = (i + bleed) * 64 - 96, c0 = u >> 8;
        unsigned f = (unsigned)(u & 255);
        if (c0 < 0) { c0 = 0; f = 0; }
        if (c0 >= bw - 1) { c0 = bw - 1; f = 0; }
        xi[i] = c0;
        xf[i] = f;
    }
    for (int j = 0; j < h; j++) {
        int v = (j + bleed) * 64 - 96, r0 = v >> 8;
        unsigned rf = (unsigned)(v & 255);
        if (r0 < 0) { r0 = 0; rf = 0; }
        if (r0 >= bh - 1) { r0 = bh - 1; rf = 0; }
        const unsigned *a = samp + r0 * bw, *b = rf ? a + bw : a;
        for (int i = 0; i < w; i++) {
            int c = xi[i];
            unsigned f = xf[i];
            unsigned top = f ? lerp_rgb(a[c], a[c + 1], f) : a[c];
            unsigned bot = f ? lerp_rgb(b[c], b[c + 1], f) : b[c];
            mat[j * w + i] = rf ? lerp_rgb(top, bot, rf) : top;
        }
    }
    ui_copy_round(&scr, x, y, mat, w, h, w, r, corners);
}

/* ---- Drawing ------------------------------------------------------------- */

/* Traffic lights: 12 pt wide, 8 pt apart. Glyphs show while the pointer is
 * over the group (macOS), and the lights are grey when the window is not
 * the front one (unless hovered). */
static void traffic_lights(window_t *w, int focused, int hover) {
    static const unsigned fill[3] = { 0xFF5F57, 0xFEBC2E, 0x28C840 };
    static const unsigned ring[3] = { 0xE0443E, 0xDEA123, 0x1AAB29 };
    static const unsigned ink[3] = { 0x4D0000, 0x995700, 0x006500 };
    int cy = w->y - TITLE_H / 2, r = 6 * S, lit = focused || hover;
    for (int i = 0; i < 3; i++) {
        int cx = w->x + (20 + 20 * i) * S;
        ui_circle(&scr, cx, cy, r, lit ? ring[i] : TH.edge_off);
        ui_circle(&scr, cx, cy, r - 1, lit ? fill[i] : TH.light_off);
        if (!hover) continue;
        if (i == 0) {
            for (int t = -2 * S; t <= 2 * S; t++) {
                ui_fill(&scr, cx + t, cy + t, S, S, ink[0]);
                ui_fill(&scr, cx + t, cy - t, S, S, ink[0]);
            }
        } else if (i == 1) {
            ui_fill(&scr, cx - 3 * S, cy, 6 * S, S, ink[1]);
        } else {
            ui_fill(&scr, cx - 3 * S, cy, 6 * S, S, ink[2]);
            ui_fill(&scr, cx, cy - 3 * S, S, 6 * S, ink[2]);
        }
    }
}

static void draw_window(window_t *w, int focused) {
    int x, y, ww, hh;
    outer(w, &x, &y, &ww, &hh);
    ui_shadow(&scr, x + 2 * S, y + SHADOW / 3, ww - 4 * S, hh - 2 * S, RADIUS, SHADOW, focused ? TH.shadow_on : TH.shadow_off);
    ui_shadow(&scr, x, y, ww, hh, RADIUS, 2 * S, 50);                              /* a tight edge */
    ui_round(&scr, x, y, ww, hh, RADIUS, focused ? TH.edge_on : TH.edge_off);       /* the outline */
    ui_round_ex(&scr, w->x, w->y - TITLE_H, w->w, TITLE_H, RADIUS - 1, focused ? TH.title_on : TH.title_off,
                255, UI_TL | UI_TR);
    ui_fill_a(&scr, w->x, w->y - 1, w->w, 1, TH.sep, TH.sep_alpha);
    traffic_lights(w, focused, tl_hover >= 0 && &wins[tl_hover] == w);
    int tw = ui_text_style_width(w->title, S, UI_BOLD), tx = w->x + (w->w - tw) / 2;
    if (tx < w->x + 76 * S) tx = w->x + 76 * S;
    ui_text_bold(&scr, tx, w->y - TITLE_H + (TITLE_H - 16 * S) / 2, w->title,
                 focused ? TH.title_text_on : TH.title_text_off, S);
    ui_copy_round(&scr, w->x, w->y, (const unsigned *)((char *)w->s + WS_HEADER), w->w, w->h, w->w,
                  RADIUS - 1, UI_BL | UI_BR);
}

static void draw_menu_bar(void) {
    ui_fill_a(&scr, 0, 0, W, MENU_H, TH.bar_tint, TH.bar_alpha);           /* translucent */
    ui_fill_a(&scr, 0, MENU_H - 1, W, 1, TH.sep, TH.sep_alpha);
    int ty = (MENU_H - 16 * S) / 2, x = 14 * S;
    int tw = ui_text_style_width("Terminal OS", S, UI_BOLD);
    bar_x0[1] = x - 8 * S;
    bar_x1[1] = x + tw + 8 * S;
    if (menu_open == 1)
        ui_round_ex(&scr, bar_x0[1], 2 * S, bar_x1[1] - bar_x0[1], MENU_H - 4 * S, R_SMALL * S, TH.hl, TH.hl_alpha, UI_ALL);
    ui_text_bold(&scr, x, ty, "Terminal OS", TH.label, S);
    window_t *f = front();
    x = bar_x1[1] + 8 * S;
    x += ui_text_bold(&scr, x, ty, f ? app_name(f) : "Desktop", TH.label, S) + 16 * S;
    int ww = ui_text_width("Window", S);
    bar_x0[2] = x - 8 * S;
    bar_x1[2] = x + ww + 8 * S;
    if (menu_open == 2)
        ui_round_ex(&scr, bar_x0[2], 2 * S, bar_x1[2] - bar_x0[2], MENU_H - 4 * S, R_SMALL * S, TH.hl, TH.hl_alpha, UI_ALL);
    ui_text(&scr, x, ty, "Window", TH.label, S);

    int rx = W - 16 * S - ui_text_width(clock_text, S);
    ui_text(&scr, rx, ty, clock_text, TH.label, S);
    int is = 18 * S, iy = (MENU_H - is) / 2;
    status_x[0] = status_x[1] = status_x[2] = 0;
    rx -= 18 * S;
    if (vol_level >= 0) {                              /* sound */
        rx -= is;
        status_x[2] = rx;
        ui_icon(&scr, rx, iy, is, "sound", vol_level ? 255 : 90);
        rx -= 12 * S;
    }
    if (wifi_on >= 0) {                                /* Wi-Fi: faded when not connected */
        rx -= is;
        status_x[1] = rx;
        ui_icon(&scr, rx, iy, is, "wifi", wifi_on ? 255 : 80);
        rx -= 12 * S;
    }
    if (bat >= 0) {                                    /* battery and percent */
        char pct[8];
        snprintf(pct, sizeof(pct), "%d%%", bat & 0xFF);
        int pw = ui_text_width(pct, S);
        rx -= pw;
        ui_text(&scr, rx, ty, pct, TH.label, S);
        rx -= is + 4 * S;
        status_x[0] = rx;
        ui_icon(&scr, rx, iy, is, "battery", 255);
    }
}

static void draw_dock(void) {
    int x = dock_x(), w = dock_w(), y = dock_bottom() - dock_h(), r = R_XXL * S;
    ui_shadow_ex(&scr, x, y + 2 * S, w, dock_h(), r, 12 * S, 60, 1);          /* not under the glass */
    ui_round_ex(&scr, x - 1, y - 1, w + 2, dock_h() + 2, r + 1, TH.dock_rim, TH.dock_rim_alpha, UI_ALL);
    material(x, y, w, dock_h(), r, UI_ALL, TH.dock_tint, TH.dock_alpha, TH.dock_solid);
    int hot = dock_hit(mouse_x, mouse_y);
    for (int i = 0; i < DOCK_APPS; i++) {
        dock_app_t *a = &apps[i];
        int size = a->size, ix = icon_x(i), iy = dock_bottom() - pad() - size - (int)(a->bounce / FP), rr = size * 22 / 100;
        if (ui_icon(&scr, ix, iy, size, a->icon, 255) != 0) {   /* no icon file: a drawn one */
            ui_shadow(&scr, ix, iy + size / 30, size, size, rr, size / 12 + 1, 70);
            ui_round(&scr, ix, iy, size, size, rr, a->color);
            int ts = size / 22;
            if (ts < 1) ts = 1;
            if (ts > 4) ts = 4;
            int gw = ui_text_style_width(a->glyph, ts, UI_BOLD);
            ui_text_bold(&scr, ix + (size - gw) / 2, iy + (size - 16 * ts) / 2, a->glyph, 0xFFFFFF, ts);
        }
        if (a->pid > 0)                                          /* the running dot */
            ui_circle(&scr, ix + size / 2, dock_bottom() - pad() / 2 + S, 2 * S, dark ? 0xE5E5E7 : 0x3A3A3E);
        if (i == hot && !menu_open) {                          /* the name, above the icon */
            int lw = ui_text_width(a->name, S) + 20 * S, lh = 24 * S;
            int lx = ix + size / 2 - lw / 2, ly = dock_bottom() - dock_h() - lh - 10 * S - (size - icon());
            ui_round_ex(&scr, lx, ly, lw, lh, R_MEDIUM * S, TH.tooltip, 225, UI_ALL);
            ui_text(&scr, lx + 10 * S, ly + (lh - 16 * S) / 2, a->name, 0xFFFFFF, S);
        }
    }
}

static void draw_menu(void) {
    if (!menu_open) return;
    int x = menu_x(), y = menu_y(), w = menu_w(), h = menu_h(), r = R_LARGE * S;
    ui_shadow_ex(&scr, x, y + 4 * S, w, h, r, 14 * S, 90, 1);                  /* blur 28 / 2 */
    ui_round_ex(&scr, x - 1, y - 1, w + 2, h + 2, r + 1, TH.hair, TH.hair_alpha, UI_ALL);
    material(x, y, w, h, r, UI_ALL, TH.menu_tint, TH.menu_alpha, TH.menu_solid);
    for (int i = 0; i < n_mitems; i++) {
        int iy = menu_item_y(i), ih = mi_h(i);
        if (mitems[i].flags & MI_SEP) {
            ui_fill_a(&scr, x + 10 * S, iy + ih / 2, w - 20 * S, 1, TH.sep, TH.sep_alpha);
            continue;
        }
        int off = mitems[i].flags & MI_OFF, hot = i == menu_hot && !off;
        if (hot) ui_round(&scr, x + 5 * S, iy, w - 10 * S, ih, R_SMALL * S, TH.accent);
        unsigned fg = hot ? 0xFFFFFF : off ? TH.secondary : TH.label;
        int ty = iy + (ih - 16 * S) / 2;
        ui_text(&scr, x + 16 * S, ty, mitems[i].label, fg, S);
        if (mitems[i].key[0]) {
            int kw = ui_text_width(mitems[i].key, S);
            ui_text(&scr, x + w - 14 * S - kw, ty, mitems[i].key, hot ? 0xFFFFFF : TH.secondary, S);
        }
    }
}

/* -> 1 if the frame was paced by the screen (page flipping): no sleep then. */
static int compose(void) {
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
        if (!kernel_cursor) draw_cursor();
        gfx_blit(x0, y0, x1 - x0, y1 - y0, scr.px + (long)y0 * scr.stride + x0, scr.stride);
    }
    if (ndmg) unpresented = 1;
    ndmg = 0;
    int paced = 0;
    if (unpresented) {
        paced = gfx_present() > 0;                    /* tear-free on Intel graphics */
        unpresented = 0;
    }
    return paced;
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

/* The window whose traffic-light group the pointer is over, or -1. */
static int traffic_hover(void) {
    int k = window_at(mouse_x, mouse_y);
    if (k < 0) return -1;
    window_t *w = &wins[k];
    return ui_inside(mouse_x, mouse_y, w->x + 8 * S, w->y - TITLE_H, 64 * S, TITLE_H) ? k : -1;
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
    if (pid > 0) {
        apps[i].pid = pid;
        apps[i].launching = !reduce_motion;
        apps[i].launch_at = uptime_ms();
        apps[i].bounce = 0;
        apps[i].bvel = 310L * S * FP;
    }
    damage_dock();
}

static void quit(void);

static void run(const char *program, const char *arg) {
    char *argv[] = { (char *)program, (char *)arg, 0 };
    spawn(program, argv, 0);
}

static void menu_choose(int id) {
    if (id == M_ABOUT) run("about", 0);
    else if (id == M_SETTINGS) run("settings", 0);
    else if (id == M_ACTIVITY) run("activity", 0);
    else if (id == M_TERMINAL) run("terminal", 0);
    else if (id == M_APPEARANCE) {
        dark = !dark;
        make_wallpaper();
        damage(0, 0, W, H);
    } else if (id == M_MOTION) {
        reduce_motion = !reduce_motion;
    } else if (id == M_TRANSP) {
        reduce_transp = !reduce_transp;
        damage(0, 0, W, H);
    } else if (id == M_RESTART || id == M_SHUTDOWN) {
        for (int k = 0; k < MAX_WINDOWS; k++)         /* the programs end first (files saved) */
            if (wins[k].used) sigsend(wins[k].pid, SIGTERM);
        sleep_ms(300);
        power(id == M_RESTART);
    } else if (id == M_LOGOUT) {
        quit();
    } else if (id == W_MINIMIZE || id == W_CLOSE) {
        window_t *f = front();
        if (f && id == W_MINIMIZE) minimise_window((int)(f - wins));
        else if (f) close_window_request((int)(f - wins));
    } else if (id >= W_FRONT) {
        int k = id - W_FRONT;
        if (k >= 0 && k < MAX_WINDOWS && wins[k].used) {
            if (wins[k].minimised) {
                wins[k].minimised = 0;
                damage_window(&wins[k]);
            }
            raise(k);
        }
    }
}

/* Keys while a menu is open: Esc, Left/Right (the other menu), Up/Down, Enter. */
static void menu_key(int key) {
    if (key == 27) {
        close_menu();
    } else if (key == KEYCODE_LEFT || key == KEYCODE_RIGHT) {
        int other = menu_open == 1 ? 2 : 1;
        close_menu();
        open_menu(other);
    } else if (key == KEYCODE_UP || key == KEYCODE_DOWN) {
        int step = key == KEYCODE_DOWN ? 1 : -1, i = menu_hot;
        for (int tries = 0; tries <= n_mitems; tries++) {
            i += step;
            if (i < 0) i = n_mitems - 1;
            if (i >= n_mitems) i = 0;
            if (!(mitems[i].flags & (MI_SEP | MI_OFF))) break;
        }
        menu_hot = i;
        damage_menu();
    } else if (key == '\n' || key == '\r') {
        if (menu_hot >= 0) {
            int id = mitems[menu_hot].id;
            close_menu();
            menu_choose(id);
        }
    }
}

static void press(void) {
    printf("windowserver: click at %d,%d\n", mouse_x, mouse_y);   /* (for the tests) */
    if (menu_open) {                                   /* any click closes the menu */
        int i = menu_hit(mouse_x, mouse_y), was = menu_open;
        int t = mouse_y < MENU_H ? bar_title_at(mouse_x) : 0;
        int id = i >= 0 && !(mitems[i].flags & MI_OFF) ? mitems[i].id : 0;
        close_menu();
        if (id) menu_choose(id);
        else if (t && t != was) open_menu(t);
        return;
    }
    if (mouse_y < MENU_H) {                            /* the status icons */
        static const char *panes[] = { "battery", "wifi", "sound" };
        for (int k = 0; k < 3; k++)
            if (status_x[k] && mouse_x >= status_x[k] && mouse_x < status_x[k] + (k ? 26 : 70) * S) {
                run("settings", panes[k]);
                return;
            }
        int t = bar_title_at(mouse_x);
        if (t) {
            open_menu(t);
            return;
        }
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
        if (ui_inside(mouse_x, mouse_y, w->x + 20 * S - r, cy - r, 2 * r, 2 * r)) {
            close_window_request(k);
            return;
        }
        if (ui_inside(mouse_x, mouse_y, w->x + 40 * S - r, cy - r, 2 * r, 2 * r)) {
            minimise_window(k);
            return;
        }
        if (ui_inside(mouse_x, mouse_y, w->x + 60 * S - r, cy - r, 2 * r, 2 * r))
            return;                                    /* zoom: windows cannot be resized yet */
        unsigned long now = uptime_ms();
        if (last_title_win == k && now - last_title_click < 400) {   /* double-click: minimise */
            last_title_win = -1;
            minimise_window(k);
            return;
        }
        last_title_win = k;
        last_title_click = now;
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
            if (hot >= 0 && (mitems[hot].flags & MI_OFF)) hot = -1;
            if (hot != menu_hot) {
                menu_hot = hot;
                damage_menu();
            }
            if (mouse_y < MENU_H) {                    /* sliding along the bar switches menus */
                int t = bar_title_at(mouse_x);
                if (t && t != menu_open) {
                    close_menu();
                    open_menu(t);
                }
            }
        }
        int th = menu_open ? -1 : traffic_hover();
        if (th != tl_hover) {
            if (tl_hover >= 0 && wins[tl_hover].used) damage_title(&wins[tl_hover]);
            tl_hover = th;
            if (th >= 0) damage_title(&wins[th]);
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

/* 0 = Sunday */
static int weekday(int y, int m, int d) {
    static const int t[12] = { 0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4 };
    if (m < 3) y--;
    return (y + y / 4 - y / 100 + y / 400 + t[(m + 11) % 12] + d) % 7;
}

static void tidy(void) {
    int status, pid;
    while ((pid = wait(-1, &status, WAIT_NOHANG)) > 0)
        for (int i = 0; i < DOCK_APPS; i++)
            if (apps[i].pid == pid) {
                apps[i].pid = 0;
                apps[i].launching = 0;
                apps[i].bounce = apps[i].bvel = 0;
                damage_dock();
            }
    for (int k = 0; k < MAX_WINDOWS; k++)            /* windows of programs that ended */
        if (wins[k].used && sigsend(wins[k].pid, 0) < 0) remove_window(k);
    static const char *months[] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                    "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };
    static const char *days[] = { "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat" };
    static unsigned long last_status;
    if (!last_status || uptime_ms() - last_status > 5000) {   /* battery, Wi-Fi, sound */
        last_status = uptime_ms();
        struct os_wifi w;
        int b = battery(), wf = wifi_status(&w), v = volume(-1);
        int nb = b < 0 ? -1 : b, nw = wf < 0 ? -1 : wf, nv = v < 0 ? -1 : v;
        if (nb != bat || nw != wifi_on || nv != vol_level) {
            bat = nb;
            wifi_on = nw;
            vol_level = nv;
            damage_menu_bar();
        }
    }
    struct os_time t;
    char now[24];
    gettime(&t);
    int h12 = t.hour % 12 == 0 ? 12 : t.hour % 12;       /* "Tue 13 Oct 5:54 PM" */
    snprintf(now, sizeof(now), "%s %d %s  %d:%02d %s", days[weekday(t.year, t.month, t.day)], t.day,
             months[(t.month + 11) % 12], h12, t.minute, t.hour < 12 ? "AM" : "PM");
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
    MENU_H = 24 * S;                                   /* the macOS menu bar is 24 pt */
    TITLE_H = 28 * S;
    RADIUS = R_WINDOW * S;
    SHADOW = 24 * S;                                   /* design blur 48 / 2 */
    unsigned *px = mmap((size_t)W * H * 4, PROT_READ | PROT_WRITE, -1);
    wallpaper = mmap((size_t)H * 4, PROT_READ | PROT_WRITE, -1);
    if (!px || !wallpaper) {
        printf("windowserver: out of memory\n");
        return 1;
    }
    ui_canvas_init(&scr, px, W, H, W);
    make_wallpaper();
    make_cursor();
    for (int i = 0; i < DOCK_APPS; i++) {
        apps[i].size = icon();
        apps[i].sp.pos = (long)icon() * FP;
    }
    ctrlc(CTRLC_KEY);
    if (gfx_begin_full() < 0) {
        printf("windowserver: cannot take the screen\n");
        return 1;
    }
    hand_over_cursor();
    printf("windowserver: %dx%d, scale %d, cells %dx%d%s\n", W, H, S, g.cell_w, g.cell_h,
           kernel_cursor ? ", pointer drawn by the kernel" : "");
    mouse_x = W / 2;
    mouse_y = H / 2;
    thread_create(serve, &port);
    for (int i = 1; i < argc; i++) {                   /* apps to open: from the Dock, or any program */
        int k = 0;
        while (k < DOCK_APPS && strcmp(argv[i], apps[k].program)) k++;
        if (k < DOCK_APPS) launch(k);
        else run(argv[i], 0);
    }

    unsigned long last_tidy = 0, last_frame = uptime_ms();
    damage(0, 0, W, H);
    for (;;) {
        mutex_lock(&lock);
        unsigned long now = uptime_ms();
        int dt = (int)(now - last_frame);
        if (dt > 50) dt = 50;
        last_frame = now;
        handle_mouse();
        int key;
        while ((key = getkey()) != 0) {
            if (key == KEY_QUIT) quit();
            if (menu_open) {
                menu_key(key);
                continue;
            }
            window_t *f = front();
            if (key == KEY_CLOSE) {
                if (f) close_window_request((int)(f - wins));
                continue;
            }
            if (f) send_event(f, WS_EV_KEY, 0, 0, key);
        }
        for (int k = 0; k < MAX_WINDOWS; k++) {
            window_t *w = &wins[k];
            if (w->used && w->s->damage != w->seen) {
                w->seen = w->s->damage;
                if (!w->minimised) damage(w->x, w->y, w->w, w->h);
            }
        }
        animate_dock(dt);
        if (uptime_ms() - last_tidy >= 500) {
            last_tidy = uptime_ms();
            tidy();
        }
        int paced = compose();
        mutex_unlock(&lock);
        if (!paced) sleep_ms(5);                      /* else the screen's refresh set the pace */
    }
}