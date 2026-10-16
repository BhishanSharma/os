// finder - Files: browse folders the way the macOS Finder does.
//
//   * a sidebar of places (Home, Temporary, System Disk) that runs the full
//     height of the window,
//   * a toolbar with Back / Forward (a history) and a search field that
//     filters the folder as you type (Ctrl+F focuses it, Esc clears it),
//   * a list with Name, Size and Kind columns, striped rows and a rounded
//     selection (the table's .inset style), sorted by name,
//   * a path bar at the bottom: click a folder in it to go there.
//
// Double-click (or Enter) opens a folder, or a file in the program that shows
// it (pictures in view, videos in video, programs in terminal, the rest in
// textedit). Backspace goes up a folder; typing a letter selects the next
// name that starts with it. Colours are semantic tokens (ui_color), so the
// window follows Dark and Increase Contrast; it redraws on WS_EV_PREFS.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <os.h>
#include <ui.h>

#define MAX_ENTRIES 128
#define PATH_LEN 256
#define HIST 32
#define KEY_CTRL_F 6

static ui_window win;
static struct os_dirent entries[MAX_ENTRIES];
static int count;                         /* entries in the folder */
static int sorted[MAX_ENTRIES];           /* entry numbers, by name */
static int vis[MAX_ENTRIES], nvis;        /* the ones the search shows */
static int selected = -1, first;          /* a row of vis; the first row on screen */
static char cwd[PATH_LEN];
static char query[48];
static int qlen, searching;               /* the search text; does the field have the keys */
static char hist[HIST][PATH_LEN];
static int hist_n, hist_pos;
static int pressed;                       /* toolbar button held: 1 back, 2 forward */
static int S;

#define ROW (24 * S)

/* ---- Places (the sidebar) ------------------------------------------------- */

typedef struct {
    const char *label, *icon;
    char path[64];
    int header, y;
} place_t;
static place_t places[6];
static int nplaces;

static void add_place(const char *label, const char *icon, const char *path, int header) {
    place_t *p = &places[nplaces++];
    p->label = label;
    p->icon = icon;
    p->header = header;
    snprintf(p->path, sizeof(p->path), "%s", path ? path : "");
}

static void init_places(const char *home) {
    add_place("Favorites", 0, 0, 1);
    add_place("Home", "user", home, 0);
    add_place("Temporary", "folder", "/TMP", 0);
    add_place("Locations", 0, 0, 1);
    add_place("System Disk", "computer", "/", 0);
}

/* ---- Names, kinds, sizes --------------------------------------------------- */

static int upper(int c) { return c >= 'a' && c <= 'z' ? c - 32 : c; }

static int ci_cmp(const char *a, const char *b) {
    while (*a && upper((unsigned char)*a) == upper((unsigned char)*b)) {
        a++;
        b++;
    }
    return upper((unsigned char)*a) - upper((unsigned char)*b);
}

static int contains_ci(const char *hay, const char *needle) {
    for (; *hay; hay++) {
        const char *h = hay, *n = needle;
        while (*n && upper((unsigned char)*h) == upper((unsigned char)*n)) {
            h++;
            n++;
        }
        if (!*n) return 1;
    }
    return 0;
}

static int has_ending(const char *name, const char *end) {
    size_t n = strlen(name), e = strlen(end);
    if (n < e) return 0;
    for (size_t i = 0; i < e; i++)
        if (upper((unsigned char)name[n - e + i]) != end[i]) return 0;
    return 1;
}

static int is_picture(const char *n) { return has_ending(n, ".PNG") || has_ending(n, ".BMP") || has_ending(n, ".PPM") || has_ending(n, ".JPG"); }
static int is_movie(const char *n) { return has_ending(n, ".MPG") || has_ending(n, ".MPEG"); }
static int is_program(const char *n) { return has_ending(n, ".ELF"); }

static const char *icon_of(const struct os_dirent *e) {
    if (e->is_dir) return "folder";
    if (is_picture(e->name)) return "picture";
    if (is_movie(e->name)) return "video";
    if (is_program(e->name)) return "terminal";
    return "file";
}

static const char *kind_of(const struct os_dirent *e) {
    if (e->is_dir) return "Folder";
    if (is_program(e->name)) return "Program";
    if (is_picture(e->name)) return "Image";
    if (is_movie(e->name)) return "Movie";
    if (has_ending(e->name, ".TXT") || has_ending(e->name, ".MD") || has_ending(e->name, ".C") || has_ending(e->name, ".H"))
        return "Plain Text";
    return "Document";
}

