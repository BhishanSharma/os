// windowserver [app]... - DOOR OS's desktop, the Lobby: owns the screen, the
// mouse and the keyboard, and draws the programs' windows.
//
// Programs ask for a window on the "windowserver" port (ws.h, ui.h) and draw
// into a shared memory surface; the server composites the surfaces back to
// front, only where something changed (a list of damaged rectangles).
//
// The look is DOOR's "Soft Minimal" (design/tokens.json): quiet navy-grey
// glass with real depth.
//   * the Rail on the left edge: Lobby, Apps, Files, Spaces, Create, Settings,
//     the DOOR mark on top and the account button at the bottom. Every item
//     has its own states (idle, hover, pressed, active) that change the
//     colour of its icon and label and fade in and out,
//   * the Status pill at the top right: search, notifications, Wi-Fi, sound,
//     battery, the date and time, and the light/dark switch,
//   * the Launcher, a glass panel next to the Rail with a search field and
//     the apps (the same panel shows Spaces, the open windows, and Create),
//   * windows with a quiet title bar and controls on the right (minimise,
//     maximise, close); they rise out of the glass: shadow, rim, light edge,
//   * glass: the backdrop sampled at 1/4 resolution, blurred, saturated and
//     tinted (surface1); Reduce Transparency makes it solid,
//   * icons are drawn by the server itself from a few lines and circles, in
//     any colour, so they follow the item's state (no pictures),
//   * motion (tokens: micro 120 ms, standard 200 ms, major 260 ms; ease-out
//     in, faster out, no bounce); Reduce Motion keeps only fades.
//
// Clicking an app starts it (or brings its window back). Ctrl+L opens the
// Launcher, Ctrl+W closes the front window, Ctrl+Q leaves.
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
#define KEY_LAUNCH 12                  /* Ctrl+L */

/* ---- Design tokens ---------------------------------------------------------
 * Sizes are in points; multiply by S (1, or 2 on a large screen) to draw.
 * Radii (door.h): micro 4, control 8, card 12, window 16, large 20. */
#define R_MICRO   DOOR_R_MICRO
#define R_CONTROL DOOR_R_CONTROL
#define R_CARD    DOOR_R_CARD
#define R_WINDOW  DOOR_R_WINDOW
#define R_LARGE   DOOR_R_LARGE

/* The colours of design/tokens.json for one appearance, plus what the
 * desktop derives from them (glass, rims, window edges). */
typedef struct {
    unsigned canvas, s1, s2, s3;                        /* canvas, surface1..3 */
    unsigned label, secondary, muted;                   /* textPrimary, textSecondary, textMuted */
    unsigned accent, on_accent, danger, success;
    unsigned glass; int glass_alpha;                    /* what tints the glass (surface1) */
    unsigned hair; int hair_alpha, hair_strong;         /* borderSubtle, borderStrong (alpha) */
    unsigned sheen; int sheen_alpha;                    /* the light edge at the top of a glass */
    unsigned title_on, title_off, title_text_on, title_text_off;
    unsigned edge_on, edge_off;                         /* a window's 1 px outline */
    int shadow_on, shadow_off, shadow_glass;            /* elevation E2 alphas (0-255) */
} theme_t;

