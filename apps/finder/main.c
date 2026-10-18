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
// it (pictures in view, videos in video, songs in music, programs in terminal,
// text in textedit; others say no app can open them). Backspace goes up a folder; typing a letter selects the next
// name that starts with it. Colours are semantic tokens (ui_color), so the
// window follows Dark and Increase Contrast.
//
// Built from views (view.h): the sidebar and the list are tables (icons,
// headings), the toolbar has buttons and a search field, the path bar is a
// row of buttons made again for each folder. Changing folder or typing in the
// search only changes their data; the views redraw what differs.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <os.h>
#include <view.h>

#define MAX_ENTRIES 128
#define PATH_LEN 256
#define HIST 32
#define KEY_CTRL_F 6

static ui_window win;
static struct os_dirent entries[MAX_ENTRIES];
static int count;                         /* entries in the folder */
static int sorted[MAX_ENTRIES];           /* entry numbers, by name */
static int vis[MAX_ENTRIES], nvis;        /* the ones the search shows */
static int selected = -1;                 /* a row of vis */
static char cwd[PATH_LEN];
static char query[48];
static int qlen;                          /* the search text */
static char hist[HIST][PATH_LEN];
static int hist_n, hist_pos;
static int S;
static view_t *sidebar, *list, *search, *back_btn, *fwd_btn, *path_bar, *count_lbl;

/* ---- Places (the sidebar) ------------------------------------------------- */

