// tree [folder] - a folder and everything in it, as a tree
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <os.h>
#include <args.h>

#define MAX_DEPTH   8
#define MAX_ENTRIES 256

static int folders, files;

/* The entries of `path`, drawn below `prefix` ("|   |   "). */
static void walk(const char *path, char *prefix, int depth) {
    struct os_dirent *e = malloc(MAX_ENTRIES * sizeof(*e));
    if (!e) return;
    int got = listdir(path, e, MAX_ENTRIES), n = 0;
    for (int i = 0; i < got; i++)                        /* not "." and ".." */
        if (strcmp(e[i].name, ".") && strcmp(e[i].name, "..")) e[n++] = e[i];
    for (int i = 0; i < n; i++) {
        int last = i == n - 1;
        printf("%s%s", prefix, last ? "`-- " : "|-- ");
        if (e[i].is_dir) {
            set_theme_color(THEME_ACCENT);
            printf("%s/\n", e[i].name);
            set_theme_color(THEME_TEXT);
            folders++;
            size_t plen = strlen(prefix);
            if (depth + 1 < MAX_DEPTH && plen + 5 < 64) {
                char sub[512];
                snprintf(sub, sizeof(sub), "%s/%s", strcmp(path, "/") ? path : "", e[i].name);
                strcpy(prefix + plen, last ? "    " : "|   ");
                walk(sub, prefix, depth + 1);
                prefix[plen] = 0;
            }
        } else {
            printf("%s\n", e[i].name);
            files++;
        }
    }
    free(e);
}

int main(int argc, char **argv) {
    char path[256], prefix[64] = "";
    args_join(path, sizeof(path), argc, argv, 1);
    if (!path[0]) getcwd(path, sizeof(path));
    struct os_stat st;
    if (stat(path, &st) != 0 || !st.is_dir) {
        printf("tree: %s: no such folder\n", path);
        return 1;
    }
    set_theme_color(THEME_ACCENT);
    printf("%s\n", path);
    set_theme_color(THEME_TEXT);
    walk(path, prefix, 0);
    printf("\n%d director%s, %d file%s\n", folders, folders == 1 ? "y" : "ies", files, files == 1 ? "" : "s");
    return 0;
}