/* Sizes the way the Finder shows them: decimal units, one decimal place. */
static void fmt_size(char *out, size_t n, const struct os_dirent *e) {
    unsigned v = e->size;
    if (e->is_dir) snprintf(out, n, "--");
    else if (v < 1000) snprintf(out, n, "%u bytes", v);
    else if (v < 1000000) snprintf(out, n, "%u.%u KB", v / 1000, (v % 1000) / 100);
    else snprintf(out, n, "%u.%u MB", v / 1000000, (v % 1000000) / 100000);
}

/* `s` cut to `max_w` pixels, ending in "..." when it was cut. */
static void fit(char *out, size_t cap, const char *s, int max_w) {
    snprintf(out, cap, "%s", s);
    if (ui_text_width(out, S) <= max_w) return;
    size_t n = strlen(out);
    char tmp[96];
    while (n > 1) {
        out[--n] = 0;
        snprintf(tmp, sizeof(tmp), "%s...", out);
        if (ui_text_width(tmp, S) <= max_w) {
            snprintf(out, cap, "%s", tmp);
            return;
        }
    }
}

/* ---- The folder -------------------------------------------------------------- */

static void refilter(void) {
    nvis = 0;
    for (int i = 0; i < count; i++)
        if (!qlen || contains_ci(entries[sorted[i]].name, query)) vis[nvis++] = sorted[i];
    selected = -1;
    first = 0;
}

static void load(void) {
    getcwd(cwd, sizeof(cwd));
    int n = listdir(0, entries, MAX_ENTRIES), k = 0;
    for (int i = 0; i < n; i++) {
        if (!strcmp(entries[i].name, ".") || !strcmp(entries[i].name, "..")) continue;
        if (k != i) entries[k] = entries[i];
        k++;
    }
    count = k;
    for (int i = 0; i < count; i++) sorted[i] = i;
    for (int i = 1; i < count; i++) {                      /* by name, like the Finder */
        int v = sorted[i], j = i - 1;
        while (j >= 0 && ci_cmp(entries[sorted[j]].name, entries[v].name) > 0) {
            sorted[j + 1] = sorted[j];
            j--;
        }
        sorted[j + 1] = v;
    }
    refilter();
    const char *base = strrchr(cwd, '/');
    ui_set_title(&win, base && base[1] ? base + 1 : "System Disk");   /* the folder's name */
}

static void push_history(void) {
    if (hist_n && !strcmp(hist[hist_pos], cwd)) return;
    hist_n = hist_n ? hist_pos + 1 : 0;                    /* a new place drops the "forward" ones */
    if (hist_n == HIST) {
        memmove(hist[0], hist[1], sizeof(hist[0]) * (HIST - 1));
        hist_n--;
    }
    snprintf(hist[hist_n++], PATH_LEN, "%s", cwd);
    hist_pos = hist_n - 1;
}

static void clear_search(void) {
    qlen = 0;
    query[0] = 0;
    searching = 0;
}

static void go(const char *path) {
    if (chdir(path) < 0) return;
    clear_search();
    load();
    push_history();
}

static void go_history(int d) {
    int np = hist_pos + d;
    if (np < 0 || np >= hist_n || chdir(hist[np]) < 0) return;
    hist_pos = np;
    clear_search();
    load();
}

static void open_row(int k) {
    const struct os_dirent *e = &entries[vis[k]];
    if (e->is_dir) {
        go(e->name);
        return;
    }
    char path[PATH_LEN + 64];
    snprintf(path, sizeof(path), "%s%s%s", cwd, cwd[strlen(cwd) - 1] == '/' ? "" : "/", e->name);
    char *argv[] = { is_picture(path) ? "view" : is_movie(path) ? "video" : is_program(path) ? "terminal" : "textedit",
                     path, 0 };
    spawn(argv[0], argv, 0);
}

/* ---- Layout (from the window's size: it can be resized) ----------------------- */

typedef struct {
    int sb;                               /* sidebar width, 0: hidden when narrow */
    int tb, hdr_h, hdr_y, list_y, list_h, bot_y, bot_h, rows;
    int nav_x, nav_y, nav_w, nav_h;       /* Back | Forward */
    int se_x, se_y, se_w, se_h;           /* the search field; se_w 0: no room */
    int kind_x, size_r;                   /* columns (kind_x = width: hidden) */
} layout_t;
static layout_t L;