typedef struct {
    const char *label, *icon;
    char path[64];
    int header;
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

static int ends_in(const char *n, const char *const *list) {
    for (; *list; list++)
        if (has_ending(n, *list)) return 1;
    return 0;
}

/* Which app opens what (by the name's ending; unknown files by their bytes). */
static const char *const pictures[] = { ".PNG", ".JPG", ".JPEG", ".GIF", ".BMP", ".TGA", ".PSD", ".PPM", ".PGM", 0 };
static const char *const movies[] = { ".MPG", ".MPEG", ".MP4", ".MOV", ".MKV", ".AVI", ".WEBM", ".M4V", 0 };
static const char *const songs[] = { ".MP3", ".MP2", ".WAV", ".M4A", ".AAC", ".OGG", ".FLAC", ".WMA", 0 };
static const char *const texts[] = { ".TXT", ".MD", ".C", ".H", ".CFG", ".INI", ".LOG", ".CSV", ".JSON",
                                     ".XML", ".HTML", ".CSS", ".JS", ".PY", ".SH", ".ASM", ".S", ".CPP", 0 };
static int is_picture(const char *n) { return ends_in(n, pictures); }
static int is_movie(const char *n) { return ends_in(n, movies); }
static int is_song(const char *n) { return ends_in(n, songs); }
static int is_text(const char *n) { return ends_in(n, texts); }
static int is_program(const char *n) { return has_ending(n, ".ELF"); }
static int is_package(const char *n) { return has_ending(n, ".TAPP"); }   /* an app to install (tapp) */

/* No known ending: text if its first 4 KB look like text (no NUL, few control bytes). */
static int looks_like_text(const char *path) {
    int fd = open(path, OPEN_READ);
    if (fd < 0) return 0;
    unsigned char buf[4096];
    long n = read(fd, buf, sizeof(buf));
    close(fd);
    if (n < 0) return 0;
    int odd = 0;
    for (long i = 0; i < n; i++) {
        if (!buf[i]) return 0;
        if (buf[i] < 32 && buf[i] != 10 && buf[i] != 13 && buf[i] != 9) odd++;
    }
    return odd * 50 <= n;
}

static const char *icon_of(const struct os_dirent *e) {
    if (e->is_dir) return "folder";
    if (is_picture(e->name)) return "picture";
    if (is_movie(e->name)) return "video";
    if (is_song(e->name)) return "music";
    if (is_program(e->name)) return "terminal";
    return "file";
}

static const char *kind_of(const struct os_dirent *e) {
    if (e->is_dir) return "Folder";
    if (is_program(e->name)) return "Program";
    if (is_package(e->name)) return "App Package";
    if (is_picture(e->name)) return "Image";
    if (is_movie(e->name)) return "Movie";
    if (is_song(e->name)) return "Audio";
    if (is_text(e->name)) return "Plain Text";
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

/* ---- The folder -------------------------------------------------------------- */

static void show_count(void) {
    char st[48];
    if (qlen) snprintf(st, sizeof(st), "%d of %d items", nvis, count);
    else snprintf(st, sizeof(st), nvis == 1 ? "%d item" : "%d items", nvis);
    v_set_text(count_lbl, st);
}

static void refilter(void) {
    nvis = 0;
    for (int i = 0; i < count; i++)
        if (!qlen || contains_ci(entries[sorted[i]].name, query)) vis[nvis++] = sorted[i];
    selected = -1;
    list->first_row = 0;
    v_dirty(list);
    v_table_rows(list, nvis);
    v_table_select(list, -1);
    show_count();
}

static void show_path(void);
static void show_history(void);

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
    show_path();
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
    show_history();
}

static void clear_search(void) {
    qlen = 0;
    query[0] = 0;
    v_set_text(search, "");
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
    show_history();
}

static void open_row(int k) {
    const struct os_dirent *e = &entries[vis[k]];
    if (e->is_dir) {
        go(e->name);
        return;
    }
    char path[PATH_LEN + 64];
    snprintf(path, sizeof(path), "%s%s%s", cwd, cwd[strlen(cwd) - 1] == '/' ? "" : "/", e->name);
    char *app = is_package(path) ? "tapp" : is_picture(path) ? "view" : is_movie(path) ? "video" : is_song(path) ? "music"
              : is_program(path) ? "terminal" : is_text(path) || looks_like_text(path) ? "textedit" : 0;
    if (!app) {                                     /* not text, and no app knows it */
        char line[96];
        snprintf(line, sizeof(line), "No app can open \"%s\"", e->name);
        v_set_text(count_lbl, line);
        return;
    }
    char *argv[] = { app, path, 0 };
    spawn(argv[0], argv, 0);
}

#define MAX_CRUMBS 10
static int n_crumbs;
static char crumb_path[MAX_CRUMBS][PATH_LEN], crumb_label[MAX_CRUMBS][40];

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


/* ---- What the views show -------------------------------------------------------- */

static void crumb_clicked(view_t *b) { go(crumb_path[(int)(long)b->data]); }

static void show_path(void) {                              /* the path bar: a button a folder */
    build_crumbs();
    v_clear(path_bar);
    for (int i = 0; i < n_crumbs; i++) {
        view_t *b = v_button(path_bar, crumb_label[i], crumb_clicked);
        b->data = (void *)(long)i;
        b->want_w = ui_text_width(crumb_label[i], S) / S + 20;
        b->want_h = 24;
    }
    v_spacer(path_bar);
    count_lbl = v_label(path_bar, "");
    count_lbl->color = UI_C_SECONDARY;
    show_count();
}

static void show_history(void) {
    v_set_flags(back_btn, hist_pos > 0 ? 0 : UI_BTN_DISABLED);
    v_set_flags(fwd_btn, hist_pos + 1 < hist_n ? 0 : UI_BTN_DISABLED);
}

static void place_cell(view_t *t, int row, int col, char *out, int max) {
    (void)t;
    (void)col;
    snprintf(out, max, "%s", places[row].label);
}

static const char *place_icon(view_t *t, int row) {
    (void)t;
    return places[row].icon;
}

static int place_header(view_t *t, int row) {
    (void)t;
    return places[row].header;
}

static void place_picked(view_t *t, int row) {
    (void)t;
    go(places[row].path);
}

static void entry_cell(view_t *t, int row, int col, char *out, int max) {
    (void)t;
    const struct os_dirent *e = &entries[vis[row]];
    if (col == 0) snprintf(out, max, "%s", e->name);
    else if (col == 1) fmt_size(out, (size_t)max, e);
    else snprintf(out, max, "%s", kind_of(e));
}

static const char *entry_icon(view_t *t, int row) {
    (void)t;
    return icon_of(&entries[vis[row]]);
}

static void entry_picked(view_t *t, int row) {
    (void)t;
    selected = row;
}

static void entry_opened(view_t *t, int row) {
    (void)t;
    open_row(row);
}

static void back_clicked(view_t *b) { (void)b; go_history(-1); }
static void fwd_clicked(view_t *b) { (void)b; go_history(1); }

static void search_changed(view_t *f) {                   /* filters as you type */
    snprintf(query, sizeof(query), "%s", f->text);
    qlen = (int)strlen(query);
    refilter();
}

static void search_entered(view_t *f) {
    (void)f;
    if (nvis) {
        v_table_select(list, 0);
        selected = 0;
    }
    v_focus(list);
}

static void type_to_select(int ch) {                       /* the next name that starts with it */
    for (int i = 1; i <= nvis; i++) {
        int k = (selected + i + nvis) % nvis;
        if (upper((unsigned char)entries[vis[k]].name[0]) == upper(ch)) {
            selected = k;
            v_table_select(list, k);
            return;
        }
    }
}

/* Keys nothing else took: the folder's shortcuts. */
static int window_key(view_t *v, int k) {
    (void)v;
    if (k == KEY_CTRL_F) v_focus(search);
    else if (k == 27) {
        clear_search();
        refilter();
    } else if (k == '\b') go("..");
    else if (k > 32 && k < 127 && nvis) type_to_select(k);
    else return 0;
    return 1;
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

    view_t *root = v_box(0, V_ROW);
    root->pad = root->gap = 0;
    root->on_key = window_key;
    sidebar = v_table(root, 1, 0, 0, place_cell);
    sidebar->flags = V_INSET;
    sidebar->flex = 0;
    sidebar->want_w = 168;
    sidebar->row_h = 30;
    sidebar->bg = UI_C_SIDEBAR;
    sidebar->row_icon = place_icon;
    sidebar->row_header = place_header;
    sidebar->on_select = place_picked;
    v_table_rows(sidebar, nplaces);
    view_t *main_col = v_box(root, V_COLUMN);
    main_col->flex = 1;
    main_col->pad = main_col->gap = 0;
    view_t *bar = v_box(main_col, V_ROW);
    bar->pad = 8;
    bar->gap = 4;
    bar->bg = UI_C_TOOLBAR;
    bar->align = V_CENTER;
    back_btn = v_button(bar, "<", back_clicked);
    fwd_btn = v_button(bar, ">", fwd_clicked);
    v_spacer(bar);
    search = v_field(bar, "", search_entered);
    search->want_w = 200;
    search->on_change = search_changed;
    snprintf(search->hint, sizeof(search->hint), "Search");
    static const char *heads[] = { "Name", "Size", "Kind" };
    static const int widths[] = { 230, 90, 100 };
    list = v_table(main_col, 3, heads, widths, entry_cell);
    list->row_icon = entry_icon;
    list->on_select = entry_picked;
    list->on_open = entry_opened;
    path_bar = v_box(main_col, V_ROW);
    path_bar->pad = 4;
    path_bar->gap = 2;
    path_bar->align = V_CENTER;

    if (argc > 1) chdir(argv[1]);                           /* "finder /SOME/FOLDER" */
    else chdir(home);                                       /* like the Finder: start at Home */
    load();
    push_history();
    v_focus(list);
    return v_run(&win, root, 0, 0);
}
