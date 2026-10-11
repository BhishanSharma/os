// finder - Files: browse folders in a window. Double-click a folder to go in,
// a file to open it (pictures in view, videos in video, the rest with cat in a Terminal).
#include <stdio.h>
#include <string.h>
#include <os.h>
#include <ui.h>

#define MAX_ENTRIES 128

static ui_window win;
static struct os_dirent entries[MAX_ENTRIES];
static char names[MAX_ENTRIES][64];
static const char *items[MAX_ENTRIES], *kinds[MAX_ENTRIES];
static int count, selected = -1, first;
static char cwd[256];
static int S, LIST_Y, BAR_H;

static int has_ending(const char *name, const char *end);

static const char *kind_of(const char *name) {
    if (has_ending(name, ".PNG") || has_ending(name, ".BMP") || has_ending(name, ".PPM") || has_ending(name, ".JPG"))
        return "picture";
    if (has_ending(name, ".MPG") || has_ending(name, ".MPEG")) return "video";
    if (has_ending(name, ".ELF")) return "terminal";
    return "file";
}

static void load(void) {
    getcwd(cwd, sizeof(cwd));
    int n = listdir(0, entries, MAX_ENTRIES), k = 0;
    for (int i = 0; i < n; i++) {
        if (!strcmp(entries[i].name, ".") || !strcmp(entries[i].name, "..")) continue;
        snprintf(names[k], sizeof(names[k]), "%s%s", entries[i].name, entries[i].is_dir ? "/" : "");
        if (k != i) entries[k] = entries[i];
        items[k] = names[k];
        kinds[k] = entries[k].is_dir ? "folder" : kind_of(entries[k].name);
        k++;
    }
    count = k;
    selected = -1;
    first = 0;
    ui_set_title(&win, cwd);
}

static void draw(void) {
    ui_canvas *c = &win.c;
    ui_fill(c, 0, 0, c->w, c->h, UI_WINDOW);
    ui_button(c, 8 * S, 6 * S, 56 * S, BAR_H - 12 * S, "Up", S, 0);
    ui_text(c, 76 * S, (BAR_H - 16 * S) / 2, cwd, UI_DIM, S);
    ui_fill(c, 0, BAR_H - 1, c->w, 1, UI_BORDER);
    ui_list_icons(c, 0, LIST_Y, c->w, c->h - LIST_Y, items, kinds, count, selected, first, S);
    if (!count) ui_text(c, 12 * S, LIST_Y + 8 * S, "(empty)", UI_DIM, S);
    ui_present(&win);
}

static int has_ending(const char *name, const char *end) {
    size_t n = strlen(name), e = strlen(end);
    if (n < e) return 0;
    for (size_t i = 0; i < e; i++) {
        char a = name[n - e + i], b = end[i];
        if (a >= 'a' && a <= 'z') a -= 32;
        if (a != b) return 0;
    }
    return 1;
}

static void open_entry(int k) {
    if (entries[k].is_dir) {
        chdir(entries[k].name);
        load();
        return;
    }
    char path[300];
    snprintf(path, sizeof(path), "%s%s%s", cwd, cwd[strlen(cwd) - 1] == '/' ? "" : "/", entries[k].name);
    int picture = has_ending(path, ".BMP") || has_ending(path, ".PNG") || has_ending(path, ".PPM");
    int movie = has_ending(path, ".MPG") || has_ending(path, ".MPEG");
    int program = has_ending(path, ".ELF");
    char *argv[] = { picture ? "view" : movie ? "video" : program ? "terminal" : "textedit", path, 0 };
    spawn(argv[0], argv, 0);
}

int main(void) {
    S = ui_scale();
    BAR_H = 34 * S;
    LIST_Y = BAR_H;
    if (ui_open(&win, "Files", 420 * S, 300 * S) < 0) {
        printf("finder: the desktop is not running (type windowserver)\n");
        return 1;
    }
    load();
    draw();
    unsigned long last_click = 0;
    int last_row = -1;
    for (;;) {
        struct ws_event e;
        ui_wait(&win, &e, 0);
        if (e.type == WS_EV_CLOSE) return 0;
        int rows = ui_list_rows(win.c.h - LIST_Y, S);
        if (e.type == WS_EV_WHEEL) {
            first += e.key;
            if (first > count - rows) first = count - rows;
            if (first < 0) first = 0;
        } else if (e.type == WS_EV_DOWN) {
            if (e.y < BAR_H) {
                if (ui_inside(e.x, e.y, 8 * S, 6 * S, 56 * S, BAR_H - 12 * S)) {
                    chdir("..");
                    load();
                }
            } else {
                int r = ui_list_hit(e.y - LIST_Y, S) + first;
                if (r >= 0 && r < count) {
                    unsigned long now = uptime_ms();
                    if (r == last_row && now - last_click < 500) open_entry(r);
                    else selected = r;
                    last_row = r;
                    last_click = now;
                }
            }
        } else if (e.type == WS_EV_KEY) {
            if (e.key == KEYCODE_DOWN && selected < count - 1) selected++;
            else if (e.key == KEYCODE_UP && selected > 0) selected--;
            else if ((e.key == '\n' || e.key == '\r') && selected >= 0) open_entry(selected);
            else if (e.key == '\b') { chdir(".."); load(); }
            if (selected >= first + rows) first = selected - rows + 1;
            if (selected >= 0 && selected < first) first = selected;
        } else {
            continue;
        }
        draw();
    }
}