static void layout(void) {
    ui_canvas *c = &win.c;
    int cw;
    L.sb = c->w >= 360 * S ? 168 * S : 0;
    cw = c->w - L.sb;
    L.tb = 44 * S;
    L.hdr_y = L.tb;
    L.hdr_h = 24 * S;
    L.bot_h = 28 * S;
    L.bot_y = c->h - L.bot_h;
    L.list_y = L.hdr_y + L.hdr_h;
    L.list_h = L.bot_y > L.list_y ? L.bot_y - L.list_y : 0;
    L.rows = L.list_h / ROW;
    L.nav_w = 60 * S;
    L.nav_h = 26 * S;
    L.nav_x = L.sb + 12 * S;
    L.nav_y = (L.tb - L.nav_h) / 2;
    int avail = c->w - 12 * S - (L.nav_x + L.nav_w + 16 * S);
    L.se_w = avail >= 96 * S ? (avail < 180 * S ? avail : 180 * S) : 0;
    L.se_h = 24 * S;
    L.se_x = c->w - 12 * S - L.se_w;
    L.se_y = (L.tb - L.se_h) / 2;
    int kind_w = 96 * S;
    L.kind_x = cw >= 420 * S ? c->w - 16 * S - kind_w : c->w;
    L.size_r = cw >= 300 * S ? (L.kind_x < c->w ? L.kind_x - 8 * S : c->w - 16 * S) : 0;
    int y = 14 * S;                                        /* the sidebar rows */
    for (int i = 0; i < nplaces; i++) {
        if (places[i].header && i) y += 8 * S;
        places[i].y = y;
        y += places[i].header ? 24 * S : 28 * S;
    }
}

static int hit_nav(int x, int y) {
    if (!ui_inside(x, y, L.nav_x, L.nav_y, L.nav_w, L.nav_h)) return 0;
    return x < L.nav_x + L.nav_w / 2 ? 1 : 2;
}
static int hit_search(int x, int y) { return L.se_w && ui_inside(x, y, L.se_x, L.se_y, L.se_w, L.se_h); }
static int hit_clear(int x, int y) {
    return qlen && ui_inside(x, y, L.se_x + L.se_w - 22 * S, L.se_y, 22 * S, L.se_h);
}
static int hit_place(int x, int y) {
    if (!L.sb || x >= L.sb) return -1;
    for (int i = 0; i < nplaces; i++)
        if (!places[i].header && y >= places[i].y && y < places[i].y + 28 * S) return i;
    return -1;
}

/* ---- Path bar ------------------------------------------------------------------ */

#define MAX_CRUMBS 10
static int n_crumbs;
static char crumb_path[MAX_CRUMBS][PATH_LEN], crumb_label[MAX_CRUMBS][40];
static int crumb_x0[MAX_CRUMBS], crumb_x1[MAX_CRUMBS];

static void build_crumbs(void) {
    char all_path[MAX_CRUMBS * 2][PATH_LEN], all_label[MAX_CRUMBS * 2][40];
    char built[PATH_LEN] = "", part[PATH_LEN];
    int n = 0;
    snprintf(all_path[n], PATH_LEN, "/");
    snprintf(all_label[n++], 40, "System Disk");
    const char *p = cwd;
    while (*p && n < MAX_CRUMBS * 2) {
        while (*p == '/') p++;
        if (!*p) break;
        size_t len = 0;
        while (p[len] && p[len] != '/' && len < PATH_LEN - 1) {
            part[len] = p[len];
            len++;
        }
        part[len] = 0;
        size_t used = strlen(built);
        snprintf(built + used, sizeof(built) - used, "/%s", part);      /* the folder's full path */
        snprintf(all_path[n], PATH_LEN, "%s", built);
        snprintf(all_label[n], 40, "%s", part);                         /* its name, cut to fit */
        n++;
        p += len;
    }
    int start = n > MAX_CRUMBS ? n - MAX_CRUMBS : 0;
    n_crumbs = 0;
    for (int i = start; i < n; i++) {
        snprintf(crumb_path[n_crumbs], PATH_LEN, "%s", all_path[i]);
        snprintf(crumb_label[n_crumbs], 40, "%s", all_label[i]);
        n_crumbs++;
    }
}

/* ---- Small drawing helpers -------------------------------------------------------- */

