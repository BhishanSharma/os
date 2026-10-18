// tapp - install, remove and list apps from other people (docs/PACKAGES.md).
//
//   tapp FILE.tapp          a window: "Install NAME?" (Files opens packages with it)
//   tapp install FILE.tapp  the same, in Terminal
//   tapp remove ID          take an installed app away
//   tapp list               the installed apps
//
// A package is a tar archive (ustar) of flat files: app.cfg (id, name,
// version, author, color, icon), the program (named like the id) and, if it
// has one, icon.png. Apps go to the user's own ~/Apps/ID: no administrator
// needed, and they are that user's only. The window server's Dock shows them
// (it is told to read them again: WS_APPS).
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <os.h>
#include <view.h>
#include <ws.h>

#define MAX_FILES 32
#define MAX_PACKAGE (64 * 1024 * 1024)

typedef struct {
    char name[100];
    const unsigned char *data;
    unsigned long size;
} entry_t;

typedef struct {
    unsigned char *raw;               /* the whole file */
    entry_t files[MAX_FILES];
    int n;
    char id[16], name[48], version[24], author[48];
} package_t;

static char apps_dir[64];             /* ~/Apps */

static int set_apps_dir(void) {
    struct os_user u;
    if (getuser(&u) != 0) return -1;
    size_t n = strlen(u.home);
    snprintf(apps_dir, sizeof(apps_dir), "%s%sApps", u.home, n && u.home[n - 1] == '/' ? "" : "/");
    return 0;
}

/* 1-8 lower-case letters or digits, a letter first (like the built-in apps). */
static int valid_id(const char *id) {
    int n = (int)strlen(id);
    if (n < 1 || n > 8 || id[0] < 'a' || id[0] > 'z') return 0;
    for (int i = 0; i < n; i++)
        if (!((id[i] >= 'a' && id[i] <= 'z') || (id[i] >= '0' && id[i] <= '9'))) return 0;
    return 1;
}

static unsigned long octal(const unsigned char *p, int n) {
    unsigned long v = 0;
    for (int i = 0; i < n && p[i] >= '0' && p[i] <= '7'; i++) v = v * 8 + (unsigned long)(p[i] - '0');
    return v;
}

static const entry_t *find(const package_t *p, const char *name) {
    for (int i = 0; i < p->n; i++)
        if (!strcmp(p->files[i].name, name)) return &p->files[i];
    return 0;
}

/* "key = value  # comment" from app.cfg into out (empty if missing). */
static void cfg_value(const entry_t *cfg, const char *key, char *out, int max) {
    out[0] = 0;
    const char *s = (const char *)cfg->data, *end = s + cfg->size;
    int klen = (int)strlen(key);
    while (s < end) {
        const char *eol = s;
        while (eol < end && *eol != '\n') eol++;
        const char *p = s;
        while (p < eol && (*p == ' ' || *p == '\t')) p++;
        if (eol - p > klen && !strncmp(p, key, (size_t)klen)) {
            const char *q = p + klen;
            while (q < eol && (*q == ' ' || *q == '\t')) q++;
            if (q < eol && *q == '=') {
                q++;
                while (q < eol && (*q == ' ' || *q == '\t')) q++;
                const char *v = q, *ve = eol;
                for (const char *h = v; h + 1 < eol; h++)
                    if ((h[0] == ' ' || h[0] == '\t') && h[1] == '#') { ve = h; break; }
                while (ve > v && (ve[-1] == ' ' || ve[-1] == '\t' || ve[-1] == '\r')) ve--;
                int n = (int)(ve - v) < max - 1 ? (int)(ve - v) : max - 1;
                memcpy(out, v, (size_t)n);
                out[n] = 0;
                return;
            }
        }
        s = eol + 1;
    }
}

