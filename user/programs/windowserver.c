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
//   * menus with separators, shortcuts, disabled items and key navigation,
//   * Increase Contrast (rims at full opacity, darker text and accent),
//   * window motion: open (spring with a 6.8% overshoot, fade 150 ms), close
//     (ease-in, 150 ms) and minimize (250 ms, shrinking towards the Dock);
//     menus and Dock labels fade in over 120 ms. Reduce Motion keeps only
//     the fades.
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
#define R_WINDOW 16                    /* radius.xxlarge: a window is a very large container */
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

static int dark, reduce_motion, reduce_transp, contrast;
static theme_t cur;                                /* the theme in force */
#define TH cur

/* The theme for the current settings. Increase Contrast: rims at full opacity
 * and 1 px, separators and the highlight stronger, secondary text and the
 * accent darker (light) or lighter (dark), a solid menu bar. */
static void apply_theme(void) {
    cur = themes[dark];
    if (!contrast) return;
    cur.hair_alpha = 255;
    cur.dock_rim_alpha = 255;
    cur.sep_alpha = 150;
    cur.hl_alpha += 30;
    cur.bar_alpha = 255;
    cur.secondary = dark ? 0xD1D1D6 : 0x48484A;
    cur.accent = dark ? 0x409CFF : 0x0040DD;
    cur.edge_on = dark ? 0xAEAEB2 : 0x3A3A3C;
    cur.edge_off = dark ? 0x8E8E93 : 0x636366;
    cur.title_text_off = dark ? 0xAEAEB2 : 0x636366;
    cur.hair = dark ? 0xFFFFFF : 0x000000;
    cur.dock_rim = dark ? 0xFFFFFF : 0x000000;
}

/* What apps see in their surface header (ws.h). */
static unsigned prefs_bits(void) {
    return (dark ? WS_PREF_DARK : 0) | (contrast ? WS_PREF_CONTRAST : 0) |
           (reduce_motion ? WS_PREF_MOTION : 0) | (reduce_transp ? WS_PREF_TRANSP : 0);
}

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

/* Window motion. A new window waits (invisible) for its first drawing, then
 * OPENs; closing and minimizing keep the window drawn while it animates. */
enum { ANIM_NONE, ANIM_WAIT, ANIM_OPEN, ANIM_CLOSE, ANIM_MIN, ANIM_RESTORE };
#define OPEN_MS   150                  /* opacity: 150 ms ease-out (the spring runs on) */
#define CLOSE_MS  150                  /* ease-in */
#define MIN_MS    250                  /* standard transition: 200-350 ms */
#define FADE_MS   120                  /* menus and Dock labels: micro-interaction */

typedef struct {
    int used, id, pid;
    int x, y, w, h;                    /* the content on the screen */
    int minimised;
    unsigned seen;                     /* the damage count drawn last */
    char title[48];
    struct ws_surface *s;
    int anim, anim_ms;                 /* ANIM_*, and how long it has run */
    spring_t sc;                       /* ANIM_OPEN: the scale, in percent * FP */
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
<<<<<<< HEAD
    { "Files", "finder", "F", "folder", 0x2F7BF5, 0, 0, { 0, 0 }, 0, 0, 0, 0 },
    { "Terminal", "terminal", ">_", "terminal", 0x30323A, 0, 0, { 0, 0 }, 0, 0, 0, 0 },
    { "TextEdit", "textedit", "T", "textedit", 0xF2F2F2, 0, 0, { 0, 0 }, 0, 0, 0, 0 },
    { "Paint", "paint", "P", "paint", 0xFF9500, 0, 0, { 0, 0 }, 0, 0, 0, 0 },
    { "Activity Monitor", "activity", "A", "activity", 0x34C759, 0, 0, { 0, 0 }, 0, 0, 0, 0 },
    { "System Settings", "settings", "S", "gear", 0x8E8E93, 0, 0, { 0, 0 }, 0, 0, 0, 0 },
=======
    { .name = "Files", .program = "finder", .glyph = "F", .icon = "folder", .color = 0x2F7BF5 },
    { .name = "Terminal", .program = "terminal", .glyph = ">_", .icon = "terminal", .color = 0x30323A },
    { .name = "TextEdit", .program = "textedit", .glyph = "T", .icon = "textedit", .color = 0xF2F2F2 },
    { .name = "Paint", .program = "paint", .glyph = "P", .icon = "paint", .color = 0xFF9500 },
    { .name = "Activity Monitor", .program = "activity", .glyph = "A", .icon = "activity", .color = 0x34C759 },
    { .name = "System Settings", .program = "settings", .glyph = "S", .icon = "gear", .color = 0x8E8E93 },
>>>>>>> a93e441314f3ad33018607b8f615930be945a4dc
};
#define DOCK_APPS ((int)(sizeof(apps) / sizeof(apps[0])))

