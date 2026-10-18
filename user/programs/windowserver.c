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
#include <stdlib.h>
#include <string.h>
#include <os.h>
#include <ui.h>
#include <image.h>

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

static int dark, reduce_motion, reduce_transp, contrast, accent;
static theme_t cur;                                /* the theme in force */

/* The desktop's settings (ws.h), shared with System Settings. */
static struct ws_settings *cfg, cfg_local;
static unsigned cfg_seen;
static unsigned long save_at;                      /* save DESKTOP.CFG then (0: nothing to save) */
static void save_settings(void);
#define TH cur

/* The theme for the current settings. Increase Contrast: rims at full opacity
 * and 1 px, separators and the highlight stronger, secondary text and the
 * accent darker (light) or lighter (dark), a solid menu bar. */
static unsigned theme_gen;                         /* bumped on each change: window pictures redraw */

static void apply_theme(void) {
    theme_gen++;
    cur = themes[dark];
    cur.accent = ui_accent_color(accent, dark, contrast);
    if (!contrast) return;
    cur.hair_alpha = 255;
    cur.dock_rim_alpha = 255;
    cur.sep_alpha = 150;
    cur.hl_alpha += 30;
    cur.bar_alpha = 255;
    cur.secondary = dark ? 0xD1D1D6 : 0x48484A;
    cur.edge_on = dark ? 0xAEAEB2 : 0x3A3A3C;
    cur.edge_off = dark ? 0x8E8E93 : 0x636366;
    cur.title_text_off = dark ? 0xAEAEB2 : 0x636366;
    cur.hair = dark ? 0xFFFFFF : 0x000000;
    cur.dock_rim = dark ? 0xFFFFFF : 0x000000;
}