static void line(ui_canvas *c, int x0, int y0, int x1, int y1, int t, unsigned col) {
    int dx = x1 - x0, dy = y1 - y0, n = abs(dx) > abs(dy) ? abs(dx) : abs(dy);
    if (!n) n = 1;
    for (int i = 0; i <= n; i++) ui_fill(c, x0 + dx * i / n - t / 2, y0 + dy * i / n - t / 2, t, t, col);
}

/* A "<" (dir -1) or ">" (dir +1) centred on (cx, cy). */
static void chevron(ui_canvas *c, int cx, int cy, int dir, unsigned col) {
    int a = 3 * S, b = 5 * S;
    line(c, cx + dir * a, cy, cx - dir * a, cy - b, 2 * S, col);
    line(c, cx + dir * a, cy, cx - dir * a, cy + b, 2 * S, col);
}

static void magnifier(ui_canvas *c, int x, int cy, unsigned col, unsigned back) {
    int r = 5 * S, cx = x + r;
    ui_circle(c, cx, cy - S, r, col);
    ui_circle(c, cx, cy - S, r - S, back);
    line(c, cx + r * 7 / 10, cy - S + r * 7 / 10, cx + r + 3 * S, cy - S + r + 3 * S, S + 1, col);
}

/* ---- Drawing ------------------------------------------------------------------------ */

static void draw_sidebar(void) {
    ui_canvas *c = &win.c;
    if (!L.sb) return;
    ui_fill(c, 0, 0, L.sb, c->h, ui_color(UI_C_SIDEBAR));
    ui_fill(c, L.sb - 1, 0, 1, c->h, ui_color(UI_C_SEPARATOR));
    for (int i = 0; i < nplaces; i++) {
        place_t *p = &places[i];
        if (p->header) {
            ui_text_bold(c, 16 * S, p->y + (24 * S - 16 * S) / 2, p->label, ui_color(UI_C_SECONDARY), S);
            continue;
        }
        if (!strcmp(p->path, cwd))                         /* the place you are in */
            ui_round_ex(c, 8 * S, p->y, L.sb - 17 * S, 28 * S, UI_R_MEDIUM * S, ui_color(UI_C_LABEL),
                        ui_pref(UI_PREF_CONTRAST) ? 40 : 22, UI_ALL);
        if (ui_icon(c, 16 * S, p->y + 6 * S, 16 * S, p->icon, 255) < 0)
            ui_round(c, 16 * S, p->y + 6 * S, 16 * S, 16 * S, UI_R_SMALL * S, ui_color(UI_C_ACCENT));
        char t[48];
        fit(t, sizeof(t), p->label, L.sb - 16 * S - 40 * S);
        ui_text(c, 40 * S, p->y + (28 * S - 16 * S) / 2, t, ui_color(UI_C_LABEL), S);
    }
}