enum { M_ABOUT = 1, M_SETTINGS, M_ACTIVITY, M_APPEARANCE, M_MOTION, M_TRANSP, M_TERMINAL,
       M_CONTRAST, M_RESTART, M_SHUTDOWN, M_LOGOUT, W_MINIMIZE, W_CLOSE, W_FRONT = 100 };
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

static int menu_fade_ms = FADE_MS;               /* how long the open menu has been fading in */
static int tip_hot = -1, tip_ms;                 /* the Dock label: which icon, how long */
static unsigned frame_no;                        /* compose calls so far */

/* Off-screen buffers (anim_cap pixels each): a window drawn on its own, its
 * shape, and what was behind a fading menu. Missing: no window motion. */
static unsigned *anim_col, *anim_mask, *snap;
static long anim_cap;
static int cache_win = -1, cache_focus;
static unsigned cache_frame;

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

/* ---- Easing: t in 0..1024 (0..1) -> 0..1024 ------------------------------- */

static long ease_out(long t) { t = t < 0 ? 0 : t > 1024 ? 1024 : t; return 1024 - (1024 - t) * (1024 - t) / 1024; }
static long ease_in(long t) { t = t < 0 ? 0 : t > 1024 ? 1024 : t; return t * t / 1024; }
static long ease_inout(long t) {
    t = t < 0 ? 0 : t > 1024 ? 1024 : t;
    return t * t * (3 * 1024 - 2 * t) / (1024 * 1024);          /* smoothstep */
}

/* A window that takes clicks, keys and the title in the menu bar. */
static int live(const window_t *w) {
    return w->used && !w->minimised && w->anim != ANIM_CLOSE && w->anim != ANIM_MIN;
}

static int anim_ok(const window_t *w) {
    return anim_cap > 0 && (long)(w->w + 2) * (w->h + TITLE_H + 2) <= anim_cap;
}

/* ---- The Dock ------------------------------------------------------------ */

static int icon(void) { return 48 * S; }
static int pad(void) { return 8 * S; }
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

static int mi_h(int i) { return (mitems[i].flags & MI_SEP) ? 8 * S : 24 * S; }
static int menu_x(void) { return bar_x0[menu_open ? menu_open : 1]; }
static int menu_y(void) { return MENU_H; }
static int menu_w(void) { return menu_wpx; }
static int menu_h(void) {
    int h = 8 * S;
    for (int i = 0; i < n_mitems; i++) h += mi_h(i);
    return h;
}
static int menu_item_y(int i) {
    int y = menu_y() + 4 * S;
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
        if (live(&wins[order[i]])) return &wins[order[i]];
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
        snprintf(text, sizeof(text), "Increase Contrast: %s", contrast ? "On" : "Off");
        add_item(text, 0, M_CONTRAST, 0);
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
            if (!wins[k].used || wins[k].anim == ANIM_CLOSE) continue;
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
    if (menu_wpx < 192 * S) menu_wpx = 192 * S;
}

static void damage_menu(void) {
    damage(menu_x() - SHADOW, menu_y() - SHADOW, menu_w() + 2 * SHADOW, menu_h() + 2 * SHADOW);
    damage_menu_bar();
}

static void open_menu(int kind) {
    build_menu(kind);
    menu_open = kind;
    menu_hot = -1;
    menu_fade_ms = 0;                                  /* fades in over 120 ms */
    damage_menu();
}

static void close_menu(void) {
    if (!menu_open) return;
    damage_menu();
    menu_open = 0;
    menu_hot = -1;
}