/* Read and check a package. -> 0, or -1 with *why set. */
static int load(const char *path, package_t *p, const char **why) {
    memset(p, 0, sizeof(*p));
    struct os_stat st;
    if (stat(path, &st) != 0 || st.is_dir) { *why = "the file cannot be read"; return -1; }
    if (st.size < 1024 || st.size > MAX_PACKAGE) { *why = "it is not a package (wrong size)"; return -1; }
    p->raw = malloc(st.size);
    int fd = p->raw ? open(path, OPEN_READ) : -1;
    if (fd < 0) { *why = "the file cannot be read"; return -1; }
    unsigned long got = 0;
    long r;
    while (got < st.size && (r = read(fd, p->raw + got, st.size - got)) > 0) got += (unsigned long)r;
    close(fd);
    if (got != st.size) { *why = "the file cannot be read"; return -1; }

    for (unsigned long off = 0; off + 512 <= got;) {
        const unsigned char *h = p->raw + off;
        if (!h[0]) break;                                  /* the end: zero blocks */
        if (memcmp(h + 257, "ustar", 5)) { *why = "it is not a package (not a tar archive)"; return -1; }
        unsigned long size = octal(h + 124, 12);
        char type = (char)h[156];
        char name[101];
        memcpy(name, h, 100);
        name[100] = 0;
        const char *n = !strncmp(name, "./", 2) ? name + 2 : name;
        off += 512;
        if (off + size > got) { *why = "the package is damaged (cut short)"; return -1; }
        if (type == '0' || type == 0) {
            if (!n[0] || strchr(n, '/') || n[0] == '.' || strstr(n, "..")) {
                *why = "the package has files in folders (only flat files are allowed)";
                return -1;
            }
            if (p->n == MAX_FILES) { *why = "the package has too many files"; return -1; }
            entry_t *e = &p->files[p->n++];
            snprintf(e->name, sizeof(e->name), "%s", n);
            e->data = p->raw + off;
            e->size = size;
        } else if (type != '5') {                          /* (folders, like "./", are skipped) */
            *why = "the package has links or special files";
            return -1;
        }
        off += (size + 511) / 512 * 512;
    }

    const entry_t *cfg = find(p, "app.cfg");
    if (!cfg) { *why = "it has no app.cfg"; return -1; }
    cfg_value(cfg, "id", p->id, sizeof(p->id));
    cfg_value(cfg, "name", p->name, sizeof(p->name));
    cfg_value(cfg, "version", p->version, sizeof(p->version));
    cfg_value(cfg, "author", p->author, sizeof(p->author));
    if (!valid_id(p->id)) { *why = "its id is not 1-8 lower-case letters or digits"; return -1; }
    if (!p->name[0]) snprintf(p->name, sizeof(p->name), "%s", p->id);
    const entry_t *prog = find(p, p->id);
    if (!prog || prog->size < 64 || memcmp(prog->data, "\x7F" "ELF", 4)) {
        *why = "it has no program for Terminal OS in it";
        return -1;
    }
    return 0;
}

/* Everything in ~/Apps/ID, then the folder. */
static int remove_dir(const char *id) {
    static struct os_dirent ents[MAX_FILES * 2];
    char dir[96], path[200];
    snprintf(dir, sizeof(dir), "%s/%s", apps_dir, id);
    struct os_stat st;
    if (stat(dir, &st) != 0 || !st.is_dir) return -1;
    int n = listdir(dir, ents, MAX_FILES * 2);
    for (int i = 0; i < n; i++) {
        if (!strcmp(ents[i].name, ".") || !strcmp(ents[i].name, "..")) continue;
        snprintf(path, sizeof(path), "%s/%s", dir, ents[i].name);
        unlink(path);
    }
    return unlink(dir) == 0 ? 0 : -1;
}

static void tell_dock(void) {
    int port = port_lookup(WS_SERVICE);
    if (port < 0) return;                              /* no desktop running */
    struct ws_request q;
    memset(&q, 0, sizeof(q));
    q.op = WS_APPS;
    q.pid = getpid();
    port_send(port, &q, sizeof(q), -1);
    port_close(port);
}

static int install(const package_t *p, const char **why) {
    char dir[96], path[200];
    mkdir(apps_dir);
    snprintf(dir, sizeof(dir), "%s/%s", apps_dir, p->id);
    struct os_stat st;
    if (stat(dir, &st) == 0) remove_dir(p->id);        /* an update: the old version goes */
    if (mkdir(dir) != 0) { *why = "the folder ~/Apps cannot be written"; return -1; }
    for (int i = 0; i < p->n; i++) {
        snprintf(path, sizeof(path), "%s/%s", dir, p->files[i].name);
        int fd = open(path, OPEN_WRITE);
        long w = fd >= 0 ? write(fd, p->files[i].data, p->files[i].size) : -1;
        if (fd >= 0 && close(fd) != 0) w = -1;
        if (w != (long)p->files[i].size) {
            remove_dir(p->id);
            *why = "the disk is full or cannot be written";
            return -1;
        }
    }
    tell_dock();
    return 0;
}

/* ---- The window: Install NAME? ---------------------------------------------- */

static package_t pkg;
static view_t *title, *detail, *cancel_btn, *install_btn;
static int installed;

static void cancel(view_t *b) {
    (void)b;
    v_quit(0);
}