static void draw_toolbar(void) {
    ui_canvas *c = &win.c;
    ui_fill(c, L.sb, 0, c->w - L.sb, L.tb, ui_color(UI_C_TOOLBAR));
    ui_fill(c, L.sb, L.tb - 1, c->w - L.sb, 1, ui_color(UI_C_SEPARATOR));
    int r = UI_R_MEDIUM * S;                               /* Back | Forward, one segmented control */
    ui_round(c, L.nav_x, L.nav_y, L.nav_w, L.nav_h, r, ui_color(UI_C_CONTROL_EDGE));
    ui_round(c, L.nav_x + 1, L.nav_y + 1, L.nav_w - 2, L.nav_h - 2, r - 1, ui_color(UI_C_CONTROL));
    int half = L.nav_w / 2, can_back = hist_pos > 0, can_fwd = hist_pos < hist_n - 1;
    if (pressed == 1 && can_back)
        ui_round_ex(c, L.nav_x + 1, L.nav_y + 1, half - 1, L.nav_h - 2, r - 1, ui_color(UI_C_LABEL), 30, UI_TL | UI_BL);
    if (pressed == 2 && can_fwd)
        ui_round_ex(c, L.nav_x + half, L.nav_y + 1, half - 1, L.nav_h - 2, r - 1, ui_color(UI_C_LABEL), 30, UI_TR | UI_BR);
    ui_fill(c, L.nav_x + half, L.nav_y + 5 * S, 1, L.nav_h - 10 * S, ui_color(UI_C_CONTROL_EDGE));
    int cy = L.nav_y + L.nav_h / 2;
    chevron(c, L.nav_x + half / 2, cy, -1, ui_color(can_back ? UI_C_LABEL : UI_C_TERTIARY));
    chevron(c, L.nav_x + half + half / 2, cy, 1, ui_color(can_fwd ? UI_C_LABEL : UI_C_TERTIARY));

    if (!L.se_w) return;                                   /* the search field */
    int sr = UI_R_MEDIUM * S;
    if (searching) ui_focus_ring(c, L.se_x, L.se_y, L.se_w, L.se_h, sr, S);
    ui_round(c, L.se_x, L.se_y, L.se_w, L.se_h, sr, searching ? ui_color(UI_C_ACCENT) : ui_color(UI_C_CONTROL_EDGE));
    ui_round(c, L.se_x + 1, L.se_y + 1, L.se_w - 2, L.se_h - 2, sr - 1, ui_color(UI_C_FIELD));
    magnifier(c, L.se_x + 8 * S, L.se_y + L.se_h / 2, ui_color(UI_C_SECONDARY), ui_color(UI_C_FIELD));
    int tx = L.se_x + 26 * S, room = L.se_w - 26 * S - (qlen ? 24 * S : 8 * S), ty = L.se_y + (L.se_h - 16 * S) / 2;
    if (!qlen) {
        ui_text(c, tx, ty, "Search", ui_color(UI_C_TERTIARY), S);
        if (searching) ui_fill(c, tx, ty + S, S, 14 * S, ui_color(UI_C_ACCENT));
    } else {
        const char *shown = query;                         /* the end stays in view */
        while (*shown && ui_text_width(shown, S) > room) shown++;
        ui_clip(c, tx, L.se_y, room, L.se_h);
        int w = ui_text(c, tx, ty, shown, ui_color(UI_C_LABEL), S);
        if (searching) ui_fill(c, tx + w + S, ty + S, S, 14 * S, ui_color(UI_C_ACCENT));
        ui_noclip(c);
        int bx = L.se_x + L.se_w - 14 * S, by = L.se_y + L.se_h / 2;      /* the clear button */
        ui_circle(c, bx, by, 6 * S, ui_color(UI_C_TERTIARY));
        line(c, bx - 2 * S, by - 2 * S, bx + 2 * S, by + 2 * S, S, ui_color(UI_C_FIELD));
        line(c, bx - 2 * S, by + 2 * S, bx + 2 * S, by - 2 * S, S, ui_color(UI_C_FIELD));
    }
}

static void draw_list(void) {
    ui_canvas *c = &win.c;
    int x0 = L.sb, w = c->w - L.sb;
    ui_fill(c, x0, L.hdr_y, w, c->h - L.hdr_y, ui_color(UI_C_CONTENT));
    unsigned dim = ui_color(UI_C_SECONDARY);               /* the column headers */
    int hy = L.hdr_y + (L.hdr_h - 16 * S) / 2;
    ui_text(c, x0 + 40 * S, hy, "Name", dim, S);
    if (L.size_r) ui_text(c, L.size_r - ui_text_width("Size", S), hy, "Size", dim, S);
    if (L.kind_x < c->w) ui_text(c, L.kind_x, hy, "Kind", dim, S);
    ui_fill(c, x0, L.hdr_y + L.hdr_h - 1, w, 1, ui_color(UI_C_SEPARATOR));

    ui_clip(c, x0, L.list_y, w, L.list_h);
    int name_r = L.size_r ? L.size_r - ui_text_width("999.9 MB", S) - 12 * S : (L.kind_x < c->w ? L.kind_x : c->w) - 12 * S;
    if (L.size_r && name_r > L.size_r - 70 * S) name_r = L.size_r - 70 * S;
    for (int i = 0; i < L.rows + 1 && first + i < nvis; i++) {
        int k = first + i, ry = L.list_y + i * ROW;
        const struct os_dirent *e = &entries[vis[k]];
        int sel = k == selected;
        if (sel) ui_round(c, x0 + 8 * S, ry + S, w - 16 * S, ROW - 2 * S, UI_R_MEDIUM * S, ui_color(UI_C_ACCENT));
        else if (k % 2) ui_fill(c, x0, ry, w, ROW, ui_color(UI_C_ROW_ALT));
        unsigned ink = sel ? UI_WHITE : ui_color(UI_C_LABEL), sub = sel ? UI_WHITE : dim;
        ui_icon(c, x0 + 16 * S, ry + (ROW - 16 * S) / 2, 16 * S, icon_of(e), 255);
        char t[96];
        fit(t, sizeof(t), e->name, name_r - (x0 + 40 * S));
        ui_text(c, x0 + 40 * S, ry + (ROW - 16 * S) / 2, t, ink, S);
        if (L.size_r) {
            char sz[24];
            fmt_size(sz, sizeof(sz), e);
            ui_text(c, L.size_r - ui_text_width(sz, S), ry + (ROW - 16 * S) / 2, sz, sub, S);
        }
        if (L.kind_x < c->w) ui_text(c, L.kind_x, ry + (ROW - 16 * S) / 2, kind_of(e), sub, S);
    }
    if (!nvis) {
        const char *msg = qlen ? "No Results" : "This folder is empty";
        ui_text(c, x0 + (w - ui_text_width(msg, S)) / 2, L.list_y + 24 * S, msg, dim, S);
    }
    if (nvis > L.rows && L.rows > 0) {                     /* a thin overlay scroller */
        int th = L.list_h * L.rows / nvis;
        if (th < 24 * S) th = 24 * S;
        int ty = L.list_y + (L.list_h - th) * first / (nvis - L.rows);
        ui_round_ex(c, c->w - 9 * S, ty + S, 6 * S, th - 2 * S, 3 * S, ui_color(UI_C_LABEL),
                    ui_pref(UI_PREF_CONTRAST) ? 170 : 90, UI_ALL);
    }
    ui_noclip(c);
}