/* What apps see in their surface header (ws.h). */
static unsigned prefs_bits(void) {
    return (dark ? WS_PREF_DARK : 0) | (contrast ? WS_PREF_CONTRAST : 0) |
           (reduce_motion ? WS_PREF_MOTION : 0) | (reduce_transp ? WS_PREF_TRANSP : 0) | (unsigned)accent << 8;
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
    int resizable, zoomed, gen;        /* zoom: WS_RESIZABLE, filling the screen, surface number */
    int saved_x, saved_y, saved_w, saved_h, want_x, want_y;   /* before zooming; where it goes */
    int gpu_body, gpu_y0, gpu_y1;      /* this frame the GPU copies its content rows [gpu_y0, gpu_y1) */
    unsigned drawn;                    /* the damage count the last frame was made from */
    unsigned *lay;                     /* its picture: shadow, rim, title bar (layer_update) */
    long lay_cap;                      /* pixels allocated */
    int lay_w, lay_h, lay_focus, lay_hover, lay_ok;
    unsigned lay_theme;
    char lay_title[48];
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

/* The Dock's apps come from DOCK.CFG, which the build makes from every
 * apps/NAME/app.cfg (scripts/mkdock.sh): "order|name|program|icon|color". */
#define MAX_DOCK 24
static dock_app_t apps[MAX_DOCK];
static int n_apps;
#define DOCK_APPS n_apps

static void dock_add(char *name, char *program, char *icon, unsigned color) {
    static char glyphs[MAX_DOCK][2];
    if (n_apps == MAX_DOCK) return;
    dock_app_t *a = &apps[n_apps];
    glyphs[n_apps][0] = name[0];                       /* the drawn icon, without a picture */
    a->name = name;
    a->program = program;
    a->icon = icon;
    a->glyph = glyphs[n_apps];
    a->color = color;
    n_apps++;
}

static void load_installed(void);
static void damage_dock(void);

static void load_dock(void) {
    static char text[4096];
    int fd = open("/SYS/DOCK.CFG", OPEN_READ);
    if (fd < 0) fd = open("/DOCK.CFG", OPEN_READ);
    long n = fd >= 0 ? read(fd, text, sizeof(text) - 1) : -1;
    if (fd >= 0) close(fd);
    if (n > 0) {
        text[n] = 0;
        for (char *line = text, *next; line && *line; line = next) {
            next = strchr(line, '\n');
            if (next) *next++ = 0;
            char *f[5];
            int k = 0;
            for (char *p = line; k < 5 && p; k++) {
                f[k] = p;
                p = strchr(p, '|');
                if (p) *p++ = 0;
            }
            if (k < 5 || !f[1][0] || !f[2][0]) continue;
            char *cr = strchr(f[4], '\r');
            if (cr) *cr = 0;
            unsigned color = 0;
            for (char *p = f[4]; *p; p++)
                color = color * 16 + (unsigned)(*p <= '9' ? *p - '0' : (*p | 32) - 'a' + 10);
            dock_add(f[1], f[2], f[3], color);
        }
    }
    if (!n_apps) {                                     /* no DOCK.CFG: the two that always exist */
        static char files[] = "Files", finder[] = "finder", folder[] = "folder";
        static char term[] = "Terminal", terminal[] = "terminal";
        dock_add(files, finder, folder, 0x2F7BF5);
        dock_add(term, terminal, terminal, 0x30323A);
    }
    load_installed();
}

/* The apps this user installed (tapp: docs/PACKAGES.md), after the built-in
 * ones: ~/Apps/ID/app.cfg ("name = ...", "color = RRGGBB", "icon = NAME"),
 * the program ~/Apps/ID/ID, its picture ~/Apps/ID/icon.png. */
static void load_installed(void) {
    static char text[MAX_DOCK][3][128];               /* name, program, icon */
    static struct os_dirent ents[MAX_DOCK];
    struct os_user u;
    char dir[64];
    if (getuser(&u) != 0) return;
    size_t hl = strlen(u.home);
    snprintf(dir, sizeof(dir), "%s%sApps", u.home, hl && u.home[hl - 1] == '/' ? "" : "/");
    int n = listdir(dir, ents, MAX_DOCK);
    for (int i = 0, k = 0; i < n && n_apps < MAX_DOCK; i++) {
        const char *id = ents[i].name;
        if (!ents[i].is_dir || id[0] == '.') continue;
        char path[200], cfg[1024];
        snprintf(path, sizeof(path), "%s/%s/app.cfg", dir, id);
        int fd = open(path, OPEN_READ);
        long len = fd >= 0 ? read(fd, cfg, sizeof(cfg) - 1) : -1;
        if (fd >= 0) close(fd);
        if (len <= 0) continue;
        cfg[len] = 0;
        char *name = text[k][0], *program = text[k][1], *icon = text[k][2];
        snprintf(name, 128, "%s", id);
        icon[0] = 0;
        unsigned color = 0x8E8E93;
        for (char *line = cfg, *next; line && *line; line = next) {   /* key = value  # comment */
            next = strchr(line, '\n');
            if (next) *next++ = 0;
            char *eq = strchr(line, '=');
            if (!eq || line[0] == '#') continue;
            char *key = line, *value = eq + 1, *end = eq;
            while (end > key && (end[-1] == ' ' || end[-1] == '\t')) end--;
            *end = 0;
            while (*value == ' ' || *value == '\t') value++;
            char *hash = strstr(value, " #");
            if (hash) *hash = 0;
            end = value + strlen(value);
            while (end > value && (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\r')) end--;
            *end = 0;
            if (!strcmp(key, "name") && *value) snprintf(name, 128, "%s", value);
            else if (!strcmp(key, "icon")) snprintf(icon, 128, "%s", value);
            else if (!strcmp(key, "color")) {
                color = 0;
                for (char *p = value; *p; p++)
                    color = color * 16 + (unsigned)(*p <= '9' ? *p - '0' : (*p | 32) - 'a' + 10);
            }
        }
        snprintf(program, 128, "%s/%s/%s", dir, id, id);
        struct os_stat st;
        snprintf(path, sizeof(path), "%s/%s/icon.png", dir, id);
        if (stat(path, &st) == 0 && !st.is_dir) snprintf(icon, 128, "%s", path);
        if (icon[0]) ui_has_icon(icon);                /* read it now, not while painters draw */
        dock_add(name, program, icon, color);
        k++;
    }
}

/* tapp installed or removed an app (WS_APPS): the Dock again, the apps still
 * running keep their state. Under the lock. */
static void reload_dock(void) {
    static dock_app_t old[MAX_DOCK];
    int n_old = n_apps;
    memcpy(old, apps, sizeof(apps));
    memset(apps, 0, sizeof(apps));
    n_apps = 0;
    load_dock();
    for (int i = 0; i < n_apps; i++)
        for (int j = 0; j < n_old; j++)
            if (!strcmp(apps[i].program, old[j].program)) {
                const char *name = apps[i].name, *program = apps[i].program, *glyph = apps[i].glyph, *icon = apps[i].icon;
                unsigned color = apps[i].color;
                apps[i] = old[j];
                apps[i].name = name;
                apps[i].program = program;
                apps[i].glyph = glyph;
                apps[i].icon = icon;
                apps[i].color = color;
                break;
            }
    damage_dock();
}

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

/* The desktop is drawn by several cores at once (painters), each a band of
 * the screen: its own copy of the back buffer's canvas (clipped to its band)
 * and its own scratch space for materials. */
typedef struct {
    ui_canvas c;
    unsigned *samp, *samp2, *mat, *xf;
    int *xi;
} painter_t;
#define MAX_PAINTERS 8
#define PAINT_MIN (256 * 256)                          /* smaller frames: one core (waking others costs more) */
static painter_t painters[MAX_PAINTERS];
static int n_painters = 1;
static volatile int paint_job, paint_left;            /* the frame's number; painters still at it */
static unsigned *wallpaper;                       /* W x H: a gradient, a colour or a picture */
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

static int icon(void) { return cfg->dock_size * S; }
static int pad(void) { return 8 * S; }
static int dock_h(void) { return icon() + 2 * pad(); }
/* A maximised window in front fills the screen and the Dock slides below the
 * bottom edge (0 shown .. 1024 hidden); the pointer at the bottom edge brings it back. */
static int dock_hide;
static int dock_bottom(void) { return H - 8 * S + dock_hide * (dock_h() + icon() + 32 * S) / 1024; }

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
    int widest = DOCK_APPS * (icon() * (100 + cfg->dock_magnify) / 100 + pad()) + pad();
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
static window_t *front(void);

/* Hide the Dock while a maximised window is in front, unless the pointer is
 * at the bottom edge or on the Dock. -> 1 while it moves. */
static int slide_dock(int dt) {
    window_t *f = front();
    int want = 0;
    if (((f && f->zoomed && !f->minimised) || cfg->dock_autohide) && !menu_open) {   /* (or always: Automatically hide) */
        int on_dock = dock_hide < 1024 && mouse_y >= dock_bottom() - dock_h() - icon() &&
                      mouse_x >= dock_x() - icon() && mouse_x < dock_x() + dock_w() + icon();
        want = mouse_y >= H - 2 * S || on_dock ? 0 : 1024;
    }
    if (dock_hide == want) return 0;
    damage_dock();                                     /* where it was */
    int step = reduce_motion ? 1024 : dt * 1024 / 200; /* 200 ms */
    if (step < 1) step = 1;
    dock_hide = want > dock_hide ? (dock_hide + step > want ? want : dock_hide + step)
                                 : (dock_hide - step < want ? want : dock_hide - step);
    damage_dock();
    return 1;
}

static int animate_dock(int dt) {
    slide_dock(dt);
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
            if (d < reach) target += icon() * cfg->dock_magnify / 100 * (reach - d) / reach;
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
    if (w->lay) munmap(w->lay);
    w->lay = 0;
    w->lay_cap = 0;
    w->lay_ok = 0;
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
    if (q->w < 1 || q->h < 1) return SYSERR_BADCALL;
    struct ws_request fit = *q;                    /* too big for this screen: as big as fits */
    if (fit.w > max_w) fit.w = max_w;
    if (fit.h > max_h) fit.h = max_h;
    q = &fit;
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
    r->w = w->w;
    r->h = w->h;
    return 0;
}

/* The green button: fill the screen between the menu bar and the Dock, or go
 * back. The program gets WS_EV_RESIZE and asks for the new surface (WS_RESIZE). */
static void zoom_window(int k) {
    window_t *w = &wins[k];
    if (!w->resizable) return;
    int nx, ny, nw, nh;
    if (!w->zoomed) {
        w->saved_x = w->x;
        w->saved_y = w->y;
        w->saved_w = w->w;
        w->saved_h = w->h;
        nx = 1;
        ny = MENU_H + TITLE_H + 1;
        nw = W - 2;
        nh = H - 1 - ny;                               /* to the bottom: the Dock hides (dock_hide) */
    } else {
        nx = w->saved_x;
        ny = w->saved_y;
        nw = w->saved_w;
        nh = w->saved_h;
    }
    if (nh < 100 * S) nh = 100 * S;
    w->zoomed = !w->zoomed;
    printf("windowserver: zoom %s to %dx%d\n", w->title, nw, nh);   /* (for the tests) */
    w->want_x = nx;
    w->want_y = ny;
    send_event(w, WS_EV_RESIZE, nw, nh, 0);
}

/* WS_RESIZE: a new surface of the size asked, the events and settings carried over. */
static int resize_window(const struct ws_request *q, struct ws_reply *r) {
    int max_w = W - 2, max_h = H - MENU_H - TITLE_H - 2;
    if (q->w < 1 || q->h < 1 || q->w > max_w || q->h > max_h) return SYSERR_BADCALL;
    for (int k = 0; k < MAX_WINDOWS; k++) {
        window_t *w = &wins[k];
        if (!w->used || w->id != q->id || w->pid != q->pid) continue;
        snprintf(r->shm, sizeof(r->shm), "ws.%d.%d", w->id, ++w->gen);
        struct ws_surface *s = shm_map(r->shm, ws_surface_size(q->w, q->h));
        if (!s) return SYSERR_NOMEM;
        memcpy(s, w->s, sizeof(*s));                   /* events, focus, settings */
        s->w = q->w;
        s->h = q->h;
        damage_window(w);                              /* where it was */
        munmap(w->s);
        w->s = s;
        w->seen = s->damage - 1;
        w->w = q->w;
        w->h = q->h;
        w->x = w->want_x ? w->want_x : w->x;
        w->y = w->want_y ? w->want_y : w->y;
        w->want_x = w->want_y = 0;
        if (w->x + w->w > W - 1) w->x = W - 1 - w->w;
        if (w->y + w->h > H - 1) w->y = H - 1 - w->h;
        if (w->x < 1) w->x = 1;
        if (w->y < MENU_H + TITLE_H + 1) w->y = MENU_H + TITLE_H + 1;
        damage_window(w);
        r->id = w->id;
        r->x = w->x;
        r->y = w->y;
        r->w = w->w;
        r->h = w->h;
        return 0;
    }
    return SYSERR_NOENT;
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
        if (q.op == WS_CREATE || q.op == WS_RESIZE) {
            struct ws_reply r;
            memset(&r, 0, sizeof(r));
            int e = q.op == WS_CREATE ? create_window(&q, &r) : resize_window(&q, &r);
            if (e < 0) r.id = e;
            mutex_unlock(&lock);
            if (reply >= 0) port_send(reply, &r, sizeof(r), -1);
        } else if (q.op == WS_APPS) {
            reload_dock();
            mutex_unlock(&lock);
        } else {
            for (int k = 0; k < MAX_WINDOWS; k++)
                if (wins[k].used && wins[k].id == q.id && wins[k].pid == q.pid && q.op == WS_RESIZABLE) {
                    wins[k].resizable = 1;
                } else if (wins[k].used && wins[k].id == q.id && wins[k].pid == q.pid) {
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

static void draw_cursor(painter_t *p) {
    int cw = CURSOR_W * S, ch = CURSOR_H * S;
    for (int j = 0; j < ch; j++)
        for (int i = 0; i < cw; i++) {
            ui_blend_pixel(&p->c, mouse_x + i, mouse_y + j, 0x000000, cursor_black[j * cw + i]);
            ui_blend_pixel(&p->c, mouse_x + i, mouse_y + j, 0xFFFFFF, cursor_white[j * cw + i]);
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

static unsigned mix(unsigned a, unsigned b, int t) {          /* t 0..256 */
    int r = (int)(a >> 16 & 255) + ((int)(b >> 16 & 255) - (int)(a >> 16 & 255)) * t / 256;
    int g = (int)(a >> 8 & 255) + ((int)(b >> 8 & 255) - (int)(a >> 8 & 255)) * t / 256;
    int bl = (int)(a & 255) + ((int)(b & 255) - (int)(a & 255)) * t / 256;
    return (unsigned)(r << 16 | g << 8 | bl);
}

/* A picture file, scaled to cover the screen (the middle of it, cropped),
 * each screen pixel the average of the picture's pixels under it. -> 0 failed. */
static int wall_picture(const char *path) {
    image_t img;
    const char *why;
    if (image_load(path, &img, &why) != 0) {
        printf("windowserver: desktop picture %s: %s\n", path, why);
        return 0;
    }
    long cw = img.width, ch = img.height, x0 = 0, y0 = 0;
    if (cw * H > ch * W) {                             /* wider than the screen: crop the sides */
        cw = ch * W / H;
        x0 = (img.width - cw) / 2;
    } else {
        ch = cw * H / W;
        y0 = (img.height - ch) / 2;
    }
    for (int y = 0; y < H; y++) {
        long sy0 = y0 + y * ch / H, sy1 = y0 + (y + 1) * ch / H;
        if (sy1 <= sy0) sy1 = sy0 + 1;
        for (int x = 0; x < W; x++) {
            long sx0 = x0 + x * cw / W, sx1 = x0 + (x + 1) * cw / W;
            if (sx1 <= sx0) sx1 = sx0 + 1;
            unsigned long r = 0, g = 0, b = 0, n = 0;
            for (long sy = sy0; sy < sy1; sy++)
                for (long sx = sx0; sx < sx1; sx++) {
                    unsigned c = img.pixels[sy * img.width + sx];
                    r += c >> 16 & 255;
                    g += c >> 8 & 255;
                    b += c & 255;
                    n++;
                }
            wallpaper[(long)y * W + x] = (unsigned)(r / n << 16 | g / n << 8 | b / n);
        }
    }
    image_free(&img);
    return 1;
}

static void make_wallpaper(void) {
    if (cfg->wall_kind == WS_WALL_PICTURE && cfg->wall_path[0] && wall_picture(cfg->wall_path)) return;
    if (cfg->wall_kind == WS_WALL_COLOR) {
        for (long i = 0; i < (long)W * H; i++) wallpaper[i] = cfg->wall_color & 0xFFFFFF;
        return;
    }
    unsigned a, b;                                     /* a gradient, a little diagonal */
    ui_wall_gradient(cfg->wall_kind == WS_WALL_GRADIENT ? cfg->wall_gradient : 0, dark, &a, &b);
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++)
            wallpaper[(long)y * W + x] = mix(a, b, (int)(((long)y * 4 + x) * 256 / ((long)H * 4 + W)));
}

/* ---- The blur material -------------------------------------------------------
 * Like NSVisualEffectView's backdrop layer: the picture behind is sampled at a
 * quarter of its resolution (scale 0.25), with a 10 pt bleed so the edges have
 * context, blurred (four 3x3 box passes), saturated and tinted, then scaled up.
 * Only the wallpaper and the windows are sampled (not the Dock itself). */
#define SAMP_W 512
#define SAMP_H 320
#define MAT_MAX (1 << 19)


static int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

static unsigned backdrop_at(int x, int y) {
    for (int i = norder - 1; i >= 0; i--) {
        window_t *w = &wins[order[i]];
        if (w->minimised) continue;
        if (ui_inside(x, y, w->x, w->y, w->w, w->h))
            return ((const unsigned *)((char *)w->s + WS_HEADER))[(long)(y - w->y) * w->w + (x - w->x)];
        if (ui_inside(x, y, w->x, w->y - TITLE_H, w->w, TITLE_H)) return TH.title_on;
    }
    return wallpaper[(long)clampi(y, 0, H - 1) * W + clampi(x, 0, W - 1)];
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
static void material(painter_t *p, int x, int y, int w, int h, int r, int corners, unsigned tint, int tint_a, unsigned solid) {
    if (x + w <= p->c.cx0 || x >= p->c.cx1 || y + h <= p->c.cy0 || y >= p->c.cy1) return;
    int bleed = 10 * S, bw = (w + 2 * bleed + 3) / 4, bh = (h + 2 * bleed + 3) / 4;
    if (reduce_transp || w > 2048 || bw > SAMP_W || bh > SAMP_H || (long)w * h > MAT_MAX) {
        ui_round_ex(&p->c, x, y, w, h, r, solid, 255, corners);
        return;
    }
    for (int cy = 0; cy < bh; cy++)
        for (int cx = 0; cx < bw; cx++)
            p->samp[cy * bw + cx] = backdrop_at(x - bleed + cx * 4 + 2, y - bleed + cy * 4 + 2);
    box3(p->samp, p->samp2, bw, bh, 1);
    box3(p->samp2, p->samp, bw, bh, 0);
    box3(p->samp, p->samp2, bw, bh, 1);
    box3(p->samp2, p->samp, bw, bh, 0);
    for (int i = 0; i < bw * bh; i++) p->samp[i] = saturate_tint(p->samp[i], 180, tint, tint_a);
    for (int i = 0; i < w; i++) {                          /* bilinear, columns once */
        int u = (i + bleed) * 64 - 96, c0 = u >> 8;
        unsigned f = (unsigned)(u & 255);
        if (c0 < 0) { c0 = 0; f = 0; }
        if (c0 >= bw - 1) { c0 = bw - 1; f = 0; }
        p->xi[i] = c0;
        p->xf[i] = f;
    }
    int j0 = p->c.cy0 > y ? p->c.cy0 - y : 0, j1 = p->c.cy1 < y + h ? p->c.cy1 - y : h;   /* (the rest is not drawn) */
    for (int j = j0; j < j1; j++) {
        int v = (j + bleed) * 64 - 96, r0 = v >> 8;
        unsigned rf = (unsigned)(v & 255);
        if (r0 < 0) { r0 = 0; rf = 0; }
        if (r0 >= bh - 1) { r0 = bh - 1; rf = 0; }
        const unsigned *a = p->samp + r0 * bw, *b = rf ? a + bw : a;
        for (int i = 0; i < w; i++) {
            int c = p->xi[i];
            unsigned f = p->xf[i];
            unsigned top = f ? lerp_rgb(a[c], a[c + 1], f) : a[c];
            unsigned bot = f ? lerp_rgb(b[c], b[c + 1], f) : b[c];
            p->mat[j * w + i] = rf ? lerp_rgb(top, bot, rf) : top;
        }
    }
    ui_copy_round(&p->c, x, y, p->mat, w, h, w, r, corners);
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
static void window_shadow(painter_t *p, int x, int y, int ww, int hh, int focused, int a255) {
    ui_shadow(&p->c, x + 2 * S, y + SHADOW / 3, ww - 4 * S, hh - 2 * S, RADIUS, SHADOW,
              (focused ? TH.shadow_on : TH.shadow_off) * a255 / 255);
    ui_shadow(&p->c, x, y, ww, hh, RADIUS, 2 * S, 50 * a255 / 255);        /* a tight edge */
}

/* The window itself (outline, title bar, content), no shadow, onto `cv` with
 * its content at (cx, cy). */
static void window_frame(ui_canvas *cv, window_t *w, int cx, int cy, int focused, int hover) {
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
}

static void window_content(ui_canvas *cv, window_t *w, int cx, int cy) {
    const unsigned *px = (const unsigned *)((char *)w->s + WS_HEADER);
    if (w->gpu_body && cx == w->x && cy == w->y) {      /* the GPU copies the middle (gfx_copy) */
        int a = w->gpu_y0 - cy, b = w->gpu_y1 - cy;
        if (a > 0) ui_copy(cv, cx, cy, px, w->w, a, w->w);
        ui_copy_round(cv, cx, cy + b, px + (long)b * w->w, w->w, w->h - b, w->w, RADIUS - 1, UI_BL | UI_BR);
        return;
    }
    ui_copy_round(cv, cx, cy, px, w->w, w->h, w->w, RADIUS - 1, UI_BL | UI_BR);
}

static void window_body(ui_canvas *cv, window_t *w, int cx, int cy, int focused, int hover) {
    window_frame(cv, w, cx, cy, focused, hover);
    window_content(cv, w, cx, cy);
}

/* ---- Window pictures -----------------------------------------------------
 * A window's shadow, rim and title bar change rarely (focus, title, size, the
 * traffic lights' hover, the theme) but were drawn again in every frame that
 * touched the window: the shadow's maths, the title's glyphs. They are drawn
 * once into a picture with transparency (drawn on black and on white: the
 * difference is how transparent each pixel is), which frames then blend. */
static unsigned *lay_scratch;
static long lay_scratch_n;

static int lay_margin(void) { return SHADOW + SHADOW / 3 + 2 * S; }

static int layer_current(const window_t *w, int focused, int hover) {
    int ww = w->w + 2, hh = w->h + TITLE_H + 2, m = lay_margin();
    return w->lay && w->lay_ok && w->lay_w == ww + 2 * m && w->lay_h == hh + 2 * m && w->lay_focus == focused &&
           w->lay_hover == hover && w->lay_theme == theme_gen && !strcmp(w->lay_title, w->title);
}

/* Before the painters start (it allocates, and they share the picture). */
static void layer_update(painter_t *p, window_t *w, int focused, int hover) {
    if (layer_current(w, focused, hover)) return;
    int x, y, ww, hh, m = lay_margin();
    outer(w, &x, &y, &ww, &hh);
    int lw = ww + 2 * m, lh = hh + 2 * m;
    long n = (long)lw * lh;
    w->lay_ok = 0;
    if (n > w->lay_cap) {
        if (w->lay) munmap(w->lay);
        w->lay = mmap((size_t)n * 4, PROT_READ | PROT_WRITE, -1);
        w->lay_cap = w->lay ? n : 0;
        if (!w->lay) return;
    }
    if (n > lay_scratch_n) {
        if (lay_scratch) munmap(lay_scratch);
        lay_scratch = mmap((size_t)n * 4, PROT_READ | PROT_WRITE, -1);
        lay_scratch_n = lay_scratch ? n : 0;
        if (!lay_scratch) return;
    }
    painter_t t = *p;
    for (int pass = 0; pass < 2; pass++) {             /* on black, then on white */
        unsigned *buf = pass ? lay_scratch : w->lay;
        unsigned bg = pass ? 0xFFFFFF : 0;
        for (long i = 0; i < n; i++) buf[i] = bg;
        ui_canvas_init(&t.c, buf, lw, lh, lw);
        window_shadow(&t, m, m, ww, hh, focused, 255);
        window_frame(&t.c, w, w->x - x + m, w->y - y + m, focused, hover);
    }
    for (long i = 0; i < n; i++) {                     /* alpha = 255 - (white - black) */
        unsigned b = w->lay[i], wh = lay_scratch[i];
        int a = 255 - ((int)((wh >> 8) & 255) - (int)((b >> 8) & 255));
        if (a < 0) a = 0;
        if (a > 255) a = 255;
        w->lay[i] = ((unsigned)a << 24) | (b & 0xFFFFFF);   /* (black's colour is premultiplied) */
    }
    w->lay_w = lw;
    w->lay_h = lh;
    w->lay_focus = focused;
    w->lay_hover = hover;
    w->lay_theme = theme_gen;
    snprintf(w->lay_title, sizeof(w->lay_title), "%s", w->title);
    w->lay_ok = 1;
}

/* The picture onto the frame; the content's rows are left out (the content
 * covers them) except its rounded bottom, whose corners need the rim behind. */
static void layer_blit(ui_canvas *c, const window_t *w) {
    int m = lay_margin(), lx = w->x - 1 - m, ly = w->y - TITLE_H - 1 - m;
    int x0 = lx < c->cx0 ? c->cx0 : lx, x1 = lx + w->lay_w > c->cx1 ? c->cx1 : lx + w->lay_w;
    int y0 = ly < c->cy0 ? c->cy0 : ly, y1 = ly + w->lay_h > c->cy1 ? c->cy1 : ly + w->lay_h;
    int hole_y1 = w->y + w->h - RADIUS;
    for (int py = y0; py < y1; py++) {
        unsigned *row = c->px + (long)py * c->stride;
        const unsigned *src = w->lay + (long)(py - ly) * w->lay_w - lx;
        int hole = py >= w->y && py < hole_y1;
        for (int px = x0; px < x1; px++) {
            if (hole && px == w->x) {                  /* skip the content */
                px = w->x + w->w - 1;
                continue;
            }
            unsigned v = src[px], a = v >> 24;
            if (a == 255) row[px] = v & 0xFFFFFF;
            else if (a) {
                unsigned d = row[px], k = 255 - a;
                unsigned r = ((v >> 16) & 255) + ((d >> 16) & 255) * k / 255;
                unsigned g = ((v >> 8) & 255) + ((d >> 8) & 255) * k / 255;
                unsigned bl = (v & 255) + (d & 255) * k / 255;
                row[px] = (r > 255 ? 255 : r) << 16 | (g > 255 ? 255 : g) << 8 | (bl > 255 ? 255 : bl);
            }
        }
    }
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
static void blit_scaled(painter_t *p, int dx, int dy, int dw, int dh, int ow, int oh, int alpha,
                        const unsigned *under, int ux, int uy, int uw) {
    int x0 = dx < p->c.cx0 ? p->c.cx0 : dx, y0 = dy < p->c.cy0 ? p->c.cy0 : dy;
    int x1 = dx + dw > p->c.cx1 ? p->c.cx1 : dx + dw, y1 = dy + dh > p->c.cy1 ? p->c.cy1 : dy + dh;
    if (dw <= 0 || dh <= 0) return;
    for (int py = y0; py < y1; py++) {
        int v = ((2 * (py - dy) + 1) * oh * 128) / dh - 128;                         /* 1/256 texels */
        int ty = v >> 8;
        unsigned fy = (unsigned)(v & 255);
        if (ty < 0) { ty = 0; fy = 0; }
        if (ty >= oh - 1) { ty = oh - 1; fy = 0; }
        const unsigned *c0 = anim_col + (long)ty * ow, *c1 = fy ? c0 + ow : c0;
        const unsigned *m0 = anim_mask + (long)ty * ow, *m1 = fy ? m0 + ow : m0;
        unsigned *row = p->c.px + (long)py * p->c.stride;
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

static void draw_window_anim(painter_t *p, int k, int focused, int hover) {
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
    int ux = dx < p->c.cx0 ? p->c.cx0 : dx, uy = dy < p->c.cy0 ? p->c.cy0 : dy;
    int ux1 = dx + dw > p->c.cx1 ? p->c.cx1 : dx + dw, uy1 = dy + dh > p->c.cy1 ? p->c.cy1 : dy + dh;
    int uw = ux1 - ux;
    const unsigned *under = 0;
    if (alpha < 255 && snap && uw > 0 && uy1 > uy && (long)uw * (uy1 - uy) <= anim_cap) {
        for (int y = uy; y < uy1; y++)
            memcpy(snap + (long)(y - uy) * uw, p->c.px + (long)y * p->c.stride + ux, (size_t)uw * 4);
        under = snap;
    }
    window_shadow(p, dx, dy, dw, dh, focused, alpha);
    blit_scaled(p, dx, dy, dw, dh, ow, oh, alpha, under, ux, uy, uw);
}

static void draw_window(painter_t *p, int k, int front_win) {
    window_t *w = &wins[k];
    if (w->minimised || w->anim == ANIM_WAIT) return;
    int exiting = w->anim == ANIM_CLOSE || w->anim == ANIM_MIN;
    int focused = front_win || exiting;                                          /* keeps its look while leaving */
    int hover = tl_hover >= 0 && &wins[tl_hover] == w;
    if (w->anim != ANIM_NONE && anim_ok(w)) {
        draw_window_anim(p, k, focused, hover);
        return;
    }
    if (layer_current(w, focused, hover)) {
        layer_blit(&p->c, w);
        window_content(&p->c, w, w->x, w->y);
        return;
    }
    int x, y, ww, hh;
    outer(w, &x, &y, &ww, &hh);
    window_shadow(p, x, y, ww, hh, focused, 255);
    window_body(&p->c, w, w->x, w->y, focused, hover);
}

static void draw_menu_bar(painter_t *p) {
    ui_fill_a(&p->c, 0, 0, W, MENU_H, TH.bar_tint, TH.bar_alpha);           /* translucent */
    ui_fill_a(&p->c, 0, MENU_H - 1, W, 1, TH.sep, TH.sep_alpha);
    int ty = (MENU_H - 16 * S) / 2, x = 16 * S;
    int tw = ui_text_style_width("Terminal OS", S, UI_BOLD);
    bar_x0[1] = x - 8 * S;
    bar_x1[1] = x + tw + 8 * S;
    if (menu_open == 1)
        ui_round_ex(&p->c, bar_x0[1], 2 * S, bar_x1[1] - bar_x0[1], MENU_H - 4 * S, R_SMALL * S, TH.hl, TH.hl_alpha, UI_ALL);
    ui_text_bold(&p->c, x, ty, "Terminal OS", TH.label, S);
    window_t *f = front();
    x = bar_x1[1] + 8 * S;
    x += ui_text_bold(&p->c, x, ty, f ? app_name(f) : "Desktop", TH.label, S) + 16 * S;
    int ww = ui_text_width("Window", S);
    bar_x0[2] = x - 8 * S;
    bar_x1[2] = x + ww + 8 * S;
    if (menu_open == 2)
        ui_round_ex(&p->c, bar_x0[2], 2 * S, bar_x1[2] - bar_x0[2], MENU_H - 4 * S, R_SMALL * S, TH.hl, TH.hl_alpha, UI_ALL);
    ui_text(&p->c, x, ty, "Window", TH.label, S);

    int rx = W - 16 * S - ui_text_width(clock_text, S);
    ui_text(&p->c, rx, ty, clock_text, TH.label, S);
    int is = 16 * S, iy = (MENU_H - is) / 2;
    status_x[0] = status_x[1] = status_x[2] = 0;
    rx -= 16 * S;
    if (vol_level >= 0) {                              /* sound */
        rx -= is;
        status_x[2] = rx;
        ui_icon(&p->c, rx, iy, is, "sound", vol_level ? 255 : 90);
        rx -= 12 * S;
    }
    if (wifi_on >= 0) {                                /* Wi-Fi: faded when not connected */
        rx -= is;
        status_x[1] = rx;
        ui_icon(&p->c, rx, iy, is, "wifi", wifi_on ? 255 : 80);
        rx -= 12 * S;
    }
    if (bat >= 0) {                                    /* battery and percent */
        char pct[8];
        snprintf(pct, sizeof(pct), "%d%%", bat & 0xFF);
        int pw = ui_text_width(pct, S);
        rx -= pw;
        ui_text(&p->c, rx, ty, pct, TH.label, S);
        rx -= is + 4 * S;
        status_x[0] = rx;
        ui_icon(&p->c, rx, iy, is, "battery", 255);
    }
}

static void draw_dock(painter_t *p) {
    int x = dock_x(), w = dock_w(), y = dock_bottom() - dock_h(), r = R_XXL * S;
    ui_shadow_ex(&p->c, x, y + 2 * S, w, dock_h(), r, 12 * S, 60, 1);          /* not under the glass */
    ui_round_ex(&p->c, x - 1, y - 1, w + 2, dock_h() + 2, r + 1, TH.dock_rim, TH.dock_rim_alpha, UI_ALL);
    material(p, x, y, w, dock_h(), r, UI_ALL, TH.dock_tint, TH.dock_alpha, TH.dock_solid);
    int hot = dock_hit(mouse_x, mouse_y);
    for (int i = 0; i < DOCK_APPS; i++) {
        dock_app_t *a = &apps[i];
        int size = a->size, ix = icon_x(i), iy = dock_bottom() - pad() - size - (int)(a->bounce / FP), rr = size * 22 / 100;
        if (ui_icon(&p->c, ix, iy, size, a->icon, 255) != 0) {   /* no icon file: a drawn one */
            ui_shadow(&p->c, ix, iy + size / 30, size, size, rr, size / 12 + 1, 70);
            ui_round(&p->c, ix, iy, size, size, rr, a->color);
            int ts = size / 22;
            if (ts < 1) ts = 1;
            if (ts > 4) ts = 4;
            int gw = ui_text_style_width(a->glyph, ts, UI_BOLD);
            ui_text_bold(&p->c, ix + (size - gw) / 2, iy + (size - 16 * ts) / 2, a->glyph, 0xFFFFFF, ts);
        }
        if (a->pid > 0 && cfg->dock_dots)                         /* the running dot */
            ui_circle(&p->c, ix + size / 2, dock_bottom() - pad() / 2 + S, 2 * S, dark ? 0xE5E5E7 : 0x3A3A3E);
        if (i == hot && !menu_open) {                          /* the name, above the icon */
            int ta = (int)(255 * ease_out((long)tip_ms * 1024 / FADE_MS) / 1024);      /* fades in over 120 ms */
            int lw = ui_text_width(a->name, S) + 16 * S, lh = 24 * S;
            int lx = ix + size / 2 - lw / 2, ly = dock_bottom() - dock_h() - lh - 8 * S - (size - icon());
            ui_round_ex(&p->c, lx, ly, lw, lh, R_MEDIUM * S, TH.tooltip, 225 * ta / 255, UI_ALL);
            ui_text(&p->c, lx + 8 * S, ly + (lh - 16 * S) / 2, a->name,
                    lerp_rgb(TH.tooltip, 0xFFFFFF, (unsigned)(ta + (ta >> 7))), S);
        }
    }
}

static void draw_menu(painter_t *p) {
    if (!menu_open) return;
    int x = menu_x(), y = menu_y(), w = menu_w(), h = menu_h(), r = R_LARGE * S;
    ui_shadow_ex(&p->c, x, y + 4 * S, w, h, r, 14 * S, 90, 1);                  /* blur 28 / 2 */
    ui_round_ex(&p->c, x - 1, y - 1, w + 2, h + 2, r + 1, TH.hair, TH.hair_alpha, UI_ALL);
    material(p, x, y, w, h, r, UI_ALL, TH.menu_tint, TH.menu_alpha, TH.menu_solid);
    for (int i = 0; i < n_mitems; i++) {
        int iy = menu_item_y(i), ih = mi_h(i);
        if (mitems[i].flags & MI_SEP) {
            ui_fill_a(&p->c, x + 8 * S, iy + ih / 2, w - 16 * S, 1, TH.sep, TH.sep_alpha);
            continue;
        }
        int off = mitems[i].flags & MI_OFF, hot = i == menu_hot && !off;
        if (hot) ui_round(&p->c, x + 4 * S, iy, w - 8 * S, ih, R_SMALL * S, TH.accent);   /* 8 - 4 padding */
        unsigned fg = hot ? 0xFFFFFF : off ? TH.secondary : TH.label;
        int ty = iy + (ih - 16 * S) / 2;
        ui_text(&p->c, x + 16 * S, ty, mitems[i].label, fg, S);
        if (mitems[i].key[0]) {
            int kw = ui_text_width(mitems[i].key, S);
            ui_text(&p->c, x + w - 16 * S - kw, ty, mitems[i].key, hot ? 0xFFFFFF : TH.secondary, S);
        }
    }
}

/* The menu, fading in over 120 ms (ease-out): what is behind it is saved, the
 * menu drawn, then the two mixed. A micro-interaction, so Reduce Motion keeps it. */
static void draw_menu_faded(painter_t *p) {
    if (!menu_open) return;
    int a = (int)(255 * ease_out((long)menu_fade_ms * 1024 / FADE_MS) / 1024);
    int x0 = menu_x() - SHADOW, y0 = menu_y() - SHADOW, x1 = menu_x() + menu_w() + SHADOW, y1 = menu_y() + menu_h() + SHADOW;
    if (x0 < p->c.cx0) x0 = p->c.cx0;
    if (y0 < p->c.cy0) y0 = p->c.cy0;
    if (x1 > p->c.cx1) x1 = p->c.cx1;
    if (y1 > p->c.cy1) y1 = p->c.cy1;
    if (a >= 255 || !snap || x1 <= x0 || y1 <= y0 || (long)(x1 - x0) * (y1 - y0) > anim_cap) {
        draw_menu(p);
        return;
    }
    for (int y = y0; y < y1; y++)
        memcpy(snap + (long)(y - y0) * (x1 - x0), p->c.px + (long)y * p->c.stride + x0, (size_t)(x1 - x0) * 4);
    draw_menu(p);
    for (int y = y0; y < y1; y++) {
        unsigned *row = p->c.px + (long)y * p->c.stride + x0;
        const unsigned *old = snap + (long)(y - y0) * (x1 - x0);
        for (int x = 0; x < x1 - x0; x++) row[x] = lerp_rgb(old[x], row[x], (unsigned)(a + (a >> 7)));
    }
}

/* The damaged parts of rows [by0, by1). */
static void paint(painter_t *p, int by0, int by1) {
    window_t *f = front();
    for (int d = 0; d < ndmg; d++) {
        int x0 = dmg[d].x0, y0 = dmg[d].y0 > by0 ? dmg[d].y0 : by0, x1 = dmg[d].x1, y1 = dmg[d].y1 < by1 ? dmg[d].y1 : by1;
        if (y1 <= y0) continue;
        ui_clip(&p->c, x0, y0, x1 - x0, y1 - y0);
        ui_copy(&p->c, x0, y0, wallpaper + (long)y0 * W + x0, x1 - x0, y1 - y0, W);
        for (int i = 0; i < norder; i++) draw_window(p, order[i], &wins[order[i]] == f);
        draw_menu_bar(p);
        draw_dock(p);
        draw_menu_faded(p);
        if (!kernel_cursor) draw_cursor(p);
    }
}

static void paint_band(int k) { paint(&painters[k], H * k / n_painters, H * (k + 1) / n_painters); }

/* A painter thread: a band of every big frame. */
static void painter(void *arg) {
    int k = (int)(long)arg, seen = 0;
    for (;;) {
        while (paint_job == seen) futex(FUTEX_WAIT, &paint_job, seen);
        seen = paint_job;
        paint_band(k);
        if (__sync_sub_and_fetch(&paint_left, 1) == 0) futex(FUTEX_WAKE, &paint_left, 0);
    }
}

/* Window motion and the menu's fade share scratch buffers: one core then. */
static int motion(void) {
    if (menu_open && menu_fade_ms < FADE_MS) return 1;
    for (int k = 0; k < MAX_WINDOWS; k++)
        if (wins[k].used && wins[k].anim != ANIM_NONE) return 1;
    return 0;
}

/* Window contents the GPU copies into the frame (gfx_copy) instead of the
 * CPU: the rows of a window that nothing is drawn over (no window above it,
 * no open menu; the menu bar's and the Dock's rows stay with the CPU), down to
 * where its rounded corners start. The CPU draws the rest, then the GPU fills
 * these in, then copies the frame to the screen. */
static int gpu_frames;                                 /* gfx_surface took our frame */

static int overlaps(int ax0, int ay0, int ax1, int ay1, int bx, int by, int bw, int bh) {
    return ax0 < bx + bw && bx < ax1 && ay0 < by + bh && by < ay1;
}

/* The highest row the Dock draws on now: magnified and bouncing icons (and
 * their shadows), the name shown above the one under the pointer. */
static int dock_reach(void) {
    int top = dock_bottom() - dock_h();
    for (int i = 0; i < DOCK_APPS; i++) {
        int size = apps[i].size;
        int iy = dock_bottom() - pad() - size - (int)(apps[i].bounce / FP) - size / 12 - 2;
        int ly = dock_bottom() - dock_h() - 24 * S - 8 * S - (size - icon());   /* as draw_dock places it */
        if (iy < top) top = iy;
        if (ly < top) top = ly;
    }
    return top - 2 * S;
}

static void plan_gpu_copies(void) {
    static struct os_copy items[64];
    int n = 0;
    for (int k = 0; k < MAX_WINDOWS; k++) wins[k].gpu_body = 0;
    if (!gpu_frames || !kernel_cursor || motion()) return;
    int dock_top = dock_reach();
    for (int i = 0; i < norder; i++) {
        window_t *w = &wins[order[i]];
        if (!live(w) || w->minimised || w->anim != ANIM_NONE) continue;
        int x0 = w->x < 0 ? 0 : w->x, x1 = w->x + w->w > W ? W : w->x + w->w;
        int y0 = w->y < MENU_H ? MENU_H : w->y, y1 = w->y + w->h - RADIUS;
        if (y1 > dock_top) y1 = dock_top;
        if (x1 <= x0 || y1 <= y0) continue;
        int covered = menu_open && overlaps(x0, y0, x1, y1, menu_x() - SHADOW, menu_y() - SHADOW,
                                            menu_w() + 2 * SHADOW, menu_h() + 2 * SHADOW);
        for (int j = i + 1; j < norder && !covered; j++) {
            window_t *u = &wins[order[j]];
            int ox, oy, ow, oh;
            if (!live(u) || u->minimised) continue;
            outer(u, &ox, &oy, &ow, &oh);
            covered = overlaps(x0, y0, x1, y1, ox - SHADOW, oy - SHADOW, ow + 2 * SHADOW, oh + 2 * SHADOW);
        }
        if (covered) continue;
        int first = n;
        for (int d = 0; d < ndmg && n <= 64; d++) {    /* what changed of it */
            int a0 = dmg[d].x0 > x0 ? dmg[d].x0 : x0, a1 = dmg[d].x1 < x1 ? dmg[d].x1 : x1;
            int b0 = dmg[d].y0 > y0 ? dmg[d].y0 : y0, b1 = dmg[d].y1 < y1 ? dmg[d].y1 : y1;
            if (a1 <= a0 || b1 <= b0) continue;
            if (n == 64) {
                n = 65;
                break;
            }
            items[n++] = (struct os_copy){ (const unsigned *)((char *)w->s + WS_HEADER), w->w, w->h,
                                           a0, b0, a1 - a0, b1 - b0, a0 - w->x, b0 - w->y };
        }
        if (n > 64) {                                  /* too many pieces: the CPU draws this one */
            n = first;
            break;
        }
        w->gpu_body = 1;
        w->gpu_y0 = y0;
        w->gpu_y1 = y1;
    }
    if (n && gfx_copy(items, n) != 0)
        for (int k = 0; k < MAX_WINDOWS; k++) wins[k].gpu_body = 0;
}

/* -> 1 if the frame was paced by the screen (page flipping): no sleep then. */
static int compose(void) {
    frame_no++;
    plan_gpu_copies();
    window_t *f = front();
    for (int i = 0; i < norder; i++) {                 /* window pictures, before the painters share them */
        window_t *w = &wins[order[i]];
        if (w->used && !w->minimised && w->anim == ANIM_NONE)
            layer_update(&painters[0], w, w == f, tl_hover >= 0 && &wins[tl_hover] == w);
    }
    long area = 0;
    for (int d = 0; d < ndmg; d++) area += (long)(dmg[d].x1 - dmg[d].x0) * (dmg[d].y1 - dmg[d].y0);
    if (n_painters > 1 && frame_no > 1 && area >= PAINT_MIN && !motion()) {   /* (the first frame loads the font) */
        paint_left = n_painters - 1;
        __sync_add_and_fetch(&paint_job, 1);
        futex(FUTEX_WAKE, &paint_job, 0);
        paint_band(0);
        int left;
        while ((left = paint_left) != 0) futex(FUTEX_WAIT, &paint_left, left);
    } else if (ndmg) {
        paint(&painters[0], 0, H);
    }
    for (int d = 0; d < ndmg; d++) {
        int x0 = dmg[d].x0, y0 = dmg[d].y0, x1 = dmg[d].x1, y1 = dmg[d].y1;
        gfx_blit(x0, y0, x1 - x0, y1 - y0, scr.px + (long)y0 * scr.stride + x0, scr.stride);
    }
    if (ndmg) unpresented = 1;
    ndmg = 0;
    return unpresented;                                /* main() presents it, outside the lock */
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
    printf("windowserver: open %s: %s\n", apps[i].program, pid > 0 ? "started" : "cannot start");   /* (in the log) */
    if (pid > 0) {
        apps[i].pid = pid;
        apps[i].launching = !reduce_motion && cfg->dock_bounce;
        apps[i].launch_at = uptime_ms();
        apps[i].bounce = 0;
        apps[i].bvel = 310L * S * FP;
    }
    damage_dock();
}

static void quit(void);

/* The programs run() started, by pid: a crash report can name them. */
static struct { int pid; char name[16]; } started[16];
static int n_started;

static void run(const char *program, const char *arg) {
    char *argv[] = { (char *)program, (char *)arg, 0 };
    int pid = spawn(program, argv, 0);
    printf("windowserver: open %s: %s\n", program, pid > 0 ? "started" : "cannot start");
    if (pid <= 0) return;
    int k = n_started++ % 16;
    started[k].pid = pid;
    snprintf(started[k].name, sizeof(started[k].name), "%s", program);
}

/* Tell every window (ws.h `prefs`) when a setting changes. */
static void push_prefs(void) {
    for (int k = 0; k < MAX_WINDOWS; k++)
        if (wins[k].used) {
            wins[k].s->prefs = prefs_bits();
            send_event(&wins[k], WS_EV_PREFS, 0, 0, 0);     /* so the app redraws in the new look */
        }
}

static void menu_choose(int id) {
    if (id == M_ABOUT) run("about", 0);
    else if (id == M_SETTINGS) run("settings", 0);
    else if (id == M_ACTIVITY) run("activity", 0);
    else if (id == M_TERMINAL) run("terminal", 0);
    else if (id == M_APPEARANCE) {
        cfg->dark = !cfg->dark;
        cfg->seq++;                                    /* (applied with the next frame) */
    } else if (id == M_MOTION) {
        cfg->reduce_motion = !cfg->reduce_motion;
        cfg->seq++;
    } else if (id == M_TRANSP) {
        cfg->reduce_transp = !cfg->reduce_transp;
        cfg->seq++;
    } else if (id == M_CONTRAST) {
        cfg->contrast = !cfg->contrast;
        cfg->seq++;
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
        if (ui_inside(mouse_x, mouse_y, w->x + LIGHT_X(2) - r, cy - r, 2 * r, 2 * r)) {
            zoom_window(k);                            /* the green button */
            return;
        }
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

/* A program the desktop started crashed: say so, like macOS's "quit
 * unexpectedly" (crashrep; the details are in the log, which Console shows). */
static void crashed(int pid, const char *name, int status) {
    static int reporter;
    if (pid == reporter) return;                       /* not about itself */
    for (int k = 0; !name && k < MAX_WINDOWS; k++)
        if (wins[k].used && wins[k].pid == pid) name = wins[k].title;
    for (int k = 0; !name && k < 16; k++)
        if (started[k].pid == pid) name = started[k].name;
    const char *why = status == 139 ? "it used memory it may not (a segmentation fault)"
                    : status == 132 ? "it ran an invalid instruction"
                                    : "it divided by zero";
    char *argv[] = { "crashrep", (char *)(name && name[0] ? name : "A program"), (char *)why, 0 };
    reporter = spawn("crashrep", argv, 0);
}

static void tidy(void) {
    int status, pid;
    while ((pid = wait(-1, &status, WAIT_NOHANG)) > 0) {
        const char *name = 0;
        for (int i = 0; i < DOCK_APPS; i++)
            if (apps[i].pid == pid) {
                name = apps[i].name;
                apps[i].pid = 0;
                apps[i].launching = 0;
                apps[i].bounce = apps[i].bvel = 0;
                damage_dock();
            }
        if (name) printf("windowserver: %s quit (%d)\n", name, status);
        if (status == 139 || status == 132 || status == 136)   /* EXIT_SEGFAULT, _ILLEGAL, _ARITHMETIC */
            crashed(pid, name, status);
    }
    for (int k = 0; k < MAX_WINDOWS; k++)            /* windows of programs that ended */
        if (wins[k].used && sigsend(wins[k].pid, 0) < 0) remove_window(k);
    static const char *months[] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                    "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };
    static const char *days[] = { "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat" };
    if (save_at && uptime_ms() >= save_at) save_settings();   /* (a slider being dragged: once it rests) */
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
    char date[16] = "", hm[16], sec[8] = "";
    if (cfg->clock_date)
        snprintf(date, sizeof(date), "%s %d %s  ", days[weekday(t.year, t.month, t.day)], t.day, months[(t.month + 11) % 12]);
    if (cfg->clock_seconds) snprintf(sec, sizeof(sec), ":%02d", t.second);
    if (cfg->clock_24h) snprintf(hm, sizeof(hm), "%02d:%02d%s", t.hour, t.minute, sec);
    else snprintf(hm, sizeof(hm), "%d:%02d%s %s", h12, t.minute, sec, t.hour < 12 ? "AM" : "PM");
    snprintf(now, sizeof(now), "%s%s", date, hm);
    if (strcmp(now, clock_text)) {
        strcpy(clock_text, now);
        damage_menu_bar();
    }
}

/* ---- The settings (ws.h): DESKTOP.CFG in the home folder ------------------------- */

static void settings_path(char *out, int n) {
    struct os_user me;
    size_t l;
    if (getuser(&me) == 0 && (l = strlen(me.home)) > 0)
        snprintf(out, n, "%s%sDESKTOP.CFG", me.home, me.home[l - 1] == '/' ? "" : "/");
    else snprintf(out, n, "/DESKTOP.CFG");
}

static void default_settings(struct ws_settings *c) {
    memset(c, 0, sizeof(*c));
    c->dock_size = 48;
    c->dock_magnify = 70;
    c->dock_dots = c->dock_bounce = c->clock_date = 1;
    pointer_settings(&c->pointer, 0);                  /* the kernel's (its defaults) */
}

static const struct { const char *key; int off; } keys[] = {
#define K(f) { #f, (int)__builtin_offsetof(struct ws_settings, f) }
    K(dark), K(contrast), K(reduce_motion), K(reduce_transp), K(accent), K(wall_kind), K(wall_gradient),
    K(dock_size), K(dock_magnify), K(dock_autohide), K(dock_dots), K(dock_bounce), K(clock_24h),
    K(clock_date), K(clock_seconds),
#define P(f) { #f, (int)__builtin_offsetof(struct ws_settings, pointer.f) }
    P(mouse_speed), P(pad_speed), P(accel), P(natural), P(wheel_reverse), P(scroll_speed),
    P(tap_click), P(tap_right), P(tap_drag), P(swap_buttons),
#undef P
#undef K
};

static void load_settings(void) {
    default_settings(cfg);
    char path[96], text[2048];
    settings_path(path, sizeof(path));
    int fd = open(path, OPEN_READ);
    if (fd < 0) return;
    long n = read(fd, text, sizeof(text) - 1);
    close(fd);
    if (n <= 0) return;
    text[n] = 0;
    for (char *line = text; line && *line;) {
        char *next = strchr(line, '\n');
        if (next) *next++ = 0;
        char *eq = strchr(line, '=');
        if (eq) {
            *eq = 0;
            const char *v = eq + 1;
            if (!strcmp(line, "wall_color")) cfg->wall_color = (unsigned)strtoul(v, 0, 16);
            else if (!strcmp(line, "wall_path")) snprintf(cfg->wall_path, sizeof(cfg->wall_path), "%s", v);
            for (unsigned k = 0; k < sizeof(keys) / sizeof(keys[0]); k++)
                if (!strcmp(line, keys[k].key)) *(int *)((char *)cfg + keys[k].off) = atoi(v);
        }
        line = next;
    }
}

static void save_settings(void) {
    save_at = 0;
    char path[96], text[2048];
    int n = 0;
    for (unsigned k = 0; k < sizeof(keys) / sizeof(keys[0]); k++)
        n += snprintf(text + n, sizeof(text) - n, "%s=%d\n", keys[k].key, *(int *)((char *)cfg + keys[k].off));
    n += snprintf(text + n, sizeof(text) - n, "wall_color=%06x\nwall_path=%s\n", cfg->wall_color, cfg->wall_path);
    settings_path(path, sizeof(path));
    int fd = open(path, OPEN_WRITE);
    if (fd < 0) return;
    write(fd, text, (size_t)n);
    close(fd);
}

/* Put the settings in force: the look, the wallpaper (if it changed), the
 * Dock, the clock; every window hears of it (WS_EV_PREFS). */
static void apply_settings(int first) {
    static struct ws_settings was;
    struct ws_settings now = *(struct ws_settings *)cfg;
    if (now.dock_size < 32) now.dock_size = cfg->dock_size = 32;
    if (now.dock_size > 96) now.dock_size = cfg->dock_size = 96;
    if (now.dock_magnify < 0 || now.dock_magnify > 100) now.dock_magnify = cfg->dock_magnify = 70;
    if (first || memcmp(&now.pointer, &was.pointer, sizeof(now.pointer)))
        pointer_settings(&cfg->pointer, 1);           /* the kernel moves the pointer */
    int wall = first || now.wall_kind != was.wall_kind || now.wall_gradient != was.wall_gradient ||
               now.wall_color != was.wall_color || strcmp(now.wall_path, was.wall_path) ||
               (now.dark != was.dark && now.wall_kind == WS_WALL_GRADIENT && now.wall_gradient == 0);
    unsigned before = prefs_bits();
    dark = now.dark != 0;
    contrast = now.contrast != 0;
    reduce_motion = now.reduce_motion != 0;
    reduce_transp = now.reduce_transp != 0;
    accent = now.accent >= 0 && now.accent < WS_ACCENTS ? now.accent : 0;
    int look = first || prefs_bits() != before;
    if (look) apply_theme();
    if (wall) make_wallpaper();
    if (look && !first) push_prefs();                  /* the apps redraw only for a new look */
    clock_text[0] = 0;                                 /* (its format may have changed) */
    int dock = now.dock_size != was.dock_size || now.dock_magnify != was.dock_magnify || now.dock_autohide != was.dock_autohide;
    if (dock)                                          /* a new size: at once (the old Dock is repainted below) */
        for (int i = 0; i < DOCK_APPS; i++) {
            apps[i].size = icon();
            apps[i].sp.pos = (long)icon() * FP;
            apps[i].sp.vel = 0;
        }
    if (look || wall || dock) damage(0, 0, W, H);
    else {                                             /* the Dock or the clock: just those */
        damage_dock();
        damage_menu_bar();
    }
    was = now;
}

/* Each frame: did System Settings (or the menu) change something? */
static void check_settings(void) {
    if (cfg->seq == cfg_seen) return;
    cfg_seen = cfg->seq;
    apply_settings(0);
    save_at = uptime_ms() + 1000;
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
    wallpaper = mmap((size_t)W * H * 4, PROT_READ | PROT_WRITE, -1);
    if (!px || !wallpaper) {
        printf("windowserver: out of memory\n");
        return 1;
    }
    ui_canvas_init(&scr, px, W, H, W);
    cfg = shm_map(WS_SETTINGS, sizeof(struct ws_settings));
    if (!cfg) cfg = &cfg_local;                        /* (no shared memory: the defaults, unshared) */
    load_settings();
    cfg->magic = WS_SETTINGS_MAGIC;
    cfg_seen = cfg->seq;
    apply_settings(1);
    long cap = (long)W * H < (1L << 20) ? (long)W * H : (1L << 20);       /* 4 MB each */
    anim_col = mmap((size_t)cap * 4, PROT_READ | PROT_WRITE, -1);
    anim_mask = mmap((size_t)cap * 4, PROT_READ | PROT_WRITE, -1);
    snap = mmap((size_t)cap * 4, PROT_READ | PROT_WRITE, -1);
    anim_cap = anim_col && anim_mask && snap ? cap : 0;                  /* else: no window motion, no menu fade */
    int cores = cpu_count();                           /* painters: one per core, up to 8 */
    for (int k = 0; k < MAX_PAINTERS && (k == 0 || k < cores); k++) {
        painter_t *pt = &painters[k];
        pt->c = scr;
        pt->samp = mmap(SAMP_W * SAMP_H * 4, PROT_READ | PROT_WRITE, -1);
        pt->samp2 = mmap(SAMP_W * SAMP_H * 4, PROT_READ | PROT_WRITE, -1);
        pt->mat = mmap(MAT_MAX * 4, PROT_READ | PROT_WRITE, -1);
        pt->xf = mmap(2048 * 4, PROT_READ | PROT_WRITE, -1);
        pt->xi = mmap(2048 * 4, PROT_READ | PROT_WRITE, -1);
        if (!pt->samp || !pt->samp2 || !pt->mat || !pt->xf || !pt->xi) {
            if (k == 0) {
                printf("windowserver: out of memory\n");
                return 1;
            }
            break;
        }
        n_painters = k + 1;
    }
    make_cursor();
    load_dock();
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
    for (int k = 1; k < n_painters; k++) thread_create(painter, (void *)(long)k);
    for (int i = 1; i < argc; i++) {                   /* apps to open: from the Dock, or any program */
        int k = 0;
        while (k < DOCK_APPS && strcmp(argv[i], apps[k].program)) k++;
        char *arg = strchr(argv[i], ':');                  /* "program:argument" */
        if (arg) *arg++ = 0;
        if (k < DOCK_APPS) launch(k);
        else run(argv[i], arg);
    }

    unsigned long last_tidy = 0, last_frame = uptime_ms();
    damage(0, 0, W, H);
    for (;;) {
        mutex_lock(&lock);
        check_settings();
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
                else if (!w->minimised && w->anim == ANIM_NONE) {
                    struct ws_surface *sf = w->s;      /* what the program says changed */
                    int x0 = sf->dirty_x0, y0 = sf->dirty_y0, x1 = sf->dirty_x1, y1 = sf->dirty_y1;
                    sf->dirty_x1 = sf->dirty_x0 = 0;
                    if (x1 <= x0 || y1 <= y0 || x0 < 0 || y0 < 0 || x1 > w->w || y1 > w->h)
                        damage(w->x, w->y, w->w, w->h);
                    else
                        damage(w->x + x0, w->y + y0, x1 - x0, y1 - y0);
                }
            }
        }
        animate_dock(dt);
        animate_windows(dt);
        if (uptime_ms() - last_tidy >= 500) {
            last_tidy = uptime_ms();
            tidy();
        }
        if (gpu_frames) gfx_sync();                  /* the GPU has read the frame before */
        int fresh = compose(), paced = 0, read = 0;
        for (int k = 0; k < MAX_WINDOWS; k++)          /* the windows this frame was made from */
            if (wins[k].used && wins[k].drawn != wins[k].seen) {
                wins[k].drawn = wins[k].seen;
                read = 1;
            }
        mutex_unlock(&lock);
        /* Present without the lock: on Intel graphics it waits for the screen's
         * refresh, and holding the lock meanwhile starved the thread that opens
         * windows (apps launched from the Dock never appeared). */
        if (fresh) {
            paced = gfx_present() > 0;                /* tear-free on Intel graphics */
            unpresented = 0;
            if (read && gpu_frames) gfx_sync();       /* (the GPU's copies of them are done) */
            static int surface_tries;                 /* the GPU takes frames from scr itself */
            if (surface_tries >= 0 && surface_tries++ < 20 && gfx_surface(scr.px, W, H, scr.stride) == 0) {
                printf("windowserver: the GPU copies frames (and window contents) from here on\n");
                surface_tries = -1;
                gpu_frames = 1;
            }
        }
        if (read) {                                   /* their programs may draw again (ui_present returns) */
            mutex_lock(&lock);
            for (int k = 0; k < MAX_WINDOWS; k++)
                if (wins[k].used) wins[k].s->released = wins[k].drawn;
            mutex_unlock(&lock);
        }
        if (!paced) sleep_ms(5);                      /* else the screen's refresh set the pace */
    }
}