/* Moving from one menu to the next: no fade (as on macOS). */
static void switch_menu(int kind) {
    close_menu();
    open_menu(kind);
    menu_fade_ms = FADE_MS;
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

/* Where a minimising window goes: its Dock icon (or the middle of the Dock). */
static void dock_target(const window_t *w, int *tx, int *ty) {
    for (int i = 0; i < DOCK_APPS; i++)
        if (apps[i].pid > 0 && apps[i].pid == w->pid) {
            *tx = icon_x(i) + apps[i].size / 2;
            *ty = dock_bottom() - pad() - apps[i].size / 2;
            return;
        }
    *tx = W / 2;
    *ty = dock_bottom() - dock_h() / 2;
}

/* The screen rectangle a window's animation can reach (with its shadow). */
static void damage_anim(window_t *w) {
    int x, y, ww, hh;
    outer(w, &x, &y, &ww, &hh);
    int x0 = x - SHADOW, y0 = y - SHADOW, x1 = x + ww + SHADOW, y1 = y + hh + SHADOW + SHADOW / 3;
    if (w->anim == ANIM_MIN || w->anim == ANIM_RESTORE) {        /* and the way to the Dock */
        int tx, ty;
        dock_target(w, &tx, &ty);
        if (tx - 2 * icon() < x0) x0 = tx - 2 * icon();
        if (tx + 2 * icon() > x1) x1 = tx + 2 * icon();
        if (ty - 2 * icon() < y0) y0 = ty - 2 * icon();
        if (ty + 2 * icon() > y1) y1 = ty + 2 * icon();
    }
    damage(x0, y0, x1 - x0, y1 - y0);
}

/* The window's surface goes away. */
static void finish_remove(int k) {
    window_t *w = &wins[k];
    damage_window(w);
    munmap(w->s);
    w->used = 0;
    w->anim = ANIM_NONE;
    int j = 0;
    for (int i = 0; i < norder; i++)
        if (order[i] != k) order[j++] = order[i];
    norder = j;
    if (cache_win == k) cache_win = -1;
    damage_menu_bar();
}

/* A window ends: it fades and shrinks a little (ease-in, 150 ms), then goes. */
static void remove_window(int k) {
    window_t *w = &wins[k];
    if (w->anim == ANIM_CLOSE) return;
    if (menu_open == 2) close_menu();
    if (drag == k) drag = -1;
    if (capture == k) capture = -1;
    if (tl_hover == k) tl_hover = -1;
    if (last_title_win == k) last_title_win = -1;
    if (w->minimised || w->anim == ANIM_WAIT || !anim_ok(w)) {      /* nothing to see: at once */
        finish_remove(k);
    } else {
        w->anim = ANIM_CLOSE;
        w->anim_ms = 0;
        w->s->focused = 0;
        damage_anim(w);
    }
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
    if (w->anim == ANIM_CLOSE || w->anim == ANIM_MIN) return;
    if (tl_hover == k) tl_hover = -1;
    w->s->focused = 0;
    if (anim_ok(w)) {
        w->anim = ANIM_MIN;                           /* shrinks towards the Dock, then hides */
        w->anim_ms = 0;
        damage_anim(w);
    } else {
        w->minimised = 1;
        damage_window(w);
    }
    window_t *f = front();
    if (f) raise((int)(f - wins));
    damage_menu_bar();
}

/* A minimised window comes back out of the Dock. */
static void restore_window(int k) {
    window_t *w = &wins[k];
    if (!w->minimised) return;
    w->minimised = 0;
    if (anim_ok(w)) {
        w->anim = ANIM_RESTORE;
        w->anim_ms = 0;
        damage_anim(w);
    } else {
        damage_window(w);
    }
}

/* The first drawing is in: the window opens with a spring that overshoots by
 * about 7% of its travel (damping 0.65) while its opacity eases out. */
static void start_open(window_t *w) {
    if (!anim_ok(w)) {
        w->anim = ANIM_NONE;
        damage_window(w);
        return;
    }
    w->anim = ANIM_OPEN;
    w->anim_ms = 0;
    w->sc.pos = 92L * FP;
    w->sc.vel = 0;
    damage_anim(w);
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
    w->s->prefs = prefs_bits();
    w->used = 1;
    w->anim = anim_cap > 0 ? ANIM_WAIT : ANIM_NONE;
    w->pid = q->pid;
    w->w = q->w;
    w->h = q->h;
    snprintf(w->title, sizeof(w->title), "%s", q->title);
    int step = 24 * S;                             /* cascade from the top left */
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

/* Traffic lights: 12 pt wide, 8 pt apart (centres 22, 42, 62 pt from the left,
 * 16 pt in). Glyphs show while the pointer is over the group (macOS), and the
 * lights are grey when the window is not the front one (unless hovered). The
 * content of the window starts at (cx, cy) on `cv`. */
#define LIGHT_X(i) ((22 + 20 * (i)) * S)
static void traffic_lights(ui_canvas *cv, int cx0, int cy0, int focused, int hover) {
    static const unsigned fill[3] = { 0xFF5F57, 0xFEBC2E, 0x28C840 };
    static const unsigned ring[3] = { 0xE0443E, 0xDEA123, 0x1AAB29 };
    static const unsigned ink[3] = { 0x4D0000, 0x995700, 0x006500 };
    int cy = cy0 - TITLE_H / 2, r = 6 * S, lit = focused || hover;
    for (int i = 0; i < 3; i++) {
        int cx = cx0 + LIGHT_X(i);
        ui_circle(cv, cx, cy, r, lit ? ring[i] : TH.edge_off);
        ui_circle(cv, cx, cy, r - 1, lit ? fill[i] : TH.light_off);
        if (!hover) continue;
        if (i == 0) {
            for (int t = -2 * S; t <= 2 * S; t++) {
                ui_fill(cv, cx + t, cy + t, S, S, ink[0]);
                ui_fill(cv, cx + t, cy - t, S, S, ink[0]);
            }
        } else if (i == 1) {
            ui_fill(cv, cx - 3 * S, cy, 6 * S, S, ink[1]);
        } else {
            ui_fill(cv, cx - 3 * S, cy, 6 * S, S, ink[2]);
            ui_fill(cv, cx, cy - 3 * S, S, 6 * S, ink[2]);
        }
    }
}

/* The shadow round an outer rectangle; a255 fades it with the window. */
static void window_shadow(int x, int y, int ww, int hh, int focused, int a255) {
    ui_shadow(&scr, x + 2 * S, y + SHADOW / 3, ww - 4 * S, hh - 2 * S, RADIUS, SHADOW,
              (focused ? TH.shadow_on : TH.shadow_off) * a255 / 255);
    ui_shadow(&scr, x, y, ww, hh, RADIUS, 2 * S, 50 * a255 / 255);        /* a tight edge */
}

/* The window itself (outline, title bar, content), no shadow, onto `cv` with
 * its content at (cx, cy). */
static void window_body(ui_canvas *cv, window_t *w, int cx, int cy, int focused, int hover) {
    ui_round(cv, cx - 1, cy - TITLE_H - 1, w->w + 2, w->h + TITLE_H + 2, RADIUS,
             focused ? TH.edge_on : TH.edge_off);                                  /* the 1 px rim */
    ui_round_ex(cv, cx, cy - TITLE_H, w->w, TITLE_H, RADIUS - 1, focused ? TH.title_on : TH.title_off,
                255, UI_TL | UI_TR);
    ui_fill_a(cv, cx, cy - 1, w->w, 1, TH.sep, TH.sep_alpha);
    traffic_lights(cv, cx, cy, focused, hover);
    int tw = ui_text_style_width(w->title, S, UI_BOLD), tx = cx + (w->w - tw) / 2;
    if (tx < cx + 80 * S) tx = cx + 80 * S;
    ui_text_bold(cv, tx, cy - TITLE_H + (TITLE_H - 16 * S) / 2, w->title,
                 focused ? TH.title_text_on : TH.title_text_off, S);
    ui_copy_round(cv, cx, cy, (const unsigned *)((char *)w->s + WS_HEADER), w->w, w->h, w->w,
                  RADIUS - 1, UI_BL | UI_BR);
}

/* ---- Window motion ----------------------------------------------------------
 * The animated window is drawn on its own (with its rim colour round it, so the
 * anti-aliased corners are right), its shape goes into a second buffer, and
 * both are scaled onto the screen with bilinear sampling and an opacity. Each
 * property has its own curve: the scale (spring or ease), the opacity (ease). */

/* Scale in 1/1000, opacity 0-255, and the centre, for the animation as it is now. */
static void anim_params(const window_t *w, int *scale, int *alpha, int *cx, int *cy) {
    int ox, oy, ow, oh;
    outer((window_t *)w, &ox, &oy, &ow, &oh);
    *cx = ox + ow / 2;
    *cy = oy + oh / 2;
    *scale = 1000;
    *alpha = 255;
    if (w->anim == ANIM_OPEN) {
        if (!reduce_motion) *scale = (int)(w->sc.pos * 10 / FP);
        *alpha = (int)(255 * ease_out((long)w->anim_ms * 1024 / OPEN_MS) / 1024);
    } else if (w->anim == ANIM_CLOSE) {
        long e = ease_in((long)w->anim_ms * 1024 / CLOSE_MS);
        if (!reduce_motion) *scale = (int)(1000 - 40 * e / 1024);
        *alpha = (int)(255 - 255 * e / 1024);
    } else if (w->anim == ANIM_MIN || w->anim == ANIM_RESTORE) {
        int dur = reduce_motion ? CLOSE_MS : MIN_MS;
        long p = (long)w->anim_ms * 1024 / dur;
        long q = w->anim == ANIM_RESTORE ? 1024 - ease_out(p) : ease_inout(p);     /* 0: window, 1024: Dock */
        if (!reduce_motion) {
            int tx, ty;
            dock_target(w, &tx, &ty);
            *scale = (int)(1000 - 880 * q / 1024);                                   /* down to 12% */
            *cx += (int)((long)(tx - *cx) * q / 1024);
            *cy += (int)((long)(ty - *cy) * q / 1024);
            *alpha = q < 600 ? 255 : (int)(255 * (1024 - q) / (1024 - 600));       /* fades at the end */
        } else {
            *alpha = (int)(255 * (1024 - q) / 1024);
        }
    }
    if (*scale < 1) *scale = 1;
    if (*alpha < 0) *alpha = 0;
    if (*alpha > 255) *alpha = 255;
}

/* The ow x oh picture (and its shape) scaled into dw x dh at (dx, dy), with
 * opacity alpha: bilinear, clipped to the damage rectangle in force. `under`
 * (or 0) is what was on the screen before the shadow was drawn, from (ux, uy),
 * uw wide: a see-through window shows that, not its own shadow, through its body. */
static void blit_scaled(int dx, int dy, int dw, int dh, int ow, int oh, int alpha,
                        const unsigned *under, int ux, int uy, int uw) {
    int x0 = dx < scr.cx0 ? scr.cx0 : dx, y0 = dy < scr.cy0 ? scr.cy0 : dy;
    int x1 = dx + dw > scr.cx1 ? scr.cx1 : dx + dw, y1 = dy + dh > scr.cy1 ? scr.cy1 : dy + dh;
    if (dw <= 0 || dh <= 0) return;
    for (int py = y0; py < y1; py++) {
        int v = ((2 * (py - dy) + 1) * oh * 128) / dh - 128;                         /* 1/256 texels */
        int ty = v >> 8;
        unsigned fy = (unsigned)(v & 255);
        if (ty < 0) { ty = 0; fy = 0; }
        if (ty >= oh - 1) { ty = oh - 1; fy = 0; }
        const unsigned *c0 = anim_col + (long)ty * ow, *c1 = fy ? c0 + ow : c0;
        const unsigned *m0 = anim_mask + (long)ty * ow, *m1 = fy ? m0 + ow : m0;
        unsigned *row = scr.px + (long)py * scr.stride;
        for (int px = x0; px < x1; px++) {
            int u = ((2 * (px - dx) + 1) * ow * 128) / dw - 128, tx = u >> 8;
            unsigned fx = (unsigned)(u & 255);
            if (tx < 0) { tx = 0; fx = 0; }
            if (tx >= ow - 1) { tx = ow - 1; fx = 0; }
            unsigned col, msk;
            if (!fx && !fy) {
                col = c0[tx];
                msk = m0[tx] & 255;
            } else {
                unsigned a = fx ? lerp_rgb(c0[tx], c0[tx + 1], fx) : c0[tx];
                unsigned b = fx ? lerp_rgb(c1[tx], c1[tx + 1], fx) : c1[tx];
                col = fy ? lerp_rgb(a, b, fy) : a;
                a = fx ? lerp_rgb(m0[tx], m0[tx + 1], fx) : m0[tx];
                b = fx ? lerp_rgb(m1[tx], m1[tx + 1], fx) : m1[tx];
                msk = (fy ? lerp_rgb(a, b, fy) : a) & 255;
            }
            unsigned a = msk * (unsigned)alpha / 255;
            if (a >= 255) {
                row[px] = col;
            } else if (a) {
                unsigned base = (under && msk >= 255) ? under[(long)(py - uy) * uw + (px - ux)] : row[px];
                row[px] = lerp_rgb(base, col, a + (a >> 7));
            }
        }
    }
}

static void draw_window_anim(int k, int focused, int hover) {
    window_t *w = &wins[k];
    int ox, oy, ow, oh;
    outer(w, &ox, &oy, &ow, &oh);
    if (cache_win != k || cache_frame != frame_no || cache_focus != focused) {   /* once per frame */
        ui_canvas ac, mc;
        ui_canvas_init(&ac, anim_col, ow, oh, ow);
        ui_canvas_init(&mc, anim_mask, ow, oh, ow);
        ui_fill(&ac, 0, 0, ow, oh, focused ? TH.edge_on : TH.edge_off);
        window_body(&ac, w, 1, TITLE_H + 1, focused, hover);
        ui_fill(&mc, 0, 0, ow, oh, 0x000000);
        ui_round(&mc, 0, 0, ow, oh, RADIUS, 0xFFFFFF);
        cache_win = k;
        cache_frame = frame_no;
        cache_focus = focused;
    }
    int scale, alpha, cx, cy;
    anim_params(w, &scale, &alpha, &cx, &cy);
    int dw = ow * scale / 1000, dh = oh * scale / 1000;
    if (dw < 1 || dh < 1) return;
    int dx = cx - dw / 2, dy = cy - dh / 2;
    int ux = dx < scr.cx0 ? scr.cx0 : dx, uy = dy < scr.cy0 ? scr.cy0 : dy;
    int ux1 = dx + dw > scr.cx1 ? scr.cx1 : dx + dw, uy1 = dy + dh > scr.cy1 ? scr.cy1 : dy + dh;
    int uw = ux1 - ux;
    const unsigned *under = 0;
    if (alpha < 255 && snap && uw > 0 && uy1 > uy && (long)uw * (uy1 - uy) <= anim_cap) {
        for (int y = uy; y < uy1; y++)
            memcpy(snap + (long)(y - uy) * uw, scr.px + (long)y * scr.stride + ux, (size_t)uw * 4);
        under = snap;
    }
    window_shadow(dx, dy, dw, dh, focused, alpha);
    blit_scaled(dx, dy, dw, dh, ow, oh, alpha, under, ux, uy, uw);
}

static void draw_window(int k, int front_win) {
    window_t *w = &wins[k];
    if (w->minimised || w->anim == ANIM_WAIT) return;
    int exiting = w->anim == ANIM_CLOSE || w->anim == ANIM_MIN;
    int focused = front_win || exiting;                                          /* keeps its look while leaving */
    int hover = tl_hover >= 0 && &wins[tl_hover] == w;
    if (w->anim != ANIM_NONE && anim_ok(w)) {
        draw_window_anim(k, focused, hover);
        return;
    }
    int x, y, ww, hh;
    outer(w, &x, &y, &ww, &hh);
    window_shadow(x, y, ww, hh, focused, 255);
    window_body(&scr, w, w->x, w->y, focused, hover);
}

static void draw_menu_bar(void) {
    ui_fill_a(&scr, 0, 0, W, MENU_H, TH.bar_tint, TH.bar_alpha);           /* translucent */
    ui_fill_a(&scr, 0, MENU_H - 1, W, 1, TH.sep, TH.sep_alpha);
    int ty = (MENU_H - 16 * S) / 2, x = 16 * S;
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
    int is = 16 * S, iy = (MENU_H - is) / 2;
    status_x[0] = status_x[1] = status_x[2] = 0;
    rx -= 16 * S;
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
            int ta = (int)(255 * ease_out((long)tip_ms * 1024 / FADE_MS) / 1024);      /* fades in over 120 ms */
            int lw = ui_text_width(a->name, S) + 16 * S, lh = 24 * S;
            int lx = ix + size / 2 - lw / 2, ly = dock_bottom() - dock_h() - lh - 8 * S - (size - icon());
            ui_round_ex(&scr, lx, ly, lw, lh, R_MEDIUM * S, TH.tooltip, 225 * ta / 255, UI_ALL);
            ui_text(&scr, lx + 8 * S, ly + (lh - 16 * S) / 2, a->name,
                    lerp_rgb(TH.tooltip, 0xFFFFFF, (unsigned)(ta + (ta >> 7))), S);
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
            ui_fill_a(&scr, x + 8 * S, iy + ih / 2, w - 16 * S, 1, TH.sep, TH.sep_alpha);
            continue;
        }
        int off = mitems[i].flags & MI_OFF, hot = i == menu_hot && !off;
        if (hot) ui_round(&scr, x + 4 * S, iy, w - 8 * S, ih, R_SMALL * S, TH.accent);   /* 8 - 4 padding */
        unsigned fg = hot ? 0xFFFFFF : off ? TH.secondary : TH.label;
        int ty = iy + (ih - 16 * S) / 2;
        ui_text(&scr, x + 16 * S, ty, mitems[i].label, fg, S);
        if (mitems[i].key[0]) {
            int kw = ui_text_width(mitems[i].key, S);
            ui_text(&scr, x + w - 16 * S - kw, ty, mitems[i].key, hot ? 0xFFFFFF : TH.secondary, S);
        }
    }
}