static const theme_t themes[2] = {
    [0] = {                                             /* light */
        .canvas = 0xE9EDF2, .s1 = 0xF2F4F8, .s2 = 0xF8F9FB, .s3 = 0xDDE2EA,
        .label = 0x1A2030, .secondary = 0x465066, .muted = 0x5F6B80,
        .accent = 0x3B4E6E, .on_accent = 0xF8F9FB, .danger = 0xA83A3A, .success = 0x2F6F4E,
        .glass = 0xF2F4F8, .glass_alpha = 168,
        .hair = 0x1A2030, .hair_alpha = 31, .hair_strong = 77,
        .sheen = 0xFFFFFF, .sheen_alpha = 190,
        .title_on = 0xF8F9FB, .title_off = 0xEEF1F5, .title_text_on = 0x1A2030, .title_text_off = 0x5F6B80,
        .edge_on = 0xA9AEB9, .edge_off = 0xD2D6DE,
        .shadow_on = 80, .shadow_off = 42, .shadow_glass = 70,
    },
    [1] = {                                             /* dark */
        .canvas = 0x131820, .s1 = 0x1A202A, .s2 = 0x232A36, .s3 = 0x2D3543,
        .label = 0xE8ECF2, .secondary = 0xAAB3C2, .muted = 0x8590A0,
        .accent = 0xA9B8D0, .on_accent = 0x131820, .danger = 0xE07A7A, .success = 0x7FB89A,
        .glass = 0x1A202A, .glass_alpha = 150,
        .hair = 0xE8ECF2, .hair_alpha = 31, .hair_strong = 61,
        .sheen = 0xFFFFFF, .sheen_alpha = 40,
        .title_on = 0x232A36, .title_off = 0x1A202A, .title_text_on = 0xE8ECF2, .title_text_off = 0x8590A0,
        .edge_on = 0x525863, .edge_off = 0x3B414C,
        .shadow_on = 140, .shadow_off = 80, .shadow_glass = 100,
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

/* The theme for the current settings. Increase Contrast: rims at full
 * opacity, secondary text and the accent stronger. */
static unsigned theme_gen;                         /* bumped on each change: window pictures redraw */

static void apply_theme(void) {
    theme_gen++;
    cur = themes[dark];
    cur.accent = ui_accent_color(accent, dark, contrast);
    if (!contrast) return;
    cur.hair_alpha = 200;
    cur.hair_strong = 255;
    cur.secondary = dark ? 0xD3D9E3 : 0x2E3648;
    cur.muted = dark ? 0xB4BDCB : 0x465066;
    cur.edge_on = dark ? 0xAEB5C2 : 0x3A4358;
    cur.edge_off = dark ? 0x8590A0 : 0x5F6B80;
    cur.title_text_off = cur.muted;
    cur.glass_alpha = 255;
}

/* What apps see in their surface header (ws.h). */
static unsigned prefs_bits(void) {
    return (dark ? WS_PREF_DARK : 0) | (contrast ? WS_PREF_CONTRAST : 0) |
           (reduce_motion ? WS_PREF_MOTION : 0) | (reduce_transp ? WS_PREF_TRANSP : 0) | (unsigned)accent << 8;
}

static int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

/* t 0..256 */
static unsigned lerp_rgb(unsigned a, unsigned b, unsigned f) {
    if (f > 256) f = 256;
    unsigned r = (((a >> 16) & 255) * (256 - f) + ((b >> 16) & 255) * f) >> 8;
    unsigned gr = (((a >> 8) & 255) * (256 - f) + ((b >> 8) & 255) * f) >> 8;
    unsigned bl = ((a & 255) * (256 - f) + (b & 255) * f) >> 8;
    return r << 16 | gr << 8 | bl;
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
#define OPEN_MS   200                  /* opacity: ease-out (the spring runs on) */
#define CLOSE_MS  150                  /* ease-in: leaving is faster than arriving */
#define MIN_MS    240
#define FADE_MS   120                  /* micro: menus, hover */

typedef struct {
    int used, id, pid;
    int x, y, w, h;                    /* the content on the screen */
    int minimised, peek;               /* peek: the Lobby hid it */
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
    int lay_w, lay_h, lay_focus, lay_hover, lay_ok, lay_flags;
    unsigned lay_theme;
    char lay_title[48];
} window_t;

typedef struct {
    const char *name, *program, *glyph, *icon;
    unsigned color;
    int pid;
    int launching;
    unsigned long launch_at;
} dock_app_t;

/* The apps (the Launcher's) come from DOCK.CFG, which the build makes from
 * every apps/NAME/app.cfg (scripts/mkdock.sh): "order|name|program|icon|color". */
#define MAX_DOCK 24
static dock_app_t apps[MAX_DOCK];
static int n_apps;
#define DOCK_APPS n_apps

static void dock_add(char *name, char *program, char *icon, unsigned color) {
    static char glyphs[MAX_DOCK][2];
    if (n_apps == MAX_DOCK) return;
    dock_app_t *a = &apps[n_apps];
    glyphs[n_apps][0] = name[0];                       /* the letter, for an app without an icon */
    a->name = name;
    a->program = program;
    a->icon = icon;
    a->glyph = glyphs[n_apps];
    a->color = color;
    n_apps++;
}

static void load_installed(void);
static void damage_launcher(void);

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


/* tapp installed or removed an app (WS_APPS): the list again, the apps still
 * running keep their state. Under the lock. */
static void panel_items_changed(void);
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
                apps[i].pid = old[j].pid;
                apps[i].launching = old[j].launching;
                apps[i].launch_at = old[j].launch_at;
                break;
            }
    panel_items_changed();
}

enum { M_ABOUT = 1, M_SETTINGS, M_ACTIVITY, M_APPEARANCE, M_MOTION, M_TRANSP, M_TERMINAL,
       M_CONTRAST, M_RESTART, M_SHUTDOWN, M_LOGOUT };
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
static int W, H, S, TITLE_H, RADIUS, SHADOW;
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
static unsigned *wallpaper;                       /* W x H: the Lobby's picture */
static struct { int x0, y0, x1, y1; } dmg[MAX_DAMAGE];
static int ndmg;
static int mouse_x, mouse_y, buttons;
static int drag = -1, drag_dx, drag_dy, capture = -1;
static char clock_text[32];
static int unpresented;                          /* blitted, not yet shown (page flipping) */
static int tl_hover = -1, tl_btn = -1;           /* window whose controls the pointer is over, and which */
static int last_title_win = -1;
static unsigned long last_title_click;

static int menu_fade_ms = FADE_MS;               /* how long the open menu has been fading in */
static unsigned frame_no;                        /* compose calls so far */

/* Off-screen buffers (anim_cap pixels each): a window drawn on its own, its
 * shape, and what was behind a fading panel. Missing: no motion. */
static unsigned *anim_col, *anim_mask, *snap;
static long anim_cap;
static int cache_win = -1, cache_focus, cache_hover, cache_w, cache_h, cache_gpu;   /* what anim_col holds */
static unsigned cache_damage, cache_theme;

static mitem_t mitems[MI_MAX];
static int n_mitems, menu_open, menu_hot = -1, menu_wpx;   /* menu_open: 0 none, 1 account, 2 power, 3 notifications */
static int menu_ax, menu_ay, menu_flags;                   /* where it hangs from */
#define MA_UP    1                               /* the menu's bottom edge is at menu_ay */
#define MA_RIGHT 2                               /* the menu's right edge is at menu_ax */

/* The status pill's data: battery, Wi-Fi, sound. */
static int bat = -1, wifi_on = -1, vol_level = -1;

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

/* ---- Icons ---------------------------------------------------------------------
 * Drawn here, from a few strokes on a 24 x 24 grid (stroke 1.75, round caps),
 * so an icon can be any colour: the colour of its item's state. Each pixel's
 * coverage comes from its distance to the strokes (1/16 pixel, integers
 * only). Coordinates below are in half grid units (24 -> 48). */
enum { GL_NONE, GL_DOOR, GL_APPS, GL_FOLDER, GL_LAYERS, GL_COMPASS, GL_GEAR, GL_USER, GL_SEARCH, GL_BELL,
       GL_WIFI, GL_SOUND, GL_BATTERY, GL_MOON, GL_SUN, GL_POWER, GL_TERM, GL_DOC, GL_MUSIC, GL_GLOBE,
       GL_VIDEO, GL_PAINT, GL_PULSE, GL_INFO, GL_COMPUTER, GL_IMAGE, GL_MOUSE, GL_LOGO, GL_MIN, GL_MAX,
       GL_RESTORE, GL_CLOSE, GL_COUNT };
enum { P_LINE, P_RING, P_DISC, P_RRECT, P_FRECT, P_CUT };
typedef struct { unsigned char t; short a, b, c, d, e; } prim_t;
#define GL_MAXP 44
static struct { prim_t p[GL_MAXP]; int n; } glyphs[GL_COUNT];
static int gl_cur;

static void gp(int t, int a, int b, int c, int d, int e) {
    if (glyphs[gl_cur].n == GL_MAXP) return;
    prim_t *p = &glyphs[gl_cur].p[glyphs[gl_cur].n++];
    p->t = (unsigned char)t;
    p->a = (short)a; p->b = (short)b; p->c = (short)c; p->d = (short)d; p->e = (short)e;
}
static void g_line(int x0, int y0, int x1, int y1) { gp(P_LINE, x0, y0, x1, y1, 0); }
static void g_ring(int cx, int cy, int r) { gp(P_RING, cx, cy, r, 0, 0); }
static void g_disc(int cx, int cy, int r) { gp(P_DISC, cx, cy, r, 0, 0); }
static void g_cut(int cx, int cy, int r) { gp(P_CUT, cx, cy, r, 0, 0); }
static void g_box(int cx, int cy, int hw, int hh, int r) { gp(P_RRECT, cx, cy, hw, hh, r); }
static void g_poly(const signed char *xy, int n, int closed) {
    for (int i = 0; i + 1 < n; i++) g_line(xy[2 * i], xy[2 * i + 1], xy[2 * i + 2], xy[2 * i + 3]);
    if (closed) g_line(xy[2 * (n - 1)], xy[2 * (n - 1) + 1], xy[0], xy[1]);
}

/* cos of k * 15 degrees, 1/1024; angles run clockwise on the screen */
static const short cos15[24] = { 1024, 989, 887, 724, 512, 265, 0, -265, -512, -724, -887, -989,
                                 -1024, -989, -887, -724, -512, -265, 0, 265, 512, 724, 887, 989 };
static int c15(int k) { return cos15[((k % 24) + 24) % 24]; }
static int s15(int k) { return c15(k - 6); }
static void g_arc(int cx, int cy, int rx, int ry, int k0, int k1) {
    for (int k = k0; k < k1; k++)
        g_line(cx + rx * c15(k) / 1024, cy + ry * s15(k) / 1024, cx + rx * c15(k + 1) / 1024, cy + ry * s15(k + 1) / 1024);
}
static void g_ray(int cx, int cy, int r0, int r1, int k) {
    g_line(cx + r0 * c15(k) / 1024, cy + r0 * s15(k) / 1024, cx + r1 * c15(k) / 1024, cy + r1 * s15(k) / 1024);
}

#define POLY(a) a, (int)(sizeof(a) / 2)
static void glyphs_init(void) {
    static const signed char tab[] = { 8, 16, 8, 12, 16, 12, 19, 16 };
    static const signed char layer_a[] = { 24, 6, 42, 16, 24, 26, 6, 16 };
    static const signed char layer_b[] = { 6, 25, 24, 35, 42, 25 };
    static const signed char layer_c[] = { 6, 33, 24, 43, 42, 33 };
    static const signed char needle[] = { 31, 17, 27, 27, 17, 31, 21, 21 };
    static const signed char hex[] = { 24, 5, 40, 14, 40, 34, 24, 43, 8, 34, 8, 14 };
    static const signed char bell[] = { 9, 35, 12, 32, 12, 22, 15, 15, 20, 11, 24, 10, 28, 11, 33, 15, 36, 22, 36, 32, 39, 35 };
    static const signed char spk[] = { 7, 19, 14, 19, 22, 12, 22, 36, 14, 29, 7, 29 };
    static const signed char chev[] = { 14, 18, 21, 24, 14, 30 };
    static const signed char beam[] = { 20, 13, 38, 9 };
    static const signed char play[] = { 33, 20, 42, 15, 42, 33, 33, 28 };
    static const signed char pulse[] = { 5, 24, 14, 24, 19, 11, 28, 37, 33, 24, 43, 24 };
    static const signed char hill[] = { 8, 34, 17, 25, 24, 32, 30, 27, 40, 35 };
    static const signed char back[] = { 17, 17, 17, 9, 39, 9, 39, 31, 31, 31 };

    gl_cur = GL_DOOR;     g_box(24, 22, 12, 17, 3); g_disc(30, 23, 2); g_line(6, 43, 42, 43);   /* a door with its knob, on the floor */
    gl_cur = GL_APPS;     g_box(15, 15, 6, 6, 3); g_box(33, 15, 6, 6, 3); g_box(15, 33, 6, 6, 3); g_box(33, 33, 6, 6, 3);
    gl_cur = GL_FOLDER;   g_box(24, 28, 18, 12, 5); g_poly(POLY(tab), 0);
    gl_cur = GL_LAYERS;   g_poly(POLY(layer_a), 1); g_poly(POLY(layer_b), 0); g_poly(POLY(layer_c), 0);
    gl_cur = GL_COMPASS;  g_ring(24, 24, 18); g_poly(POLY(needle), 1);
    gl_cur = GL_GEAR;     g_poly(POLY(hex), 1); g_ring(24, 24, 6);
    gl_cur = GL_USER;     g_ring(24, 17, 7); g_arc(24, 41, 14, 14, 12, 24);
    gl_cur = GL_SEARCH;   g_ring(21, 21, 13); g_line(31, 31, 41, 41);
    gl_cur = GL_BELL;     g_poly(POLY(bell), 0); g_line(9, 35, 39, 35); g_arc(24, 37, 4, 4, 0, 12); g_disc(24, 8, 3);
    gl_cur = GL_WIFI;     g_arc(24, 39, 9, 9, 15, 21); g_arc(24, 39, 18, 18, 15, 21); g_arc(24, 39, 27, 27, 15, 21); g_disc(24, 39, 2);
    gl_cur = GL_SOUND;    g_poly(POLY(spk), 1); g_arc(25, 24, 9, 9, 21, 27); g_arc(25, 24, 17, 17, 21, 27);
    gl_cur = GL_BATTERY;  g_box(22, 24, 18, 10, 5); g_line(42, 20, 42, 28);
    gl_cur = GL_MOON;     g_disc(24, 24, 18); g_cut(33, 16, 15);
    gl_cur = GL_SUN;      g_ring(24, 24, 7);
    for (int k = 0; k < 24; k += 3) g_ray(24, 24, 12, 18, k);
    gl_cur = GL_POWER;    g_arc(24, 26, 16, 16, 21, 39); g_line(24, 6, 24, 22);
    gl_cur = GL_TERM;     g_box(24, 24, 19, 16, 5); g_poly(POLY(chev), 0); g_line(25, 31, 33, 31);
    gl_cur = GL_DOC;      g_box(24, 24, 14, 18, 5); g_line(17, 18, 31, 18); g_line(17, 25, 31, 25); g_line(17, 32, 25, 32);
    gl_cur = GL_MUSIC;    g_disc(15, 36, 5); g_disc(33, 32, 5); g_line(20, 36, 20, 13); g_line(38, 32, 38, 9); g_poly(POLY(beam), 0);
    gl_cur = GL_GLOBE;    g_ring(24, 24, 18); g_line(6, 24, 42, 24); g_arc(24, 24, 7, 18, 0, 24);
    gl_cur = GL_VIDEO;    g_box(21, 24, 16, 12, 5); g_poly(POLY(play), 1);
    gl_cur = GL_PAINT;    g_ring(24, 24, 18); g_disc(16, 20, 2); g_disc(24, 14, 2); g_disc(32, 20, 2); g_disc(17, 31, 2);
    gl_cur = GL_PULSE;    g_poly(POLY(pulse), 0);
    gl_cur = GL_INFO;     g_ring(24, 24, 18); g_line(24, 22, 24, 34); g_disc(24, 15, 2);
    gl_cur = GL_COMPUTER; g_box(24, 21, 18, 12, 4); g_line(18, 40, 30, 40); g_line(24, 33, 24, 40);
    gl_cur = GL_IMAGE;    g_box(24, 24, 18, 15, 5); g_disc(32, 18, 3); g_poly(POLY(hill), 0);
    gl_cur = GL_MOUSE;    g_box(24, 24, 10, 17, 10); g_line(24, 7, 24, 19);
    gl_cur = GL_LOGO;     g_arc(24, 24, 18, 18, 22, 44); g_disc(37, 11, 3);                   /* the DOOR mark */
    gl_cur = GL_MIN;      g_line(10, 24, 38, 24);
    gl_cur = GL_MAX;      g_box(24, 24, 13, 13, 4);
    gl_cur = GL_RESTORE;  g_box(20, 28, 11, 11, 3); g_poly(POLY(back), 0);
    gl_cur = GL_CLOSE;    g_line(12, 12, 36, 36); g_line(36, 12, 12, 36);
}

static int isq(long n) {
    if (n <= 0) return 0;
    long x = n < 4 ? n : (1L << ((64 - __builtin_clzl((unsigned long)n) + 1) / 2));
    for (;;) {
        long y = (x + n / x) / 2;
        if (y >= x) return (int)x;
        x = y;
    }
}

/* Coverage 0..16 of one stroke or shape at (px, py), all in 1/16 pixel. */
static int prim_cover(const prim_t *q, int px, int py, int hw) {
    long dx = px - q->a, dy = py - q->b;
    int d;
    switch (q->t) {
    case P_LINE: {
        long vx = q->c - q->a, vy = q->d - q->b, l2 = vx * vx + vy * vy, t = dx * vx + dy * vy;
        if (t < 0) t = 0;
        else if (t > l2) t = l2;
        long ex = l2 ? dx - vx * t / l2 : dx, ey = l2 ? dy - vy * t / l2 : dy;
        d = isq(ex * ex + ey * ey);
        return clampi(hw - d + 8, 0, 16);
    }
    case P_RING:
        d = isq(dx * dx + dy * dy) - q->c;
        if (d < 0) d = -d;
        return clampi(hw - d + 8, 0, 16);
    case P_DISC:
    case P_CUT:
        d = isq(dx * dx + dy * dy) - q->c;
        return clampi(8 - d, 0, 16);
    default: {                                          /* P_RRECT outline, P_FRECT filled */
        long ax = dx < 0 ? -dx : dx, ay = dy < 0 ? -dy : dy;
        long qx = ax - (q->c - q->e), qy = ay - (q->d - q->e);
        int out = isq((qx > 0 ? qx * qx : 0) + (qy > 0 ? qy * qy : 0));
        int in = (int)(qx > qy ? qx : qy);
        if (in > 0) in = 0;
        int sd = out + in - q->e;
        if (q->t == P_FRECT) return clampi(8 - sd, 0, 16);
        if (sd < 0) sd = -sd;
        return clampi(hw - sd + 8, 0, 16);
    }
    }
}

/* An icon, `size` pixels square (24 * S is the design size), in `color`.
 * `level` (GL_BATTERY): the charge in percent, drawn inside. */
static void gl_draw(ui_canvas *cv, int x, int y, int size, int id, unsigned color, int alpha, int level) {
    if (id <= 0 || id >= GL_COUNT || alpha <= 0 || size <= 0) return;
    if (x >= cv->cx1 || y >= cv->cy1 || x + size <= cv->cx0 || y + size <= cv->cy0) return;
    prim_t q[GL_MAXP + 1];
    int n = glyphs[id].n;
    for (int i = 0; i < n; i++) {                       /* half grid units -> 1/16 pixel */
        const prim_t *s = &glyphs[id].p[i];
        q[i].t = s->t;
        q[i].a = (short)(s->a * size / 3); q[i].b = (short)(s->b * size / 3);
        q[i].c = (short)(s->c * size / 3); q[i].d = (short)(s->d * size / 3);
        q[i].e = (short)(s->e * size / 3);
    }
    if (id == GL_BATTERY && level >= 0) {
        int w = 28 * clampi(level, 4, 100) / 100;       /* inner width in half units */
        q[n].t = P_FRECT;
        q[n].a = (short)((8 + w / 2) * size / 3);
        q[n].b = (short)(24 * size / 3);
        q[n].c = (short)((w / 2 > 1 ? w / 2 : 1) * size / 3);
        q[n].d = (short)(6 * size / 3);
        q[n].e = (short)(2 * size / 3);
        n++;
    }
    int hw = size * 7 / 12;                             /* half the stroke: 0.875 px at 24 */
    if (hw < 6) hw = 6;
    int has_cut = 0;
    for (int i = 0; i < n; i++) if (q[i].t == P_CUT) has_cut = 1;
    int y0 = y < cv->cy0 ? cv->cy0 - y : 0, y1 = y + size > cv->cy1 ? cv->cy1 - y : size;
    int x0 = x < cv->cx0 ? cv->cx0 - x : 0, x1 = x + size > cv->cx1 ? cv->cx1 - x : size;
    for (int iy = y0; iy < y1; iy++)
        for (int ix = x0; ix < x1; ix++) {
            int px = ix * 16 + 8, py = iy * 16 + 8, cov = 0, cut = 0;
            for (int i = 0; i < n && (cov < 16 || has_cut); i++) {
                int c = prim_cover(&q[i], px, py, hw);
                if (q[i].t == P_CUT) { if (c > cut) cut = c; }
                else if (c > cov) cov = c;
            }
            if (cut) cov = cov * (16 - cut) / 16;
            if (cov) ui_blend_pixel(cv, x + ix, y + iy, color, cov * alpha / 16);
        }
}

/* The glyph for an app.cfg icon name; 0: none (a picture or a letter instead). */
static int glyph_for(const char *icon) {
    static const struct { const char *name; int gl; } map[] = {
        { "folder", GL_FOLDER }, { "gear", GL_GEAR }, { "terminal", GL_TERM }, { "textedit", GL_DOC },
        { "file", GL_DOC }, { "paint", GL_PAINT }, { "activity", GL_PULSE }, { "music", GL_MUSIC },
        { "sound", GL_SOUND }, { "video", GL_VIDEO }, { "info", GL_INFO }, { "computer", GL_COMPUTER },
        { "picture", GL_IMAGE }, { "browser", GL_GLOBE }, { "user", GL_USER }, { "mouse", GL_MOUSE },
        { "wifi", GL_WIFI }, { "battery", GL_BATTERY },
    };
    if (!icon) return 0;
    for (unsigned i = 0; i < sizeof(map) / sizeof(map[0]); i++)
        if (!strcmp(icon, map[i].name)) return map[i].gl;
    return 0;
}

/* ---- The shell's layout and state ---------------------------------------------
 * The Rail (left), the Status pill (top right), the Launcher panel, menus. */

static void launch(int i);
static void run(const char *program, const char *arg);
static void restore_window(int k);
static void minimise_window(int k);
static void raise(int k);
static void quit(void);

static int rail_w(void) { return 80 * S; }
static int win_left(void) { return rail_w() + 12 * S; }          /* least x of a window's content */
static int win_top(void) { return TITLE_H + 12 * S; }            /* least y of a window's content */

static window_t *front(void) {
    for (int i = norder - 1; i >= 0; i--)
        if (live(&wins[order[i]])) return &wins[order[i]];
    return 0;
}

enum { R_LOBBY, R_APPS, R_FILES, R_SPACES, R_CREATE, R_SETTINGS, R_COUNT };
static const char *const rail_label[R_COUNT] = { "Lobby", "Apps", "Files", "Spaces", "Create", "Settings" };
static const int rail_glyph[R_COUNT] = { GL_DOOR, GL_APPS, GL_FOLDER, GL_LAYERS, GL_COMPASS, GL_GEAR };

/* Every item the pointer can be over: its id indexes the state arrays. */
enum { H_RAIL0 = 0, H_AVATAR = 6, H_LOGO, H_SEARCH, H_BELL, H_WIFI, H_SOUND, H_BATTERY, H_THEME, H_POWER,
       H_FIELD, H_TILE0 = 16, MAX_TILES = 32, H_COUNT = H_TILE0 + MAX_TILES };
static int hov[H_COUNT], act[H_COUNT];             /* hover and active, 0..256: they fade */
static int press_id = -1, hot = -1;                /* the item held down; the item under the pointer */

static int rail_item_y(int i) { return 92 * S + i * 72 * S; }
static int rail_item_x(void) { return 8 * S; }
static int rail_item_w(void) { return rail_w() - 16 * S; }
static int rail_item_h(void) { return 64 * S; }
static int avatar_cx(void) { return rail_w() / 2; }
static int avatar_cy(void) { return H - 40 * S; }
static int logo_cy(void) { return 44 * S; }

/* The Status pill: buttons left to right, the time between them. */
static struct { int id, x, w; } pb[8];
static int pb_n, pill_x, pill_y, pill_w, pill_h, time_x, time_w;
static char bat_text[8];

static void pill_layout(void) {
    int bw = 40 * S;
    pill_h = 48 * S;
    pill_y = 12 * S;
    pb_n = 0;
    time_w = ui_text_width(clock_text, S) + 20 * S;
    int bat_w = 0;
    if (bat >= 0) {
        snprintf(bat_text, sizeof(bat_text), "%d%%", bat & 0xFF);
        bat_w = 12 * S + 20 * S + 6 * S + ui_text_width(bat_text, S) + 12 * S;
        if (bat_w < bw) bat_w = bw;
    }
    int total = 16 * S + time_w + bw * 3 + (wifi_on >= 0 ? bw : 0) + (vol_level >= 0 ? bw : 0) + bat_w;
    pill_w = total;
    pill_x = W - 16 * S - total;
    int x = pill_x + 8 * S;
#define PB(i, wd) do { pb[pb_n].id = (i); pb[pb_n].x = x; pb[pb_n].w = (wd); pb_n++; x += (wd); } while (0)
    PB(H_SEARCH, bw);
    PB(H_BELL, bw);
    if (wifi_on >= 0) PB(H_WIFI, bw);
    if (vol_level >= 0) PB(H_SOUND, bw);
    if (bat >= 0) PB(H_BATTERY, bat_w);
    time_x = x;
    x += time_w;
    PB(H_THEME, bw);
#undef PB
}

/* The Launcher panel: Apps (with a search field), Spaces (the open windows)
 * and Create share it. */
enum { PM_APPS, PM_SPACES, PM_CREATE };
enum { LK_APP, LK_WIN, LK_PANE };
typedef struct { char label[48]; int kind, idx; } litem_t;
static litem_t items[MAX_TILES];
static int n_items, items_dirty;
static int panel_open, panel_mode, panel_scroll, panel_sel = -1;
static int lt;                                     /* how open it is: 0..1024 (it animates) */
static char query[32];
static int qlen, caret_on;

typedef struct { int x, y, w, h, gx, gy, cw, ch, rows; } panel_geo_t;

static void panel_geo(panel_geo_t *p) {
    p->cw = 112 * S;
    p->ch = 104 * S;
    p->rows = H / S >= 700 ? 3 : 2;
    p->w = 4 * p->cw + 40 * S;
    p->gx = 20 * S;
    p->gy = (20 + 44 + 12) * S;
    p->h = p->gy + p->rows * p->ch + 12 * S + 60 * S;
    p->x = rail_w() + 24 * S;
    p->y = H - 24 * S - p->h;
    if (p->y < 12 * S) p->y = 12 * S;
}

static int panel_vis(void) { return lt > 0; }
static int panel_off(void) { return reduce_motion ? 0 : (1024 - lt) * 16 * S / 1024; }   /* it rises 16 pt as it opens */

static void damage_launcher(void) {
    panel_geo_t p;
    panel_geo(&p);
    damage(p.x - SHADOW, p.y - SHADOW, p.w + 2 * SHADOW, p.h + 2 * SHADOW + 20 * S);
}

static int has_ci(const char *hay, const char *needle) {                  /* case-blind substring */
    for (; *hay; hay++) {
        const char *a = hay, *b = needle;
        while (*a && *b && ((*a | 32) == (*b | 32))) { a++; b++; }
        if (!*b) return 1;
    }
    return 0;
}

static const char *const create_progs[] = { "paint", "textedit", "music" };
static const struct { const char *label, *pane; int glyph; } panes[] = {
    { "Wi-Fi", "wifi", GL_WIFI }, { "Sound", "sound", GL_SOUND }, { "Battery", "battery", GL_BATTERY },
};

static void add_litem(int kind, int idx, const char *label) {
    if (n_items == MAX_TILES) return;
    litem_t *it = &items[n_items++];
    it->kind = kind;
    it->idx = idx;
    snprintf(it->label, sizeof(it->label), "%s", label);
}

static void build_items(void) {
    n_items = 0;
    if (panel_mode == PM_APPS) {
        for (int i = 0; i < DOCK_APPS; i++)
            if (!qlen || has_ci(apps[i].name, query)) add_litem(LK_APP, i, apps[i].name);
        if (qlen)
            for (unsigned i = 0; i < sizeof(panes) / sizeof(panes[0]); i++)
                if (has_ci(panes[i].label, query)) add_litem(LK_PANE, (int)i, panes[i].label);
    } else if (panel_mode == PM_CREATE) {
        for (int i = 0; i < DOCK_APPS; i++)
            for (unsigned k = 0; k < sizeof(create_progs) / sizeof(create_progs[0]); k++)
                if (!strcmp(apps[i].program, create_progs[k])) add_litem(LK_APP, i, apps[i].name);
    } else {
        for (int i = norder - 1; i >= 0; i--) {                              /* the front window first */
            window_t *w = &wins[order[i]];
            if (w->used && w->anim != ANIM_CLOSE && w->anim != ANIM_WAIT) add_litem(LK_WIN, order[i], w->title);
        }
    }
    if (panel_sel >= n_items) panel_sel = n_items - 1;
    panel_geo_t p;
    panel_geo(&p);
    int total = (n_items + 3) / 4, most = total > p.rows ? total - p.rows : 0;
    if (panel_scroll > most) panel_scroll = most;
    if (panel_scroll < 0) panel_scroll = 0;
}

static void panel_items_changed(void) {
    if (!panel_open) return;
    build_items();
    damage_launcher();
}

static void damage_rail(void) { damage(0, 0, rail_w() + SHADOW, H); }
static void damage_pill(void) {                    /* (wide enough for any width the time and icons make) */
    int w = W < 780 * S ? W : 780 * S;
    damage(W - w, 0, w, 12 * S + 48 * S + SHADOW);
}
/* The rail and the pill again: focus, running apps and titles changed. */
static void damage_chrome(void) {
    damage_rail();
    damage_pill();
    if (panel_open && panel_mode == PM_SPACES) items_dirty = 1;
}

static void close_menu(void);
static void panel_show(int mode) {
    close_menu();
    if (!panel_open || panel_mode != mode) {
        panel_mode = mode;
        qlen = 0;
        query[0] = 0;
        panel_scroll = 0;
        panel_sel = -1;
        build_items();
    }
    panel_open = 1;
    damage_launcher();
}
static void panel_hide(void) {
    if (!panel_open) return;
    panel_open = 0;
    damage_launcher();
}
static void panel_toggle(int mode) {
    if (panel_open && panel_mode == mode) panel_hide();
    else panel_show(mode);
}

/* Keep the selected tile in view. */
static void panel_reveal(void) {
    panel_geo_t p;
    panel_geo(&p);
    if (panel_sel < 0) return;
    int row = panel_sel / 4;
    if (row < panel_scroll) panel_scroll = row;
    if (row >= panel_scroll + p.rows) panel_scroll = row - p.rows + 1;
}

static int tile_rect(int item, const panel_geo_t *p, int *x, int *y) {     /* the tile's cell, if shown */
    int vi = item - panel_scroll * 4;
    if (vi < 0 || vi >= p->rows * 4 || item >= n_items) return 0;
    *x = p->x + p->gx + (vi % 4) * p->cw;
    *y = p->y + p->gy + (vi / 4) * p->ch;
    return 1;
}
static void power_rect(const panel_geo_t *p, int *x, int *y, int *w, int *h) {
    *x = p->x + 20 * S;
    *y = p->y + p->h - 50 * S;
    *w = *h = 40 * S;
}
static void field_rect(const panel_geo_t *p, int *x, int *y, int *w, int *h) {
    *x = p->x + 20 * S;
    *y = p->y + 20 * S;
    *w = p->w - 40 * S;
    *h = 44 * S;
}

static int app_index(const char *program) {
    for (int i = 0; i < DOCK_APPS; i++)
        if (!strcmp(apps[i].program, program)) return i;
    return -1;
}
static int front_is(const char *program) {
    window_t *f = front();
    if (!f) return 0;
    for (int i = 0; i < DOCK_APPS; i++)
        if (apps[i].pid > 0 && apps[i].pid == f->pid) return !strcmp(apps[i].program, program);
    return 0;
}
static int app_has_window_by_pid(int pid) {
    if (pid <= 0) return 0;
    for (int k = 0; k < MAX_WINDOWS; k++)
        if (wins[k].used && wins[k].pid == pid) return 1;
    return 0;
}
static int app_running(const char *program) {
    int i = app_index(program);
    return i >= 0 && apps[i].pid > 0;
}

/* Is this item "on"? (the current place, an open panel or menu, a running app) */
static int ui_active(int id) {
    window_t *f = front();
    switch (id) {
    case H_RAIL0 + R_LOBBY: return !f && !panel_open;
    case H_RAIL0 + R_APPS: return panel_open && panel_mode == PM_APPS;
    case H_RAIL0 + R_FILES: return !panel_open && front_is("finder");
    case H_RAIL0 + R_SPACES: return panel_open && panel_mode == PM_SPACES;
    case H_RAIL0 + R_CREATE: return panel_open && panel_mode == PM_CREATE;
    case H_RAIL0 + R_SETTINGS: return !panel_open && front_is("settings");
    case H_AVATAR: return menu_open == 1;
    case H_BELL: return menu_open == 3;
    case H_POWER: return menu_open == 2;
    default:
        if (id >= H_TILE0 && id - H_TILE0 < n_items) {
            litem_t *it = &items[id - H_TILE0];
            if (it->kind == LK_APP) return apps[it->idx].pid > 0;
            if (it->kind == LK_WIN) return f == &wins[it->idx];
        }
    }
    return 0;
}

/* The item at a point: its id; -2 inside the Launcher but on nothing; -1 none. */
static int ui_hit(int x, int y) {
    if (menu_open) return -1;
    if (panel_vis()) {
        panel_geo_t p;
        panel_geo(&p);
        if (ui_inside(x, y, p.x, p.y, p.w, p.h)) {
            int bx, by, bw, bh;
            power_rect(&p, &bx, &by, &bw, &bh);
            if (ui_inside(x, y, bx, by, bw, bh)) return H_POWER;
            if (panel_mode == PM_APPS) {
                field_rect(&p, &bx, &by, &bw, &bh);
                if (ui_inside(x, y, bx, by, bw, bh)) return H_FIELD;
            }
            for (int i = 0; i < n_items; i++)
                if (tile_rect(i, &p, &bx, &by) && ui_inside(x, y, bx, by, p.cw, p.ch)) return H_TILE0 + i;
            return -2;
        }
    }
    pill_layout();
    if (y >= pill_y && y < pill_y + pill_h)
        for (int i = 0; i < pb_n; i++)
            if (x >= pb[i].x && x < pb[i].x + pb[i].w) return pb[i].id;
    if (x < rail_w()) {
        int dx = x - avatar_cx(), dy = y - avatar_cy();
        if (dx * dx + dy * dy <= 22 * S * 22 * S) return H_AVATAR;
        if (ui_inside(x, y, avatar_cx() - 24 * S, logo_cy() - 24 * S, 48 * S, 48 * S)) return H_LOGO;
        for (int i = 0; i < R_COUNT; i++)
            if (ui_inside(x, y, rail_item_x(), rail_item_y(i), rail_item_w(), rail_item_h())) return H_RAIL0 + i;
    }
    return -1;
}

/* How an item looks now: the icon and label move from secondary through
 * primary (hover) to the accent (active, pressed); the plate behind fades in. */
static unsigned ui_fg(int id) {
    unsigned c = lerp_rgb(TH.secondary, TH.label, (unsigned)hov[id]);
    c = lerp_rgb(c, TH.accent, (unsigned)act[id]);
    if (press_id == id) c = TH.accent;
    return c;
}
static int ui_plate(int id) {                      /* alpha of the plate behind it, 0..255 */
    int a = hov[id] * 70 / 256 + act[id] * 150 / 256 + (press_id == id ? 70 : 0);
    return a > 255 ? 255 : a;
}

/* Moves every fade on by dt ms: hover, active, the Launcher opening, the caret.
 * -> 1 while something moves. */
static int animate_ui(int dt) {
    int moving = 0, rail = 0, pill = 0, panel = 0;
    int want = panel_open ? 1024 : 0;
    if (lt != want) {
        int ms = reduce_motion ? FADE_MS : panel_open ? 260 : 195;      /* major; leaving is faster */
        int step = dt * 1024 / ms;
        if (step < 1) step = 1;
        lt = want > lt ? (lt + step > want ? want : lt + step) : (lt - step < want ? want : lt - step);
        panel = 1;
        moving = 1;
    }
    hot = ui_hit(mouse_x, mouse_y);
    if (hot < 0) hot = -1;
    int fade = dt * 256 / FADE_MS;
    if (fade < 1) fade = 1;
    for (int id = 0; id < H_COUNT; id++) {
        int th = id == hot ? 256 : 0, ta = ui_active(id) ? 256 : 0, ch = 0;
        if (hov[id] != th) { hov[id] = hov[id] < th ? (hov[id] + fade > th ? th : hov[id] + fade) : (hov[id] - fade < th ? th : hov[id] - fade); ch = 1; }
        if (act[id] != ta) { act[id] = act[id] < ta ? (act[id] + fade > ta ? ta : act[id] + fade) : (act[id] - fade < ta ? ta : act[id] - fade); ch = 1; }
        if (!ch) continue;
        moving = 1;
        if (id < H_SEARCH) rail = 1;
        else if (id < H_POWER) pill = 1;
        else panel = 1;
    }
    unsigned long now = uptime_ms();
    for (int i = 0; i < DOCK_APPS; i++)                                   /* a starting app pulses */
        if (apps[i].launching) {
            if (now - apps[i].launch_at > 8000 || app_has_window_by_pid(apps[i].pid)) apps[i].launching = 0;
            rail = 1;
            if (panel_vis()) panel = 1;
            moving = 1;
        }
    if (panel_vis() && panel_mode == PM_APPS) {                           /* the caret blinks */
        int on = (int)(now / 530 & 1);
        if (on != caret_on) {
            caret_on = on;
            panel_geo_t p;
            int x, y, w, h;
            panel_geo(&p);
            field_rect(&p, &x, &y, &w, &h);
            damage(x, y, w, h);
        }
    }
    if (rail) damage_rail();
    if (pill) damage_pill();
    if (panel) damage_launcher();
    return moving;
}

/* ---- Menus (account, power, notifications): a glass list hung from a button ---- */

static int mi_h(int i) { return (mitems[i].flags & MI_SEP) ? 8 * S : 32 * S; }
static int menu_w(void) { return menu_wpx; }
static int menu_h(void) {
    int h = 8 * S;
    for (int i = 0; i < n_mitems; i++) h += mi_h(i);
    return h;
}
static int menu_x(void) {
    int x = (menu_flags & MA_RIGHT) ? menu_ax - menu_w() : menu_ax;
    return clampi(x, 8 * S, W - menu_w() - 8 * S);
}
static int menu_y(void) {
    int y = (menu_flags & MA_UP) ? menu_ay - menu_h() : menu_ay;
    return clampi(y, 8 * S, H - menu_h() - 8 * S);
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

static void build_menu(int kind) {
    char text[44];
    n_mitems = 0;
    if (kind == 1) {
        add_item("About DOOR OS", 0, M_ABOUT, 0);
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
    } else if (kind == 2) {
        add_item("Restart...", 0, M_RESTART, 0);
        add_item("Shut Down...", 0, M_SHUTDOWN, 0);
        add_sep();
        add_item("Log Out", "Ctrl+Q", M_LOGOUT, 0);
    } else {
        add_item("Notifications", 0, 0, MI_OFF);
        add_sep();
        add_item("You're all caught up", 0, 0, MI_OFF);
    }
    int maxl = 0, maxk = 0;
    for (int i = 0; i < n_mitems; i++) {
        if (mitems[i].flags & MI_SEP) continue;
        int lw = ui_text_width(mitems[i].label, S), kw = ui_text_width(mitems[i].key, S);
        if (lw > maxl) maxl = lw;
        if (kw > maxk) maxk = kw;
    }
    menu_wpx = 16 * S + maxl + (maxk ? 24 * S + maxk : 0) + 16 * S;
    if (menu_wpx < 200 * S) menu_wpx = 200 * S;
}

static void damage_menu(void) {
    damage(menu_x() - SHADOW, menu_y() - SHADOW, menu_w() + 2 * SHADOW, menu_h() + 2 * SHADOW + 8 * S);
    damage_chrome();
}

static void open_menu(int kind, int ax, int ay, int flags) {
    if (menu_open) close_menu();
    build_menu(kind);
    menu_open = kind;
    menu_ax = ax;
    menu_ay = ay;
    menu_flags = flags;
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

static int menu_hit(int x, int y) {
    if (!menu_open || !ui_inside(x, y, menu_x(), menu_y(), menu_w(), menu_h())) return -1;
    for (int i = 0; i < n_mitems; i++) {
        int iy = menu_item_y(i);
        if (y >= iy && y < iy + mi_h(i)) return (mitems[i].flags & MI_SEP) ? -1 : i;
    }
    return -1;
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
    damage_chrome();                             /* the app's name */
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

/* Where a minimising window goes: its app's place on the Rail (Files, Settings), else Spaces. */
static void dock_target(const window_t *w, int *tx, int *ty) {
    int item = R_SPACES;
    for (int i = 0; i < DOCK_APPS; i++)
        if (apps[i].pid > 0 && apps[i].pid == w->pid) {
            if (!strcmp(apps[i].program, "finder")) item = R_FILES;
            else if (!strcmp(apps[i].program, "settings")) item = R_SETTINGS;
            break;
        }
    *tx = rail_item_x() + rail_item_w() / 2;
    *ty = rail_item_y(item) + rail_item_h() / 2;
}

/* The screen rectangle a window's animation can reach (with its shadow). */
static void damage_anim(window_t *w) {
    int x, y, ww, hh;
    outer(w, &x, &y, &ww, &hh);
    int x0 = x - SHADOW, y0 = y - SHADOW, x1 = x + ww + SHADOW, y1 = y + hh + SHADOW + SHADOW / 3;
    if (w->anim == ANIM_MIN || w->anim == ANIM_RESTORE) {        /* and the way to the Dock */
        int tx, ty;
        dock_target(w, &tx, &ty);
        if (tx - (80 * S) < x0) x0 = tx - (80 * S);
        if (tx + (80 * S) > x1) x1 = tx + (80 * S);
        if (ty - (80 * S) < y0) y0 = ty - (80 * S);
        if (ty + (80 * S) > y1) y1 = ty + (80 * S);
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
    damage_chrome();
}

/* A window ends: it fades and shrinks a little (ease-in, 150 ms), then goes. */
static void remove_window(int k) {
    window_t *w = &wins[k];
    if (w->anim == ANIM_CLOSE) return;
    if (menu_open) close_menu();
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
    damage_chrome();
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
    damage_chrome();
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
    w->sc.pos = 96L * FP;
    w->sc.vel = 0;
    damage_anim(w);
}

static int create_window(const struct ws_request *q, struct ws_reply *r) {
    int max_w = W - win_left() - 12 * S, max_h = H - win_top() - 12 * S;
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
    int step = 24 * S;                             /* cascade from the top left of the free area */
    w->x = win_left() + 24 * S + (placed % 8) * step;
    w->y = win_top() + 12 * S + (placed % 8) * step;
    placed++;
    if (w->x + w->w > W - 12 * S) w->x = W - 12 * S - w->w;
    if (w->y + w->h > H - 12 * S) w->y = H - 12 * S - w->h;
    if (w->x < win_left()) w->x = win_left();
    if (w->y < win_top()) w->y = win_top();
    order[norder++] = k;
    if (menu_open) close_menu();
    raise(k);
    r->id = w->id;
    r->x = w->x;
    r->y = w->y;
    r->w = w->w;
    r->h = w->h;
    return 0;
}

/* The maximise control: fill the free area (right of the Rail), or go
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
        nx = win_left();                               /* the free area: right of the Rail, a margin round it */
        ny = win_top();
        nw = W - nx - 12 * S;
        nh = H - ny - 12 * S;
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
    int max_w = W - win_left() - 12 * S, max_h = H - win_top() - 12 * S;
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
        if (w->x + w->w > W - 12 * S) w->x = W - 12 * S - w->w;
        if (w->y + w->h > H - 12 * S) w->y = H - 12 * S - w->h;
        if (w->x < win_left()) w->x = win_left();
        if (w->y < win_top()) w->y = win_top();
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
                    damage_chrome();
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

/* The Lobby's own picture (the default): a ring of light on a quiet navy-grey
 * wall, a floor that reflects it. Integer maths only. The other gradients and
 * pictures of System Settings stay as they were. */
static int glow(int e, int w, int amp) {                 /* a soft peak of height amp, width w, at e = 0 */
    long ww = (long)w * w;
    return (int)(amp * ww / (ww + (long)e * e));
}

static int glow2(int e, int w, int amp) {                /* the same, falling off faster (a halo with no visible end) */
    long ww = (long)w * w, den = ww + (long)e * e;
    return (int)((long)amp * ww / den * ww / den);
}

static void door_wallpaper(void) {
    int R = H * 55 / 100, cx = W * 85 / 100, cy = H * 14 / 100, yh = H * 72 / 100;
    int w1 = 2 * S, w2 = 56 * S, w3 = 36 * S;
    long ro = R + 4 * w2;
    unsigned bg0, bg1, in_near, in_far, core, halo, fl0, fl1;
    int core_a, halo_a, refl_a;
    if (dark) {
        bg0 = 0x0F141B; bg1 = 0x1D2531; in_far = lerp_rgb(TH.s2, TH.s3, 190); in_near = lerp_rgb(TH.s3, TH.accent, 80);
        core = TH.label; halo = TH.accent; fl0 = 0x161C25; fl1 = 0x0B0F14;
        core_a = 235; halo_a = 70; refl_a = 46;
    } else {
        bg0 = 0xE9EDF2; bg1 = 0xD2D9E4; in_far = 0xEEF1F6; in_near = 0xFBFCFD;
        core = TH.accent; halo = 0xFFFFFF; fl0 = 0xDCE1E9; fl1 = 0xC6CDD9;
        core_a = 130; halo_a = 150; refl_a = 90;
    }
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) {
            long dx = x - cx, dy = y - cy, d2 = dx * dx + dy * dy;
            int diag = (int)(((long)y * 3 + (long)(W - x) * 2) * 256 / ((long)H * 3 + (long)W * 2));
            unsigned c = lerp_rgb(bg0, bg1, (unsigned)diag);
            if (y > yh) {                                                 /* the floor and the ring's reflection */
                int tf = (int)((long)(y - yh) * 256 / (H - yh));
                c = lerp_rgb(fl0, fl1, (unsigned)tf);
                long ym = (long)2 * yh - y - cy, m2 = dx * dx + ym * ym;
                int fade = 256 - tf;
                if (m2 < (long)R * R) c = lerp_rgb(c, in_near, (unsigned)(34 * fade / 256));
                if (m2 < ro * ro) c = lerp_rgb(c, halo, (unsigned)glow2(isq(m2) - R, w3, refl_a * fade / 256));
                c = lerp_rgb(c, core, (unsigned)glow(y - yh, 3 * S, 26));    /* the horizon, a hair of light */
            } else if (d2 < ro * ro) {
                int d = isq(d2), e = d - R;
                if (e < 0) {                                              /* inside: lighter towards the ring */
                    int near = 256 - clampi((-e) * 256 / (R * 70 / 100), 0, 256);
                    c = lerp_rgb(in_far, in_near, (unsigned)near);
                }
                c = lerp_rgb(c, halo, (unsigned)glow2(e, w2, halo_a));
                c = lerp_rgb(c, core, (unsigned)glow(e, w1, core_a));
            }
            wallpaper[(long)y * W + x] = c;
        }
}

static void make_wallpaper(void) {
    if (cfg->wall_kind == WS_WALL_PICTURE && cfg->wall_path[0] && wall_picture(cfg->wall_path)) return;
    if (cfg->wall_kind == WS_WALL_COLOR) {
        for (long i = 0; i < (long)W * H; i++) wallpaper[i] = cfg->wall_color & 0xFFFFFF;
        return;
    }
    if (cfg->wall_kind != WS_WALL_GRADIENT || cfg->wall_gradient == 0) {   /* the default: the ring */
        door_wallpaper();
        return;
    }
    unsigned a, b;                                     /* a gradient, a little diagonal */
    ui_wall_gradient(cfg->wall_gradient, dark, &a, &b);
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
#define MAT_MAX (1 << 20)


/* The GPU's shaders do the blur and the fades (gfx_gpu) while the kernel
 * says yes; the CPU's code below is the fallback. */
static int gpu_shaders = 1, gpu_fails;

static int gpu_done(int result) {                     /* 8 failures in a row: the CPU from then on */
    if (result == 0) {
        gpu_fails = 0;
        return 1;
    }
    if (++gpu_fails >= 8) gpu_shaders = 0;
    return 0;
}

/* ---- Drawing lists -------------------------------------------------------
 * Shadows, round fills and copies, window pictures go into a list; then the
 * GPU draws the list as one batch (gfx_gpu_draw), or the CPU does, in the same
 * order: when it is small (a batch costs more), or the GPU says no. */
#define GPU_MIN 8192                                   /* pixels */
#define GPU_SLOT (16L << 20)                           /* the most of a picture the kernel maps */

static long clip_area(const ui_canvas *c, int x0, int y0, int x1, int y1) {
    if (x0 < c->cx0) x0 = c->cx0;
    if (y0 < c->cy0) y0 = c->cy0;
    if (x1 > c->cx1) x1 = c->cx1;
    if (y1 > c->cy1) y1 = c->cy1;
    return x1 > x0 && y1 > y0 ? (long)(x1 - x0) * (y1 - y0) : 0;
}

static struct os_gpu_draw *gd_add(struct os_gpu_draws *d, int op, int x, int y, int w, int h) {
    struct os_gpu_draw *o = &d->op[d->n++];            /* (callers stay within GD_MAX) */
    memset(o, 0, sizeof(*o));
    o->op = op;
    o->x = x;
    o->y = y;
    o->w = w;
    o->h = h;
    return o;
}

static void gd_shadow(struct os_gpu_draws *d, int x, int y, int w, int h, int r, int size, int alpha, int outside) {
    struct os_gpu_draw *o = gd_add(d, GD_SHADOW, x, y, w, h);
    o->r = r;
    o->size = size;
    o->alpha = clampi(alpha, 0, 255);
    o->flags = outside;
}

static void gd_fill(struct os_gpu_draws *d, int x, int y, int w, int h, int r, unsigned color, int alpha, int corners) {
    struct os_gpu_draw *o = gd_add(d, GD_FILL, x, y, w, h);
    o->r = r;
    o->color = color;
    o->alpha = clampi(alpha, 0, 255);
    o->flags = corners;
}

static void over_cpu(ui_canvas *c, const struct os_gpu_draw *o);
static void blit_scaled(painter_t *p, const struct os_gpu_draw *o);

/* -> 1: the GPU drew them. */
static int gd_run(painter_t *p, struct os_gpu_draws *d) {
    long area = 0;
    int fits = d->n > 0;
    for (int i = 0; i < d->n; i++) {
        const struct os_gpu_draw *o = &d->op[i];
        int g = o->op == GD_SHADOW ? o->size : 0;
        area += clip_area(&p->c, o->x - g, o->y - g, o->x + o->w + g, o->y + o->h + g);
        if (o->w < 1 || o->h < 1) fits = 0;
        if (o->op == GD_SCALED && (long)o->src_w * o->src_h * 4 > GPU_SLOT) fits = 0;
        if ((o->op == GD_OVER || o->op == GD_ROUND) && (long)o->h * o->src_stride * 4 > GPU_SLOT) fits = 0;
    }
    if (fits && gpu_shaders && area >= GPU_MIN) {
        d->px = p->c.px;
        d->stride = p->c.stride;
        d->cx0 = p->c.cx0;
        d->cy0 = p->c.cy0;
        d->cx1 = p->c.cx1;
        d->cy1 = p->c.cy1;
        if (gpu_done(gfx_gpu_draw(d))) {
            d->n = 0;
            return 1;
        }
    }
    for (int i = 0; i < d->n; i++) {
        const struct os_gpu_draw *o = &d->op[i];
        if (o->op == GD_SHADOW) ui_shadow_ex(&p->c, o->x, o->y, o->w, o->h, o->r, o->size, o->alpha, o->flags & 1);
        else if (o->op == GD_FILL) ui_round_ex(&p->c, o->x, o->y, o->w, o->h, o->r, o->color, o->alpha, o->flags & 15);
        else if (o->op == GD_ROUND) ui_copy_round(&p->c, o->x, o->y, o->src, o->w, o->h, o->src_stride, o->r, o->flags & 15);
        else if (o->op == GD_OVER) over_cpu(&p->c, o);
        else if (o->op == GD_SCALED) blit_scaled(p, o);
    }
    d->n = 0;
    return 0;
}

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

/* The frosted pictures, each kept until what is behind it changes: the
 * backdrop's samples are taken every time (cheap) and compared with the ones
 * it was made from. Eight, for at most four panels a frame: one in use is
 * never the one taken for another. */
#define MAT_SLOTS 8
typedef struct {
    int x, y, w, h, tint_a, ns, gpu;                   /* gpu: made by the GPU (in memory, not in caches) */
    unsigned tint, used, *pic, *samp;
    long cap;
} mat_t;
static mat_t mats[MAT_SLOTS];
static unsigned mat_clock;
static mutex_t mat_lock;

static int material_make(painter_t *p, unsigned *out, int w, int h, int bw, int bh, int bleed, unsigned tint, int tint_a);

/* A frosted rounded rectangle, into the list. Reduce Transparency (or a size
 * too big to sample): the solid colour instead. */
static void material(painter_t *p, struct os_gpu_draws *d, int x, int y, int w, int h, int r, int corners,
                     unsigned tint, int tint_a, unsigned solid) {
    if (x + w <= p->c.cx0 || x >= p->c.cx1 || y + h <= p->c.cy0 || y >= p->c.cy1) return;
    int bleed = 10 * S, bw = (w + 2 * bleed + 3) / 4, bh = (h + 2 * bleed + 3) / 4;
    if (reduce_transp || w > 2048 || bw > SAMP_W || bh > SAMP_H || (long)w * h > MAT_MAX) {
        gd_fill(d, x, y, w, h, r, solid, 255, corners);
        return;
    }
    int ns = bw * bh;
    for (int cy = 0; cy < bh; cy++)
        for (int cx = 0; cx < bw; cx++)
            p->samp[cy * bw + cx] = backdrop_at(x - bleed + cx * 4 + 2, y - bleed + cy * 4 + 2);
    mutex_lock(&mat_lock);
    mat_t *m = 0;
    for (int i = 0; i < MAT_SLOTS && !m; i++)
        if (mats[i].pic && mats[i].x == x && mats[i].y == y && mats[i].w == w && mats[i].h == h &&
            mats[i].tint == tint && mats[i].tint_a == tint_a)
            m = &mats[i];
    if (m && (m->ns != ns || memcmp(m->samp, p->samp, (size_t)ns * 4))) m->ns = 0;   /* what is behind it changed */
    if (!m) {                                          /* the one used longest ago */
        m = &mats[0];
        for (int i = 1; i < MAT_SLOTS; i++)
            if (mats[i].used < m->used) m = &mats[i];
        m->ns = 0;
    }
    if (!m->ns) {
        if ((long)w * h > m->cap || !m->samp) {
            if (m->pic) munmap(m->pic);
            m->pic = mmap((size_t)w * h * 4, PROT_READ | PROT_WRITE, -1);
            m->cap = m->pic ? (long)w * h : 0;
            if (!m->samp) m->samp = mmap(SAMP_W * SAMP_H * 4, PROT_READ | PROT_WRITE, -1);
        }
        if (!m->pic || !m->samp) {
            if (m->pic) munmap(m->pic);
            m->pic = 0;
            m->cap = 0;
            mutex_unlock(&mat_lock);
            gd_fill(d, x, y, w, h, r, solid, 255, corners);
            return;
        }
        memcpy(m->samp, p->samp, (size_t)ns * 4);      /* (material_make blurs p->samp in place) */
        m->x = x;
        m->y = y;
        m->w = w;
        m->h = h;
        m->tint = tint;
        m->tint_a = tint_a;
        m->gpu = material_make(p, m->pic, w, h, bw, bh, bleed, tint, tint_a);
        m->ns = ns;
    }
    m->used = ++mat_clock;
    struct os_gpu_draw *o = gd_add(d, GD_ROUND, x, y, w, h);
    o->src = m->pic;
    o->src_stride = w;
    o->r = r;
    o->flags = corners | (m->gpu ? GD_KEEP : 0);
    mutex_unlock(&mat_lock);
}

/* The whole w x h picture into out -> 1: the GPU made it. */
static int material_make(painter_t *p, unsigned *out, int w, int h, int bw, int bh, int bleed, unsigned tint, int tint_a) {
    int j0 = 0, j1 = h;
    if (gpu_shaders) {                                 /* the GPU: blur, saturate, tint, upscale (gfx_gpu) */
        struct os_gpu_op op = { .op = GPU_MATERIAL, .samp = p->samp, .scratch = p->samp2, .out = out,
                                .bw = bw, .bh = bh, .w = w, .j0 = j0, .j1 = j1, .bleed = bleed,
                                .sat = 180, .tint = tint, .alpha = (unsigned)tint_a };
        if (gpu_done(gfx_gpu(&op))) return 1;
    }
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
            out[(long)j * w + i] = rf ? lerp_rgb(top, bot, rf) : top;
        }
    }
    return 0;
}

/* ---- Drawing: windows ----------------------------------------------------------- */

static void fit_text(char *out, int n, const char *s, int maxw, int style) {
    snprintf(out, (size_t)n, "%s", s);
    if (ui_text_style_width(out, S, style) <= maxw) return;
    int len = (int)strlen(out);
    while (len > 1) {
        char t[64];
        len--;
        out[len] = 0;
        snprintf(t, sizeof(t), "%s...", out);
        if (ui_text_style_width(t, S, style) <= maxw) {
            snprintf(out, (size_t)n, "%s", t);
            return;
        }
    }
}

/* The window controls (minimise, maximise, close) on the right of the title
 * bar: 36 x 28 pt, 4 pt apart. x is from the content's left edge. */
#define CTL_W (36 * S)
#define CTL_H (28 * S)
static int ctl_x(const window_t *w, int i) { return w->w - 10 * S - (3 - i) * CTL_W - (2 - i) * 4 * S; }
static int ctl_y(void) { return (TITLE_H - CTL_H) / 2; }               /* from the title bar's top */

/* Which control is at a point (the whole bar's height counts as its target). */
static int ctl_at(const window_t *w, int x, int y) {
    if (y < w->y - TITLE_H || y >= w->y) return -1;
    for (int i = 0; i < 3; i++) {
        int bx = w->x + ctl_x(w, i);
        if (x >= bx - 2 * S && x < bx + CTL_W + 2 * S) return i;
    }
    return -1;
}

/* The shadow round an outer rectangle; a255 fades it with the window: a wide
 * soft one for height (E2) and a tight dark one where the window meets the wall. */
static void window_shadows(struct os_gpu_draws *d, int x, int y, int ww, int hh, int focused, int a255) {
    gd_shadow(d, x + 2 * S, y + SHADOW / 3, ww - 4 * S, hh - 2 * S, RADIUS, SHADOW,
              (focused ? TH.shadow_on : TH.shadow_off) * a255 / 255, 0);
    gd_shadow(d, x, y, ww, hh, RADIUS, 2 * S, 50 * a255 / 255, 0);         /* a tight edge */
}

static void window_shadow(painter_t *p, int x, int y, int ww, int hh, int focused, int a255) {
    struct os_gpu_draws d = { .n = 0 };
    window_shadows(&d, x, y, ww, hh, focused, a255);
    gd_run(p, &d);
}

/* The window itself (outline, title bar, content), no shadow, onto `cv` with
 * its content at (cx, cy). hover: 0, or the control under the pointer + 1. */
static void window_frame(ui_canvas *cv, window_t *w, int cx, int cy, int focused, int hover) {
    ui_round(cv, cx - 1, cy - TITLE_H - 1, w->w + 2, w->h + TITLE_H + 2, RADIUS,
             focused ? TH.edge_on : TH.edge_off);                                  /* the 1 px rim */
    ui_round_ex(cv, cx, cy - TITLE_H, w->w, TITLE_H, RADIUS - 1, focused ? TH.title_on : TH.title_off,
                255, UI_TL | UI_TR);
    ui_fill_a(cv, cx + RADIUS, cy - TITLE_H, w->w - 2 * RADIUS, S, TH.sheen, TH.sheen_alpha / 3);   /* light from above */
    ui_fill_a(cv, cx, cy - 1, w->w, 1, TH.hair, TH.hair_alpha);
    int maxw = ctl_x(w, 0) - 16 * S - 12 * S;
    char title[56];
    fit_text(title, sizeof(title), w->title, maxw, UI_BOLD);
    ui_text_bold(cv, cx + 16 * S, cy - TITLE_H + (TITLE_H - 16 * S) / 2, title,
                 focused ? TH.title_text_on : TH.title_text_off, S);
    for (int i = 0; i < 3; i++) {
        int bx = cx + ctl_x(w, i), by = cy - TITLE_H + ctl_y();
        int on = hover == i + 1, off = i == 1 && !w->resizable;
        if (on && !off)
            ui_round_ex(cv, bx, by, CTL_W, CTL_H, R_CONTROL * S, i == 2 ? TH.danger : TH.s3, 255, UI_ALL);
        unsigned ink = off ? TH.muted : on ? (i == 2 ? TH.on_accent : TH.label) : focused ? TH.secondary : TH.muted;
        int gl = i == 0 ? GL_MIN : i == 1 ? (w->zoomed ? GL_RESTORE : GL_MAX) : GL_CLOSE;
        gl_draw(cv, bx + (CTL_W - 18 * S) / 2, by + (CTL_H - 18 * S) / 2, 18 * S, gl, ink, off ? 110 : 255, -1);
    }
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

/* The control hover a window is drawn with. */
static int hover_of(const window_t *w) { return tl_hover >= 0 && &wins[tl_hover] == w ? tl_btn + 1 : 0; }

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
           w->lay_hover == hover && w->lay_flags == (w->resizable | w->zoomed << 1) &&
           w->lay_theme == theme_gen && !strcmp(w->lay_title, w->title);
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
    w->lay_flags = w->resizable | w->zoomed << 1;
    w->lay_theme = theme_gen;
    snprintf(w->lay_title, sizeof(w->lay_title), "%s", w->title);
    w->lay_ok = 1;
}

/* The picture onto the frame; the content's rows are left out (the content
 * covers them) except its rounded bottom, whose corners need the rim behind. */
static void layer_blit(painter_t *p, const window_t *w) {
    int m = lay_margin();
    struct os_gpu_draws d = { .n = 0 };
    struct os_gpu_draw *o = gd_add(&d, GD_OVER, w->x - 1 - m, w->y - TITLE_H - 1 - m, w->lay_w, w->lay_h);
    o->src = w->lay;
    o->src_stride = w->lay_w;
    o->hx0 = w->x;
    o->hy0 = w->y;
    o->hx1 = w->x + w->w;
    o->hy1 = w->y + w->h - RADIUS;
    gd_run(p, &d);
}

/* GD_OVER on the CPU. */
static void over_cpu(ui_canvas *c, const struct os_gpu_draw *o) {
    int lx = o->x, ly = o->y;
    int x0 = lx < c->cx0 ? c->cx0 : lx, x1 = lx + o->w > c->cx1 ? c->cx1 : lx + o->w;
    int y0 = ly < c->cy0 ? c->cy0 : ly, y1 = ly + o->h > c->cy1 ? c->cy1 : ly + o->h;
    for (int py = y0; py < y1; py++) {
        unsigned *row = c->px + (long)py * c->stride;
        const unsigned *src = o->src + (long)(py - ly) * o->src_stride - lx;
        int hole = py >= o->hy0 && py < o->hy1;
        for (int px = x0; px < x1; px++) {
            if (hole && px == o->hx0) {                /* skip the content */
                px = o->hx1 - 1;
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
static void blit_scaled(painter_t *p, const struct os_gpu_draw *o) {
    int dx = o->x, dy = o->y, dw = o->w, dh = o->h, ow = o->src_w, oh = o->src_h, alpha = o->alpha;
    const unsigned *under = o->under, *anim_col = o->src, *anim_mask = o->mask;
    int x0 = dx < p->c.cx0 ? p->c.cx0 : dx, y0 = dy < p->c.cy0 ? p->c.cy0 : dy;
    int x1 = dx + dw > p->c.cx1 ? p->c.cx1 : dx + dw, y1 = dy + dh > p->c.cy1 ? p->c.cy1 : dy + dh;
    int ux = x0, uy = y0, uw = x1 - x0;
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
    if (cache_win != k || cache_focus != focused || cache_hover != hover || cache_damage != w->s->damage ||
        cache_w != ow || cache_h != oh || cache_theme != theme_gen) {   /* drawn once; then only scaled */
        ui_canvas ac, mc;
        ui_canvas_init(&ac, anim_col, ow, oh, ow);
        ui_canvas_init(&mc, anim_mask, ow, oh, ow);
        ui_fill(&ac, 0, 0, ow, oh, focused ? TH.edge_on : TH.edge_off);
        window_body(&ac, w, 1, TITLE_H + 1, focused, hover);
        ui_fill(&mc, 0, 0, ow, oh, 0x000000);
        ui_round(&mc, 0, 0, ow, oh, RADIUS, 0xFFFFFF);
        cache_win = k;
        cache_focus = focused;
        cache_hover = hover;
        cache_damage = w->s->damage;
        cache_w = ow;
        cache_h = oh;
        cache_theme = theme_gen;
        cache_gpu = 0;                                 /* (the CPU wrote it: to memory once) */
    }
    int scale, alpha, cx, cy;
    anim_params(w, &scale, &alpha, &cx, &cy);
    int dw = ow * scale / 1000, dh = oh * scale / 1000;
    if (dw < 1 || dh < 1) return;
    int dx = cx - dw / 2, dy = cy - dh / 2;
    int ux = dx < p->c.cx0 ? p->c.cx0 : dx, uy = dy < p->c.cy0 ? p->c.cy0 : dy;
    int ux1 = dx + dw > p->c.cx1 ? p->c.cx1 : dx + dw, uy1 = dy + dh > p->c.cy1 ? p->c.cy1 : dy + dh;
    int uw = ux1 - ux;
    if (uw <= 0 || uy1 <= uy) return;
    const unsigned *under = 0;
    if (alpha < 255 && snap && (long)uw * (uy1 - uy) <= anim_cap) {
        for (int y = uy; y < uy1; y++)
            memcpy(snap + (long)(y - uy) * uw, p->c.px + (long)y * p->c.stride + ux, (size_t)uw * 4);
        under = snap;
    }
    struct os_gpu_draws d = { .n = 0 };                /* shadow and picture: one batch */
    window_shadows(&d, dx, dy, dw, dh, focused, alpha);
    struct os_gpu_draw *o = gd_add(&d, GD_SCALED, dx, dy, dw, dh);
    o->src = anim_col;
    o->mask = anim_mask;
    o->under = under;
    o->src_w = ow;
    o->src_h = oh;
    o->alpha = alpha;
    o->flags = cache_gpu ? GD_KEEP : 0;
    if (gd_run(p, &d)) cache_gpu = 1;          /* (the whole picture went to memory) */
}

static void draw_window(painter_t *p, int k, int front_win) {
    window_t *w = &wins[k];
    if (w->minimised || w->anim == ANIM_WAIT) return;
    int exiting = w->anim == ANIM_CLOSE || w->anim == ANIM_MIN;
    int focused = front_win || exiting;                                          /* keeps its look while leaving */
    int hover = hover_of(w);
    if (w->anim != ANIM_NONE && anim_ok(w)) {
        draw_window_anim(p, k, focused, hover);
        return;
    }
    if (layer_current(w, focused, hover)) {
        layer_blit(p, w);
        window_content(&p->c, w, w->x, w->y);
        return;
    }
    int x, y, ww, hh;
    outer(w, &x, &y, &ww, &hh);
    window_shadow(p, x, y, ww, hh, focused, 255);
    window_body(&p->c, w, w->x, w->y, focused, hover);
}

/* ---- Drawing: the glass shell ------------------------------------------------------- */

/* A glass panel: depth first (a wide soft shadow for height, a tight one at the
 * edge), a rim, the frosted backdrop, a light edge on top and a shade under it.
 * level 2 = E2 (the Rail, the Launcher, menus), 1 = E1 (the pill). */
static void glass(painter_t *p, int x, int y, int w, int h, int r, int corners, int level, int sheen) {
    int sz = level == 2 ? 24 * S : 10 * S, dy = level == 2 ? 8 * S : 3 * S;
    int al = level == 2 ? TH.shadow_glass : TH.shadow_glass * 2 / 3;
    struct os_gpu_draws d = { .n = 0 };                                           /* (one GPU batch) */
    gd_shadow(&d, x, y + dy, w, h, r, sz, al, 1);                                 /* height: far and soft */
    gd_shadow(&d, x, y + S, w, h, r, 3 * S, al * 2 / 3, 1);                       /* contact: tight and dark */
    gd_fill(&d, x - 1, y - 1, w + 2, h + 2, r + 1, TH.hair, TH.hair_strong, corners);   /* rim */
    material(p, &d, x, y, w, h, r, corners, TH.glass, TH.glass_alpha, TH.s1);
    if (sheen && h >= 4 * r)                                                       /* light falling from above */
        for (int k = 0; k < 5; k++)
            gd_fill(&d, x, y, w, h * (5 - k) / 11, r, TH.sheen, TH.sheen_alpha / 14 + 1, corners & (UI_TL | UI_TR));
    gd_run(p, &d);
    ui_fill_a(&p->c, x + r, y, w - 2 * r, S, TH.sheen, TH.sheen_alpha / 2);        /* the lit top edge */
    ui_fill_a(&p->c, x + r, y + h - S, w - 2 * r, S, 0x000000, 36);                /* and the shade below */
}

/* How far an app's start-up pulse is, 0..255 (0: not starting). */
static int launch_pulse(int i) {
    if (i < 0 || !apps[i].launching) return 0;
    int ph = (int)((uptime_ms() - apps[i].launch_at) % 900), tri = ph < 450 ? ph : 900 - ph;
    return 40 + tri * 150 / 450;
}

static void draw_rail(painter_t *p) {
    ui_canvas *c = &p->c;
    int w = rail_w(), r = 20 * S, ex = r + 4 * S;
    glass(p, -ex, 0, w + ex, H, r, UI_BR, 2, 0);                          /* (its left edge is off the screen) */
    ui_fill_a(c, w - S, 0, S, H, TH.sheen, TH.sheen_alpha / 4);
    int lc = avatar_cx(), ly = logo_cy();
    gl_draw(c, lc - 16 * S, ly - 16 * S, 32 * S, GL_LOGO, ui_fg(H_LOGO), 255, -1);
    for (int i = 0; i < R_COUNT; i++) {
        int id = H_RAIL0 + i, bx = rail_item_x(), by = rail_item_y(i), bw = rail_item_w(), bh = rail_item_h();
        int a = ui_plate(id), pr = press_id == id ? S : 0;
        int ai = i == R_FILES ? app_index("finder") : i == R_SETTINGS ? app_index("settings") : -1;
        int pulse = launch_pulse(ai);
        if (act[id]) ui_shadow(c, bx, by + 2 * S, bw, bh, R_CARD * S + 4 * S, 8 * S, 46 * act[id] / 256);
        if (a || pulse) {
            ui_round_ex(c, bx - 1, by - 1, bw + 2, bh + 2, 17 * S, TH.hair, TH.hair_alpha * (act[id] + hov[id] / 2 > 256 ? 256 : act[id] + hov[id] / 2) / 256, UI_ALL);
            ui_round_ex(c, bx, by, bw, bh, 16 * S, TH.s3, a, UI_ALL);
            if (pulse) ui_round_ex(c, bx, by, bw, bh, 16 * S, TH.accent, pulse / 4, UI_ALL);
        }
        if (act[id])                                                              /* the place marker on the edge */
            ui_round_ex(c, -3 * S, by + 18 * S, 6 * S, bh - 36 * S, 3 * S, TH.accent, act[id] > 255 ? 255 : act[id], UI_ALL);
        unsigned fg = ui_fg(id);
        gl_draw(c, bx + (bw - 24 * S) / 2, by + 9 * S + pr, 24 * S, rail_glyph[i], fg, 255, -1);
        int tw = ui_text_width(rail_label[i], S);
        ui_text(c, bx + (bw - tw) / 2, by + 37 * S + pr, rail_label[i], fg, S);
        if (ai >= 0 && apps[ai].pid > 0 && cfg->dock_dots && !act[id])            /* running, not in front */
            ui_circle(c, bx + bw - 10 * S, by + 10 * S, 2 * S, TH.accent);
    }
    int cx = avatar_cx(), cy = avatar_cy(), pr = press_id == H_AVATAR ? S : 0;
    ui_round_ex(c, cx - 23 * S, cy - 23 * S, 46 * S, 46 * S, 23 * S, TH.hair, TH.hair_strong, UI_ALL);
    ui_round_ex(c, cx - 22 * S, cy - 22 * S, 44 * S, 44 * S, 22 * S, lerp_rgb(TH.s2, TH.s3, (unsigned)hov[H_AVATAR]), 255, UI_ALL);
    gl_draw(c, cx - 11 * S, cy - 11 * S + pr, 22 * S, GL_USER, ui_fg(H_AVATAR), 255, -1);
}

static void draw_pill(painter_t *p) {
    ui_canvas *c = &p->c;
    pill_layout();
    glass(p, pill_x, pill_y, pill_w, pill_h, pill_h / 2, UI_ALL, 1, 0);
    int by = pill_y + 4 * S, bh = 40 * S, ty = pill_y + (pill_h - 16 * S) / 2;
    for (int i = 0; i < pb_n; i++) {
        int id = pb[i].id, bx = pb[i].x, bw = pb[i].w, a = ui_plate(id), pr = press_id == id ? S : 0;
        if (a) ui_round_ex(c, bx + 2 * S, by, bw - 4 * S, bh, 20 * S, TH.s3, a, UI_ALL);
        unsigned fg = ui_fg(id);
        int gx = bx + (bw - 20 * S) / 2, gy = by + 10 * S + pr;
        switch (id) {
        case H_SEARCH: gl_draw(c, gx, gy, 20 * S, GL_SEARCH, fg, 255, -1); break;
        case H_BELL: gl_draw(c, gx, gy, 20 * S, GL_BELL, fg, 255, -1); break;
        case H_WIFI: gl_draw(c, gx, gy, 20 * S, GL_WIFI, fg, wifi_on ? 255 : 110, -1); break;
        case H_SOUND: gl_draw(c, gx, gy, 20 * S, GL_SOUND, fg, vol_level ? 255 : 110, -1); break;
        case H_THEME: gl_draw(c, gx, gy, 20 * S, dark ? GL_MOON : GL_SUN, fg, 255, -1); break;
        case H_BATTERY: {
            int pct = bat & 0xFF, charging = bat & 0x100;
            unsigned bc = pct <= 15 && !charging ? TH.danger : charging ? TH.success : fg;
            gl_draw(c, bx + 12 * S, gy, 20 * S, GL_BATTERY, bc, 255, pct);
            ui_text(c, bx + 12 * S + 20 * S + 6 * S, ty + pr, bat_text, fg, S);
            break;
        }
        }
    }
    ui_text(c, time_x + 10 * S, ty, clock_text, TH.label, S);
}

/* One tile of the Launcher: a plate that lifts and brightens under the
 * pointer, an icon that takes the state's colour, a name below. */
static void draw_tile(painter_t *p, int i, int cx, int cy, const panel_geo_t *g) {
    ui_canvas *c = &p->c;
    int id = H_TILE0 + i, size = 56 * S;
    litem_t *it = &items[i];
    int tx = cx + (g->cw - size) / 2;
    int lift = reduce_motion ? 0 : hov[id] * 2 * S / 256, ty = cy + 4 * S - lift + (press_id == id ? S : 0);
    int pulse = it->kind == LK_APP ? launch_pulse(it->idx) : 0;
    if (i == panel_sel) ui_round_ex(c, tx - 3 * S, ty - 3 * S, size + 6 * S, size + 6 * S, 19 * S, TH.accent, 190, UI_ALL);
    ui_shadow_ex(c, tx, ty + 2 * S + lift, size, size, 16 * S, 8 * S + lift, 30 + hov[id] * 60 / 256, 1);   /* it lifts: the shadow grows */
    ui_round_ex(c, tx - 1, ty - 1, size + 2, size + 2, 17 * S, TH.hair,
                TH.hair_alpha + hov[id] * (TH.hair_strong - TH.hair_alpha) / 256, UI_ALL);
    int fa = 150 + hov[id] * 85 / 256 + (press_id == id ? 20 : 0);
    ui_round_ex(c, tx, ty, size, size, 16 * S, lerp_rgb(TH.s2, TH.s3, (unsigned)hov[id]), fa > 255 ? 255 : fa, UI_ALL);
    if (act[id]) ui_round_ex(c, tx, ty, size, size, 16 * S, TH.accent, act[id] * 30 / 256, UI_ALL);
    if (pulse) ui_round_ex(c, tx, ty, size, size, 16 * S, TH.accent, pulse / 3, UI_ALL);
    ui_fill_a(c, tx + 16 * S, ty, size - 32 * S, S, TH.sheen, TH.sheen_alpha / 3);              /* a lit top edge */
    unsigned fg = ui_fg(id);
    int gl = 0;
    const dock_app_t *a = 0;
    if (it->kind == LK_APP) a = &apps[it->idx];
    else if (it->kind == LK_PANE) gl = panes[it->idx].glyph;
    else {
        gl = GL_APPS;
        for (int k = 0; k < DOCK_APPS; k++)
            if (apps[k].pid > 0 && apps[k].pid == wins[it->idx].pid) { a = &apps[k]; break; }
    }
    if (a) gl = glyph_for(a->icon);
    if (gl) {
        gl_draw(c, tx + (size - 28 * S) / 2, ty + (size - 28 * S) / 2, 28 * S, gl, fg, 255, a || it->kind == LK_PANE ? 60 : -1);
    } else if (a && a->icon && a->icon[0] && ui_icon(c, tx + 8 * S, ty + 8 * S, 40 * S, a->icon, 255) == 0) {
        /* its own picture */
    } else if (a) {                                                   /* no icon: its first letter */
        int ts = 2 * S, gw = ui_text_style_width(a->glyph, ts, UI_BOLD);
        ui_text_bold(c, tx + (size - gw) / 2, ty + (size - 16 * ts) / 2, a->glyph, fg, ts);
    }
    char label[48];
    fit_text(label, sizeof(label), it->label, g->cw - 8 * S, UI_REGULAR);
    unsigned lc = lerp_rgb(TH.secondary, TH.label, (unsigned)(hov[id] > act[id] ? hov[id] : act[id]));
    int lw = ui_text_width(label, S);
    ui_text(c, cx + (g->cw - lw) / 2, cy + 4 * S + size + 8 * S, label, lc, S);
    if (act[id] && cfg->dock_dots) ui_circle(c, cx + g->cw / 2, cy + g->ch - 5 * S, 2 * S, TH.accent);
}

static void draw_launcher(painter_t *p) {
    if (!panel_vis()) return;
    ui_canvas *c = &p->c;
    panel_geo_t g;
    panel_geo(&g);
    int off = panel_off(), x = g.x, y = g.y + off;
    glass(p, x, y, g.w, g.h, R_LARGE * S, UI_ALL, 2, 1);
    int fx, fy, fw, fh;
    field_rect(&g, &fx, &fy, &fw, &fh);
    fy += off;
    if (panel_mode == PM_APPS) {                                       /* the search field: always in focus */
        int fa = 170 + hov[H_FIELD] * 50 / 256;
        ui_round_ex(c, fx - 1, fy - 1, fw + 2, fh + 2, 13 * S, TH.accent, 120 + hov[H_FIELD] * 60 / 256, UI_ALL);
        ui_round_ex(c, fx, fy, fw, fh, 12 * S, TH.s2, fa, UI_ALL);
        gl_draw(c, fx + 14 * S, fy + 12 * S, 20 * S, GL_SEARCH, qlen ? TH.label : TH.muted, 255, -1);
        int tx = fx + 46 * S, ty = fy + (fh - 16 * S) / 2;
        if (qlen) {
            int tw = ui_text(c, tx, ty, query, TH.label, S);
            if (caret_on) ui_fill(c, tx + tw + 2 * S, fy + 12 * S, S + S / 2, 20 * S, TH.accent);
        } else {
            if (caret_on) ui_fill(c, tx, fy + 12 * S, S + S / 2, 20 * S, TH.accent);
            ui_text(c, tx + 4 * S, ty, "Search apps and settings", TH.muted, S);
        }
    } else {
        const char *title = panel_mode == PM_SPACES ? "Spaces" : "Create";
        ui_text_bold(c, fx + 4 * S, fy + (fh - 16 * S) / 2, title, TH.label, S);
        char cnt[24];
        snprintf(cnt, sizeof(cnt), panel_mode == PM_SPACES ? "%d open" : "%d apps", n_items);
        int cw = ui_text_width(cnt, S);
        ui_text(c, fx + fw - cw - 4 * S, fy + (fh - 16 * S) / 2, cnt, TH.muted, S);
    }
    for (int i = 0; i < n_items; i++) {
        int tx, ty;
        if (tile_rect(i, &g, &tx, &ty)) draw_tile(p, i, tx, ty + off, &g);
    }
    if (!n_items) {
        const char *msg = panel_mode == PM_SPACES ? "No windows are open" : panel_mode == PM_APPS ? "Nothing found" : "No apps here yet";
        int mw = ui_text_width(msg, S);
        ui_text(c, x + (g.w - mw) / 2, y + g.gy + g.rows * g.ch / 2 - 8 * S, msg, TH.muted, S);
    }
    int total = (n_items + 3) / 4;                                     /* more rows than fit: the handle shows where */
    ui_fill_a(c, x + 20 * S, y + g.h - 60 * S, g.w - 40 * S, S, TH.hair, TH.hair_alpha);
    int bx, by, bw, bh;
    power_rect(&g, &bx, &by, &bw, &bh);
    by += off;
    int a = ui_plate(H_POWER);
    if (a) ui_round_ex(c, bx, by, bw, bh, 20 * S, TH.s3, a, UI_ALL);
    gl_draw(c, bx + 10 * S, by + 10 * S + (press_id == H_POWER ? S : 0), 20 * S, GL_POWER, ui_fg(H_POWER), 255, -1);
    int hw = 36 * S, hx = x + g.w / 2 - hw / 2 + (total > g.rows ? (panel_scroll * 24 * S / total - 12 * S) : 0);
    ui_round_ex(c, hx, y + g.h - 16 * S, hw, 4 * S, 2 * S, TH.muted, 130, UI_ALL);
}

static void draw_menu(painter_t *p) {
    if (!menu_open) return;
    ui_canvas *c = &p->c;
    int x = menu_x(), y = menu_y(), w = menu_w(), h = menu_h(), r = R_CARD * S;
    glass(p, x, y, w, h, r, UI_ALL, 2, 0);
    for (int i = 0; i < n_mitems; i++) {
        int iy = menu_item_y(i), ih = mi_h(i);
        if (mitems[i].flags & MI_SEP) {
            ui_fill_a(c, x + 8 * S, iy + ih / 2, w - 16 * S, S, TH.hair, TH.hair_alpha);
            continue;
        }
        int off = mitems[i].flags & MI_OFF, hl = i == menu_hot && !off;
        if (hl) ui_round(c, x + 4 * S, iy + 2 * S, w - 8 * S, ih - 4 * S, R_CONTROL * S, TH.accent);
        unsigned fg = hl ? TH.on_accent : off ? TH.muted : TH.label;
        int ty = iy + (ih - 16 * S) / 2;
        ui_text(c, x + 16 * S, ty, mitems[i].label, fg, S);
        if (mitems[i].key[0]) {
            int kw = ui_text_width(mitems[i].key, S);
            ui_text(c, x + w - 16 * S - kw, ty, mitems[i].key, hl ? TH.on_accent : TH.muted, S);
        }
    }
}

/* A panel fading in (ease-out): what is behind it is saved, the panel drawn,
 * then the two mixed. Micro and major motion, so Reduce Motion keeps the fade. */
static void draw_faded(painter_t *p, int x0, int y0, int x1, int y1, int a, void (*fn)(painter_t *)) {
    if (x0 < p->c.cx0) x0 = p->c.cx0;
    if (y0 < p->c.cy0) y0 = p->c.cy0;
    if (x1 > p->c.cx1) x1 = p->c.cx1;
    if (y1 > p->c.cy1) y1 = p->c.cy1;
    if (a >= 255 || !snap || x1 <= x0 || y1 <= y0 || (long)(x1 - x0) * (y1 - y0) > anim_cap) {
        fn(p);
        return;
    }
    for (int y = y0; y < y1; y++)
        memcpy(snap + (long)(y - y0) * (x1 - x0), p->c.px + (long)y * p->c.stride + x0, (size_t)(x1 - x0) * 4);
    fn(p);
    if (gpu_shaders) {                                 /* the GPU blends (gfx_gpu) */
        struct os_gpu_op op = { .op = GPU_OP_FADE, .old = snap, .out = p->c.px + (long)y0 * p->c.stride + x0,
                                .w = x1 - x0, .h = y1 - y0, .pitch = p->c.stride, .f = (unsigned)(a + (a >> 7)) };
        if (gpu_done(gfx_gpu(&op))) return;
    }
    for (int y = y0; y < y1; y++) {
        unsigned *row = p->c.px + (long)y * p->c.stride + x0;
        const unsigned *old = snap + (long)(y - y0) * (x1 - x0);
        for (int x = 0; x < x1 - x0; x++) row[x] = lerp_rgb(old[x], row[x], (unsigned)(a + (a >> 7)));
    }
}

static void draw_launcher_faded(painter_t *p) {
    if (!panel_vis()) return;
    panel_geo_t g;
    panel_geo(&g);
    int a = (int)(255 * ease_out(lt) / 1024);
    draw_faded(p, g.x - SHADOW, g.y - SHADOW, g.x + g.w + SHADOW, g.y + g.h + SHADOW + 20 * S, a, draw_launcher);
}

static void draw_menu_faded(painter_t *p) {
    if (!menu_open) return;
    int a = (int)(255 * ease_out((long)menu_fade_ms * 1024 / FADE_MS) / 1024);
    draw_faded(p, menu_x() - SHADOW, menu_y() - SHADOW, menu_x() + menu_w() + SHADOW, menu_y() + menu_h() + SHADOW + 8 * S, a, draw_menu);
}

static void paint(painter_t *p, int by0, int by1) {
    window_t *f = front();
    for (int d = 0; d < ndmg; d++) {
        int x0 = dmg[d].x0, y0 = dmg[d].y0 > by0 ? dmg[d].y0 : by0, x1 = dmg[d].x1, y1 = dmg[d].y1 < by1 ? dmg[d].y1 : by1;
        if (y1 <= y0) continue;
        ui_clip(&p->c, x0, y0, x1 - x0, y1 - y0);
        ui_copy(&p->c, x0, y0, wallpaper + (long)y0 * W + x0, x1 - x0, y1 - y0, W);
        for (int i = 0; i < norder; i++) draw_window(p, order[i], &wins[order[i]] == f);
        draw_rail(p);
        draw_pill(p);
        draw_launcher_faded(p);
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
    if (lt != (panel_open ? 1024 : 0)) return 1;
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

static void plan_gpu_copies(void) {
    static struct os_copy items[64];
    int n = 0;
    for (int k = 0; k < MAX_WINDOWS; k++) wins[k].gpu_body = 0;
    if (!gpu_frames || !kernel_cursor || motion()) return;
    int dock_top = H;
    pill_layout();
    for (int i = 0; i < norder; i++) {
        window_t *w = &wins[order[i]];
        if (!live(w) || w->minimised || w->anim != ANIM_NONE) continue;
        if (w->x < rail_w() + SHADOW) continue;                   /* under the Rail's shadow: the CPU draws it */
        int x0 = w->x, x1 = w->x + w->w > W ? W : w->x + w->w;
        int y0 = w->y, y1 = w->y + w->h - RADIUS;
        if (x1 > pill_x - SHADOW && y0 < pill_y + pill_h + SHADOW) y0 = pill_y + pill_h + SHADOW;   /* the pill stays with the CPU */
        if (y1 > dock_top) y1 = dock_top;
        if (x1 <= x0 || y1 <= y0) continue;
        panel_geo_t pg;
        panel_geo(&pg);
        int covered = (menu_open && overlaps(x0, y0, x1, y1, menu_x() - SHADOW, menu_y() - SHADOW,
                                             menu_w() + 2 * SHADOW, menu_h() + 2 * SHADOW)) ||
                      (panel_vis() && overlaps(x0, y0, x1, y1, pg.x - SHADOW, pg.y - SHADOW,
                                               pg.w + 2 * SHADOW, pg.h + 2 * SHADOW + 20 * S));
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
            layer_update(&painters[0], w, w == f, hover_of(w));
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

/* The window control under the pointer, if any: tl_hover (the window) and tl_btn. */
static void control_hover(int *win, int *btn) {
    *win = *btn = -1;
    int k = window_at(mouse_x, mouse_y);
    if (k < 0) return;
    int b = ctl_at(&wins[k], mouse_x, mouse_y);
    if (b < 0) return;
    *win = k;
    *btn = b;
}

static int lobby_peek;                           /* the Lobby is showing: the windows are hidden */

/* Lobby: step back from the windows to the bare wall, and again to bring them back. */
static void lobby_toggle(void) {
    close_menu();
    panel_hide();
    if (!lobby_peek) {
        int any = 0;
        for (int k = 0; k < MAX_WINDOWS; k++)
            if (live(&wins[k])) {
                wins[k].peek = 1;
                minimise_window(k);
                any = 1;
            }
        lobby_peek = any;
        return;
    }
    int list[MAX_WINDOWS], n = 0;
    for (int i = 0; i < norder; i++)                       /* back to front, so the stack comes back as it was */
        if (wins[order[i]].used && wins[order[i]].peek) list[n++] = order[i];
    for (int i = 0; i < n; i++) {
        wins[list[i]].peek = 0;
        restore_window(list[i]);
        raise(list[i]);
    }
    lobby_peek = 0;
}

static void launch(int i) {
    for (int k = 0; k < MAX_WINDOWS; k++)            /* running: bring its window back */
        if (wins[k].used && wins[k].anim != ANIM_CLOSE && apps[i].pid > 0 && wins[k].pid == apps[i].pid) {
            restore_window(k);
            raise(k);
            return;
        }
    if (apps[i].pid > 0 && apps[i].launching) return;     /* a second click while it starts */
    char *argv[] = { (char *)apps[i].program, 0 };
    int pid = spawn(apps[i].program, argv, 0);
    printf("windowserver: open %s: %s\n", apps[i].program, pid > 0 ? "started" : "cannot start");   /* (in the log) */
    if (pid > 0) {
        apps[i].pid = pid;
        apps[i].launching = cfg->dock_bounce != 0;       /* "Animate opening apps": the item pulses meanwhile */
        apps[i].launch_at = uptime_ms();
    }
    damage_chrome();
    damage_launcher();
}

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

static void open_program(const char *program) {
    int i = app_index(program);
    if (i >= 0) launch(i);
    else run(program, 0);
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
    }
}

/* Keys while a menu is open: Esc, Up/Down, Enter. */
static void menu_key(int key) {
    if (key == 27) {
        close_menu();
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

/* A tile of the Launcher was chosen. */
static void activate_item(int i) {
    if (i < 0 || i >= n_items) return;
    litem_t it = items[i];
    panel_hide();
    if (it.kind == LK_APP) {
        launch(it.idx);
    } else if (it.kind == LK_WIN) {
        if (wins[it.idx].used) {
            restore_window(it.idx);
            raise(it.idx);
        }
    } else {
        run("settings", panes[it.idx].pane);
    }
}

/* Keys while the Launcher is open: type to search, arrows to choose, Enter, Esc. */
static void panel_key(int key) {
    if (key == 27) {
        panel_hide();
        return;
    }
    if (key == '\n' || key == '\r') {
        int i = panel_sel >= 0 ? panel_sel : (qlen && n_items ? 0 : -1);
        if (i >= 0) activate_item(i);
        return;
    }
    if (key == KEYCODE_LEFT || key == KEYCODE_RIGHT || key == KEYCODE_UP || key == KEYCODE_DOWN) {
        if (!n_items) return;
        if (panel_sel < 0) panel_sel = 0;
        else panel_sel += key == KEYCODE_RIGHT ? 1 : key == KEYCODE_LEFT ? -1 : key == KEYCODE_DOWN ? 4 : -4;
        panel_sel = clampi(panel_sel, 0, n_items - 1);
        panel_reveal();
        damage_launcher();
        return;
    }
    if (panel_mode != PM_APPS) return;
    int changed = 0;
    if ((key == '\b' || key == 127) && qlen) {
        query[--qlen] = 0;
        changed = 1;
    } else if (key >= 32 && key < 127 && qlen < (int)sizeof(query) - 1) {
        query[qlen++] = (char)key;
        query[qlen] = 0;
        changed = 1;
    }
    if (changed) {
        panel_sel = qlen ? 0 : -1;
        panel_scroll = 0;
        build_items();
        damage_launcher();
    }
}

static void damage_for(int id) {
    if (id < H_SEARCH) damage_rail();
    else if (id <= H_THEME) damage_pill();
    else damage_launcher();
}

/* A rail item, a pill button or a Launcher tile was clicked (pressed and released on it). */
static void activate(int id) {
    if (id >= H_TILE0) {
        activate_item(id - H_TILE0);
        return;
    }
    switch (id) {
    case H_RAIL0 + R_LOBBY: lobby_toggle(); break;
    case H_RAIL0 + R_APPS: panel_toggle(PM_APPS); break;
    case H_RAIL0 + R_FILES: panel_hide(); open_program("finder"); break;
    case H_RAIL0 + R_SPACES: panel_toggle(PM_SPACES); break;
    case H_RAIL0 + R_CREATE: panel_toggle(PM_CREATE); break;
    case H_RAIL0 + R_SETTINGS: panel_hide(); open_program("settings"); break;
    case H_AVATAR: open_menu(1, rail_w() + 8 * S, H - 16 * S, MA_UP); break;
    case H_LOGO: run("about", 0); break;
    case H_SEARCH: panel_show(PM_APPS); break;
    case H_BELL:
        pill_layout();
        for (int i = 0; i < pb_n; i++)
            if (pb[i].id == H_BELL) open_menu(3, pb[i].x + pb[i].w, pill_y + pill_h + 8 * S, MA_RIGHT);
        break;
    case H_WIFI: run("settings", "wifi"); break;
    case H_SOUND: run("settings", "sound"); break;
    case H_BATTERY: run("settings", "battery"); break;
    case H_THEME:
        cfg->dark = !cfg->dark;
        cfg->seq++;
        break;
    case H_POWER: {
        panel_geo_t g;
        int bx, by, bw, bh;
        panel_geo(&g);
        power_rect(&g, &bx, &by, &bw, &bh);
        open_menu(2, bx, by - 8 * S, MA_UP);
        break;
    }
    default: break;
    }
}

static void press(void) {
    printf("windowserver: click at %d,%d\n", mouse_x, mouse_y);   /* (for the tests) */
    if (menu_open) {                                   /* any click closes the menu */
        int i = menu_hit(mouse_x, mouse_y);
        int id = i >= 0 && !(mitems[i].flags & MI_OFF) ? mitems[i].id : 0;
        close_menu();
        if (id) menu_choose(id);
        return;
    }
    int h = ui_hit(mouse_x, mouse_y);
    if (h == -2) return;                               /* in the Launcher, on nothing */
    if (h >= 0) {                                      /* acts when the button comes up over it */
        press_id = h;
        damage_for(h);
        return;
    }
    if (panel_open) panel_hide();                      /* a click anywhere else puts the Launcher away */
    int k = window_at(mouse_x, mouse_y);
    if (k < 0) return;
    window_t *w = &wins[k];
    raise(k);
    if (mouse_y < w->y) {                              /* the title bar */
        int b = ctl_at(w, mouse_x, mouse_y);
        if (b == 0) {
            minimise_window(k);
            return;
        }
        if (b == 1) {
            if (w->resizable) zoom_window(k);
            return;
        }
        if (b == 2) {
            close_window_request(k);
            return;
        }
        unsigned long now = uptime_ms();
        if (last_title_win == k && now - last_title_click < 400) {   /* double-click: zoom (or minimise) */
            last_title_win = -1;
            if (w->resizable) zoom_window(k);
            else minimise_window(k);
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
        mouse_x = m.x;
        mouse_y = m.y;
        damage_cursor();
        if (menu_open) {
            int hl = menu_hit(mouse_x, mouse_y);
            if (hl >= 0 && (mitems[hl].flags & MI_OFF)) hl = -1;
            if (hl != menu_hot) {
                menu_hot = hl;
                damage_menu();
            }
        }
        int tw, tb;
        if (menu_open) tw = tb = -1;
        else control_hover(&tw, &tb);
        if (tw != tl_hover || tb != tl_btn) {
            if (tl_hover >= 0 && wins[tl_hover].used) damage_title(&wins[tl_hover]);
            tl_hover = tw;
            tl_btn = tb;
            if (tw >= 0) damage_title(&wins[tw]);
        }
        if (drag >= 0) {
            window_t *w = &wins[drag];
            damage_window(w);
            w->x = mouse_x - drag_dx;
            w->y = mouse_y - drag_dy;
            if (w->y < TITLE_H + 4 * S) w->y = TITLE_H + 4 * S;
            if (w->x < rail_w() + 8 * S) w->x = rail_w() + 8 * S;
            damage_window(w);
        } else if (capture >= 0 && (m.buttons & MOUSE_BUTTON_LEFT)) {
            send_event(&wins[capture], WS_EV_MOVE, mouse_x - wins[capture].x, mouse_y - wins[capture].y, 0);
        }
    }
    buttons = m.buttons;
    if ((buttons & MOUSE_BUTTON_LEFT) && !(was & MOUSE_BUTTON_LEFT)) press();
    if (!(buttons & MOUSE_BUTTON_LEFT) && (was & MOUSE_BUTTON_LEFT)) {
        if (press_id >= 0) {
            int id = press_id, over = ui_hit(mouse_x, mouse_y);
            press_id = -1;
            damage_for(id);
            if (over == id) activate(id);
        }
        if (capture >= 0)
            send_event(&wins[capture], WS_EV_UP, mouse_x - wins[capture].x, mouse_y - wins[capture].y, 0);
        drag = capture = -1;
    }
    if (m.wheel) {
        panel_geo_t g;
        panel_geo(&g);
        if (panel_vis() && !menu_open && ui_inside(mouse_x, mouse_y, g.x, g.y, g.w, g.h)) {
            int total = (n_items + 3) / 4, most = total > g.rows ? total - g.rows : 0;
            int next = clampi(panel_scroll + (m.wheel > 0 ? 1 : -1), 0, most);
            if (next != panel_scroll) {
                panel_scroll = next;
                damage_launcher();
            }
        } else {
            int k = window_at(mouse_x, mouse_y);
            if (k >= 0) send_event(&wins[k], WS_EV_WHEEL, mouse_x - wins[k].x, mouse_y - wins[k].y, m.wheel);
        }
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
 * restoring) and the open menu's fade. The Rail, the pill and the Launcher
 * move in animate_ui. */
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
            int moving = spring_step(&w->sc, 100L * FP, dt, 200, 28);   /* critically damped: no overshoot */
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
                damage_chrome();
                damage_launcher();
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
            damage_pill();
        }
    }
    struct os_time t;
    char now[40];
    gettime(&t);
    int h12 = t.hour % 12 == 0 ? 12 : t.hour % 12;       /* "Tue 13 Oct 5:54 PM" */
    char date[16] = "", hm[16], sec[8] = "";
    if (cfg->clock_date)
        snprintf(date, sizeof(date), "%s, %d %s   ", days[weekday(t.year, t.month, t.day)], t.day, months[(t.month + 11) % 12]);
    if (cfg->clock_seconds) snprintf(sec, sizeof(sec), ":%02d", t.second);
    if (cfg->clock_24h) snprintf(hm, sizeof(hm), "%02d:%02d%s", t.hour, t.minute, sec);
    else snprintf(hm, sizeof(hm), "%d:%02d%s %s", h12, t.minute, sec, t.hour < 12 ? "AM" : "PM");
    snprintf(now, sizeof(now), "%s%s", date, hm);
    if (strcmp(now, clock_text)) {
        strcpy(clock_text, now);
        damage_pill();
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
 * clock; every window hears of it (WS_EV_PREFS). (The Dock's size and
 * magnification of System Settings no longer apply: the Rail has one size.) */
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
    if (look || wall) damage(0, 0, W, H);
    else damage_chrome();                              /* the clock's format, the running dots */
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
    /* The scale follows the screen, not the console font: 2x only on very tall (4K-class) screens. */
    S = (H >= 1600 && W >= 2560) ? 2 : 1;
    TITLE_H = 36 * S;
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
    long cap = (long)W * H < (1L << 21) ? (long)W * H : (1L << 21);       /* 8 MB each */
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
    glyphs_init();
    load_dock();
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
            if (key == KEY_LAUNCH) {
                panel_toggle(PM_APPS);
                continue;
            }
            if (menu_open) {
                menu_key(key);
                continue;
            }
            if (panel_open) {
                panel_key(key);
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
        animate_ui(dt);
        if (items_dirty && panel_open) {
            items_dirty = 0;
            build_items();
            damage_launcher();
        }
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