// ls [folder] - list a folder: folders first, then files with their sizes
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <os.h>
#include <args.h>

#define MAX_ENTRIES 1024

/* "71 B", "31.7 KiB", "2.4 MiB" */
static void format_size(char *out, size_t size, unsigned long bytes) {
    if (bytes < 1024)
        snprintf(out, size, "%lu B", bytes);
    else if (bytes < 1024 * 1024)
        snprintf(out, size, "%lu.%lu KiB", bytes / 1024, (bytes % 1024) * 10 / 1024);
    else
        snprintf(out, size, "%lu.%lu MiB", bytes >> 20, (bytes % (1024 * 1024)) * 10 / (1024 * 1024));
}

static int lower(int c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }

/* Folders first, then by name, ignoring case. */
static int before(const struct os_dirent *a, const struct os_dirent *b) {
    if (a->is_dir != b->is_dir) return a->is_dir;
    const char *p = a->name, *q = b->name;
    while (*p && lower(*p) == lower(*q)) p++, q++;
    return lower(*p) < lower(*q);
}

static void sort(struct os_dirent *e, int n) {
    struct os_dirent tmp;
    for (int i = 1; i < n; i++) {
        int j = i;
        memcpy(&tmp, &e[i], sizeof(tmp));
        while (j > 0 && before(&tmp, &e[j - 1])) {
            memcpy(&e[j], &e[j - 1], sizeof(tmp));
            j--;
        }
        memcpy(&e[j], &tmp, sizeof(tmp));
    }
}

static void pad(int from, int to) {
    for (int i = from; i < to; i++) putchar(' ');
}

int main(int argc, char **argv) {
    char path[256];
    args_join(path, sizeof(path), argc, argv, 1);        /* ls My Folder */
    struct os_dirent *e = malloc(MAX_ENTRIES * sizeof(*e));
    if (!e) {
        printf("ls: out of memory\n");
        return 1;
    }
    int n = listdir(path[0] ? path : 0, e, MAX_ENTRIES);
    if (n < 0) {
        printf("ls: %s: no such folder\n", path[0] ? path : ".");
        return 1;
    }
    int kept = 0;
    for (int i = 0; i < n; i++)                          /* not "." and ".." */
        if (strcmp(e[i].name, ".") && strcmp(e[i].name, "..")) e[kept++] = e[i];
    n = kept;
    if (n == 0) {
        printf("(empty)\n");
        return 0;
    }
    sort(e, n);

    int width = 16, dirs = 0, files = 0;
    unsigned long total = 0;
    for (int i = 0; i < n; i++) {                        /* the name column fits the longest */
        int len = (int)strlen(e[i].name) + 1;
        if (len > width) width = len > 48 ? 48 : len;
    }
    char size[16];
    for (int i = 0; i < n; i++) {
        int len = (int)strlen(e[i].name);
        if (e[i].is_dir) {
            set_theme_color(THEME_ACCENT);
            printf("  %s/", e[i].name);
            set_theme_color(THEME_TEXT);
            pad(len + 1, width);
            printf("     <DIR>\n");
            dirs++;
        } else {
            format_size(size, sizeof(size), e[i].size);
            printf("  %s", e[i].name);
            pad(len, width);
            printf("%10s\n", size);
            files++;
            total += e[i].size;
        }
    }
    format_size(size, sizeof(size), total);
    printf("\n  %d file%s, %d director%s, %s\n", files, files == 1 ? "" : "s",
           dirs, dirs == 1 ? "y" : "ies", size);
    return 0;
}