/* The menu, fading in over 120 ms (ease-out): what is behind it is saved, the
 * menu drawn, then the two mixed. A micro-interaction, so Reduce Motion keeps it. */
static void draw_menu_faded(void) {
    if (!menu_open) return;
    int a = (int)(255 * ease_out((long)menu_fade_ms * 1024 / FADE_MS) / 1024);
    int x0 = menu_x() - SHADOW, y0 = menu_y() - SHADOW, x1 = menu_x() + menu_w() + SHADOW, y1 = menu_y() + menu_h() + SHADOW;
    if (x0 < scr.cx0) x0 = scr.cx0;
    if (y0 < scr.cy0) y0 = scr.cy0;
    if (x1 > scr.cx1) x1 = scr.cx1;
    if (y1 > scr.cy1) y1 = scr.cy1;
    if (a >= 255 || !snap || x1 <= x0 || y1 <= y0 || (long)(x1 - x0) * (y1 - y0) > anim_cap) {
        draw_menu();
        return;
    }
    for (int y = y0; y < y1; y++)
        memcpy(snap + (long)(y - y0) * (x1 - x0), scr.px + (long)y * scr.stride + x0, (size_t)(x1 - x0) * 4);
    draw_menu();
    for (int y = y0; y < y1; y++) {
        unsigned *row = scr.px + (long)y * scr.stride + x0;
        const unsigned *old = snap + (long)(y - y0) * (x1 - x0);
        for (int x = 0; x < x1 - x0; x++) row[x] = lerp_rgb(old[x], row[x], (unsigned)(a + (a >> 7)));
    }
}