static void draw_path_bar(void) {
    ui_canvas *c = &win.c;
    int x0 = L.sb, w = c->w - L.sb;
    ui_fill(c, x0, L.bot_y, w, L.bot_h, ui_color(UI_C_WINDOW));
    ui_fill(c, x0, L.bot_y, w, 1, ui_color(UI_C_SEPARATOR));
    char st[32];
    if (qlen) snprintf(st, sizeof(st), "%d of %d items", nvis, count);
    else snprintf(st, sizeof(st), nvis == 1 ? "%d item" : "%d items", nvis);
    int sw = ui_text_width(st, S), ty = L.bot_y + (L.bot_h - 16 * S) / 2;
    ui_text(c, c->w - 12 * S - sw, ty, st, ui_color(UI_C_SECONDARY), S);

    build_crumbs();                                        /* folders, from the left; the tail if too long */
    int room = w - 12 * S - sw - 24 * S - 12 * S, sep = 16 * S, widths[MAX_CRUMBS], total = 0, start = 0;
    for (int i = 0; i < n_crumbs; i++) {
        widths[i] = 16 * S + 4 * S + ui_text_width(crumb_label[i], S);
        total += widths[i] + (i ? sep : 0);
    }
    while (start < n_crumbs - 1 && total > room) {
        total -= widths[start] + (start + 1 < n_crumbs ? sep : 0);
        start++;
    }
    for (int i = 0; i < MAX_CRUMBS; i++) crumb_x0[i] = crumb_x1[i] = -1;
    int x = x0 + 12 * S;
    ui_clip(c, x0, L.bot_y, w - sw - 24 * S, L.bot_h);
    for (int i = start; i < n_crumbs; i++) {
        if (i > start) {
            chevron(c, x + sep / 2, L.bot_y + L.bot_h / 2, 1, ui_color(UI_C_TERTIARY));
            x += sep;
        }
        int last = i == n_crumbs - 1;
        crumb_x0[i] = x;
        ui_icon(c, x, L.bot_y + (L.bot_h - 16 * S) / 2, 16 * S, i == 0 ? "computer" : "folder", 255);
        ui_text(c, x + 20 * S, ty, crumb_label[i], ui_color(last ? UI_C_LABEL : UI_C_SECONDARY), S);
        x += widths[i];
        crumb_x1[i] = x;
    }
    ui_noclip(c);
}

static void draw(void) {
    ui_canvas *c = &win.c;
    layout();
    ui_fill(c, 0, 0, c->w, c->h, ui_color(UI_C_CONTENT));
    draw_list();
    draw_toolbar();
    draw_path_bar();
    draw_sidebar();
    ui_present(&win);
}

/* ---- Input ----------------------------------------------------------------------------- */

static void select_row(int k) {
    if (k < 0 || k >= nvis) return;
    selected = k;
    if (selected >= first + L.rows) first = selected - L.rows + 1;
    if (selected < first) first = selected;
    if (first < 0) first = 0;
}