static void do_install(view_t *b) {
    (void)b;
    if (installed) {                                   /* "Open" */
        char program[200];
        snprintf(program, sizeof(program), "%s/%s/%s", apps_dir, pkg.id, pkg.id);
        char *argv[] = { program, 0 };
        spawn(program, argv, 0);
        v_quit(0);
        return;
    }
    const char *why;
    char line[200];
    if (install(&pkg, &why) != 0) {
        snprintf(line, sizeof(line), "\"%s\" could not be installed: %s.", pkg.name, why);
        v_set_text(detail, line);
        return;
    }
    installed = 1;
    snprintf(line, sizeof(line), "%s is installed.", pkg.name);
    v_set_text(title, line);
    v_set_text(detail, "It is in the Dock. To remove it: tapp remove in Terminal.");
    v_set_text(cancel_btn, "Done");
    v_set_text(install_btn, "Open");
}

static int window(const char *path) {
    const char *why = 0;
    int ok = load(path, &pkg, &why) == 0;
    char head[120], line[240];
    if (ok) {
        snprintf(head, sizeof(head), "Install \"%s\"?", pkg.name);
        snprintf(line, sizeof(line), "%s%s%s%s. It goes in your Dock and runs as you: install apps "
                 "only from people you trust.",
                 pkg.version[0] ? "Version " : "", pkg.version, pkg.author[0] ? " by " : "",
                 pkg.author[0] ? pkg.author : (pkg.version[0] ? "" : "An app"));
    } else {
        snprintf(head, sizeof(head), "This package cannot be installed.");
        snprintf(line, sizeof(line), "Because %s.", why);
    }
    int S = ui_scale();
    ui_window w;
    if (ui_open(&w, "Install App", 460 * S, 180 * S) < 0) return 1;
    view_t *root = v_box(0, V_ROW);
    root->gap = 16;
    v_icon(root, "file", 56);
    view_t *words = v_box(root, V_COLUMN);
    words->flex = 1;
    title = v_label(words, head);
    title->style = UI_BOLD;
    detail = v_text(words, line);
    detail->color = UI_C_SECONDARY;
    v_spacer(words);
    view_t *buttons = v_box(words, V_ROW);
    v_spacer(buttons);
    cancel_btn = v_button(buttons, ok ? "Cancel" : "OK", cancel);
    if (ok) install_btn = v_button(buttons, "Install", do_install);
    (ok ? install_btn : cancel_btn)->flags = UI_BTN_DEFAULT;   /* Return */
    return v_run(&w, root, 0, 0);
}

/* ---- Terminal ------------------------------------------------------------------------ */

static int list(void) {
    static struct os_dirent ents[64];
    int n = listdir(apps_dir, ents, 64), shown = 0;
    for (int i = 0; i < n; i++) {
        if (!ents[i].is_dir || ents[i].name[0] == '.') continue;
        char path[200], cfg[1024];
        snprintf(path, sizeof(path), "%s/%s/app.cfg", apps_dir, ents[i].name);
        int fd = open(path, OPEN_READ);
        long len = fd >= 0 ? read(fd, cfg, sizeof(cfg)) : -1;
        if (fd >= 0) close(fd);
        entry_t e = { "app.cfg", (const unsigned char *)cfg, len > 0 ? (unsigned long)len : 0 };
        char name[48], version[24];
        cfg_value(&e, "name", name, sizeof(name));
        cfg_value(&e, "version", version, sizeof(version));
        printf("  %-8s  %s%s%s\n", ents[i].name, name[0] ? name : ents[i].name, version[0] ? "  " : "", version);
        shown++;
    }
    if (!shown) printf("No apps installed (they go in %s).\n", apps_dir);
    return 0;
}

static int usage(void) {
    printf("tapp FILE.tapp          install it (a window asks first)\n"
           "tapp install FILE.tapp  install it\n"
           "tapp remove ID          remove an installed app\n"
           "tapp list               the installed apps\n");
    return 1;
}

int main(int argc, char **argv) {
    if (set_apps_dir() != 0) {
        printf("tapp: who is this? (no user)\n");
        return 1;
    }
    if (argc < 2) return usage();
    if (!strcmp(argv[1], "list")) return list();
    if (!strcmp(argv[1], "remove") && argc == 3) {
        if (!valid_id(argv[2]) || remove_dir(argv[2]) != 0) {
            printf("tapp: no app \"%s\" is installed (`tapp list`)\n", argv[2]);
            return 1;
        }
        tell_dock();
        printf("Removed %s.\n", argv[2]);
        return 0;
    }
    if (!strcmp(argv[1], "install") && argc == 3) {
        const char *why;
        if (load(argv[2], &pkg, &why) != 0 || install(&pkg, &why) != 0) {
            printf("tapp: %s cannot be installed: %s\n", argv[2], why);
            return 1;
        }
        printf("Installed %s%s%s (%s): it is in the Dock.\n", pkg.name, pkg.version[0] ? " " : "", pkg.version, pkg.id);
        return 0;
    }
    if (argc == 2 && argv[1][0] != '-') return window(argv[1]);
    return usage();
}