/* -> 1 if the frame was paced by the screen (page flipping): no sleep then. */
static int compose(void) {
    window_t *f = front();
    frame_no++;
    for (int d = 0; d < ndmg; d++) {
        int x0 = dmg[d].x0, y0 = dmg[d].y0, x1 = dmg[d].x1, y1 = dmg[d].y1;
        ui_clip(&scr, x0, y0, x1 - x0, y1 - y0);
        for (int y = y0; y < y1; y++) ui_fill(&scr, x0, y, x1 - x0, 1, wallpaper[y]);
        for (int i = 0; i < norder; i++) draw_window(order[i], &wins[order[i]] == f);
        draw_menu_bar();
        draw_dock();
        draw_menu_faded();
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
        if (live(w) && ui_inside(x, y, ox, oy, ow, oh)) return order[i];
    }
    return -1;
}

/* The window whose traffic-light group the pointer is over, or -1. */
static int traffic_hover(void) {
    int k = window_at(mouse_x, mouse_y);
    if (k < 0) return -1;
    window_t *w = &wins[k];
    return ui_inside(mouse_x, mouse_y, w->x + 12 * S, w->y - TITLE_H, 60 * S, TITLE_H) ? k : -1;
}

static void launch(int i) {
    for (int k = 0; k < MAX_WINDOWS; k++)            /* running: bring its window back */
        if (wins[k].used && wins[k].anim != ANIM_CLOSE && apps[i].pid > 0 && wins[k].pid == apps[i].pid) {
            restore_window(k);
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

/* Tell every window (ws.h `prefs`) when a setting changes. */
static void push_prefs(void) {
    for (int k = 0; k < MAX_WINDOWS; k++)
        if (wins[k].used) wins[k].s->prefs = prefs_bits();
}

static void menu_choose(int id) {
    if (id == M_ABOUT) run("about", 0);
    else if (id == M_SETTINGS) run("settings", 0);
    else if (id == M_ACTIVITY) run("activity", 0);
    else if (id == M_TERMINAL) run("terminal", 0);
    else if (id == M_APPEARANCE) {
        dark = !dark;
        apply_theme();
        make_wallpaper();
        push_prefs();
        damage(0, 0, W, H);
    } else if (id == M_MOTION) {
        reduce_motion = !reduce_motion;
        push_prefs();
    } else if (id == M_TRANSP) {
        reduce_transp = !reduce_transp;
        push_prefs();
        damage(0, 0, W, H);
    } else if (id == M_CONTRAST) {
        contrast = !contrast;
        apply_theme();
        push_prefs();
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
            restore_window(k);
            raise(k);
        }
    }
}

/* Keys while a menu is open: Esc, Left/Right (the other menu), Up/Down, Enter. */
static void menu_key(int key) {
    if (key == 27) {
        close_menu();
    } else if (key == KEYCODE_LEFT || key == KEYCODE_RIGHT) {
        switch_menu(menu_open == 1 ? 2 : 1);
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
        else if (t && t != was) {
            open_menu(t);
            menu_fade_ms = FADE_MS;                    /* from one menu to the next: no fade */
        }
        return;
    }
    if (mouse_y < MENU_H) {                            /* the status icons */
        static const char *panes[] = { "battery", "wifi", "sound" };
        for (int k = 0; k < 3; k++)
            if (status_x[k] && mouse_x >= status_x[k] && mouse_x < status_x[k] + (k ? 24 : 72) * S) {
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
        if (ui_inside(mouse_x, mouse_y, w->x + LIGHT_X(0) - r, cy - r, 2 * r, 2 * r)) {
            close_window_request(k);
            return;
        }
        if (ui_inside(mouse_x, mouse_y, w->x + LIGHT_X(1) - r, cy - r, 2 * r, 2 * r)) {
            minimise_window(k);
            return;
        }
        if (ui_inside(mouse_x, mouse_y, w->x + LIGHT_X(2) - r, cy - r, 2 * r, 2 * r))
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
                if (t && t != menu_open) switch_menu(t);
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

/* Moves every animation on by dt ms: windows (opening, closing, minimizing,
 * restoring), the menu's fade and the Dock label's fade. */
static void animate_windows(int dt) {
    for (int k = 0; k < MAX_WINDOWS; k++) {
        window_t *w = &wins[k];
        if (!w->used || w->anim == ANIM_NONE) continue;
        w->anim_ms += dt;
        if (w->anim == ANIM_WAIT) {                      /* the program never drew: show it anyway */
            if (w->anim_ms > 400) start_open(w);
            continue;
        }
        int done = 0;
        if (w->anim == ANIM_OPEN) {
            int moving = spring_step(&w->sc, 100L * FP, dt, 130, 15);   /* response 0.55, damping 0.65 */
            done = w->anim_ms >= OPEN_MS && (reduce_motion || !moving);
        } else if (w->anim == ANIM_CLOSE) {
            done = w->anim_ms >= CLOSE_MS;
        } else {
            done = w->anim_ms >= (reduce_motion ? CLOSE_MS : MIN_MS);
        }
        damage_anim(w);
        if (!done) continue;
        int anim = w->anim;
        w->anim = ANIM_NONE;
        if (anim == ANIM_CLOSE) finish_remove(k);
        else if (anim == ANIM_MIN) w->minimised = 1;
    }
    if (menu_open && menu_fade_ms < FADE_MS) {
        menu_fade_ms += dt;
        damage_menu();
    }
    int hot = menu_open ? -1 : dock_hit(mouse_x, mouse_y);
    if (hot != tip_hot) {
        tip_hot = hot;
        tip_ms = 0;
    } else if (hot >= 0 && tip_ms < FADE_MS) {
        tip_ms += dt;
        damage_dock();
    }
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
    TITLE_H = 32 * S;                                  /* 8-point grid */
    RADIUS = R_WINDOW * S;
    SHADOW = 24 * S;                                   /* design blur 48 / 2 */
    unsigned *px = mmap((size_t)W * H * 4, PROT_READ | PROT_WRITE, -1);
    wallpaper = mmap((size_t)H * 4, PROT_READ | PROT_WRITE, -1);
    if (!px || !wallpaper) {
        printf("windowserver: out of memory\n");
        return 1;
    }
    ui_canvas_init(&scr, px, W, H, W);
    apply_theme();
    long cap = (long)W * H < (1L << 20) ? (long)W * H : (1L << 20);       /* 4 MB each */
    anim_col = mmap((size_t)cap * 4, PROT_READ | PROT_WRITE, -1);
    anim_mask = mmap((size_t)cap * 4, PROT_READ | PROT_WRITE, -1);
    snap = mmap((size_t)cap * 4, PROT_READ | PROT_WRITE, -1);
    anim_cap = anim_col && anim_mask && snap ? cap : 0;                  /* else: no window motion, no menu fade */
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
                if (w->anim == ANIM_WAIT) start_open(w);       /* the first drawing: open it */
                else if (!w->minimised && w->anim == ANIM_NONE) damage(w->x, w->y, w->w, w->h);
            }
        }
        animate_dock(dt);
        animate_windows(dt);
        if (uptime_ms() - last_tidy >= 500) {
            last_tidy = uptime_ms();
            tidy();
        }
        int paced = compose();
        mutex_unlock(&lock);
        if (!paced) sleep_ms(5);                      /* else the screen's refresh set the pace */
    }
}