static void scroll(int d) {
    first += d;
    if (first > nvis - L.rows) first = nvis - L.rows;
    if (first < 0) first = 0;
}

static void type_to_select(int ch) {                       /* the next name that starts with it */
    for (int i = 1; i <= nvis; i++) {
        int k = (selected + i + nvis) % nvis;
        if (upper((unsigned char)entries[vis[k]].name[0]) == upper(ch)) {
            select_row(k);
            return;
        }
    }
}

static void key(int k) {
    if (searching) {
        if (k == 27) clear_search(), refilter();
        else if (k == '\n' || k == '\r') {
            searching = 0;
            if (nvis) select_row(0);
        } else if (k == '\b') {
            if (qlen) query[--qlen] = 0;
            refilter();
        } else if (k >= 32 && k < 127 && qlen < (int)sizeof(query) - 1) {
            query[qlen++] = (char)k;
            query[qlen] = 0;
            refilter();
        } else if (k == KEYCODE_DOWN) {
            select_row(selected < 0 ? 0 : selected + 1);
        } else if (k == KEYCODE_UP) {
            select_row(selected > 0 ? selected - 1 : 0);
        }
        return;
    }
    if (k == KEYCODE_DOWN) select_row(selected < 0 ? 0 : selected + 1);
    else if (k == KEYCODE_UP) select_row(selected > 0 ? selected - 1 : 0);
    else if ((k == '\n' || k == '\r') && selected >= 0) open_row(selected);
    else if (k == '\b') go("..");
    else if (k == KEY_CTRL_F) searching = 1;
    else if (k == 27 && qlen) clear_search(), refilter();
    else if (k > 32 && k < 127 && nvis) type_to_select(k);
}

int main(int argc, char **argv) {
    S = ui_scale();
    if (ui_open(&win, "Files", 640 * S, 420 * S) < 0) {
        printf("finder: the desktop is not running (type windowserver)\n");
        return 1;
    }
    ui_set_resizable(&win);
    struct os_user u;
    char home[64] = "/";
    if (getuser(&u) == 0 && u.home[0]) snprintf(home, sizeof(home), "%s", u.home);
    init_places(home);
    if (argc > 1) chdir(argv[1]);                           /* "finder /SOME/FOLDER" */
    else chdir(home);                                       /* like the Finder: start at Home */
    load();
    push_history();
    draw();
    unsigned long last_click = 0;
    int last_row = -1;
    for (;;) {
        struct ws_event e;
        ui_wait(&win, &e, 0);
        if (e.type == WS_EV_CLOSE) return 0;
        if (e.type == WS_EV_RESIZE || e.type == WS_EV_PREFS) {   /* a new size, or Dark / Contrast changed */
            draw();
            continue;
        }
        layout();
        if (e.type == WS_EV_WHEEL) {
            scroll(e.key);
        } else if (e.type == WS_EV_DOWN) {
            int nav = hit_nav(e.x, e.y), pl = hit_place(e.x, e.y);
            searching = hit_search(e.x, e.y);
            if (searching) {
                if (hit_clear(e.x, e.y)) clear_search(), refilter();
            } else if (nav) {
                pressed = nav;
            } else if (pl >= 0) {
                go(places[pl].path);
            } else if (e.x >= L.sb && e.y >= L.list_y && e.y < L.list_y + L.list_h) {
                int r = (e.y - L.list_y) / ROW + first;
                if (r >= 0 && r < nvis) {
                    unsigned long now = uptime_ms();
                    if (r == last_row && now - last_click < 500) open_row(r);
                    else selected = r;
                    last_row = r;
                    last_click = now;
                } else {
                    selected = -1;                          /* the empty part: nothing selected */
                }
            } else if (e.y >= L.bot_y && e.x >= L.sb) {
                for (int i = 0; i < n_crumbs; i++)
                    if (crumb_x0[i] >= 0 && e.x >= crumb_x0[i] && e.x < crumb_x1[i]) {
                        go(crumb_path[i]);
                        break;
                    }
            }
        } else if (e.type == WS_EV_UP) {
            int nav = hit_nav(e.x, e.y);
            if (pressed && nav == pressed) go_history(pressed == 1 ? -1 : 1);   /* released over it: do it */
            pressed = 0;
        } else if (e.type == WS_EV_KEY) {
            key(e.key);
        } else {
            continue;
        }
        draw();
    }
}