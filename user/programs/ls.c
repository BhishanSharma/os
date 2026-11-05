// ls [-l] [folder] - list a folder: folders first, then files with their sizes
//
// -l also shows each one's permissions, owner and when it last changed (on
// FAT32 volumes, which keep no owners: rwxr-xr-x folders, rw-r--r-- files, root).
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

/* -l: "drwxr-x---  alice  2026-11-05 14:02" */
static void long_info(const char *dir, const char *name, int local_offset) {
    char path[600];
    snprintf(path, sizeof(path), "%s%s%s", dir, dir[0] && dir[strlen(dir) - 1] != '/' ? "/" : "", name);
    struct os_statx st;
    if (statx(path, &st) != 0) {
        printf("  ?????????? %-8s %16s ", "", "");
        return;
    }
    char m[11] = "-rwxrwxrwx";
    if (st.is_dir) m[0] = 'd';
    for (int i = 0; i < 9; i++)
        if (!(st.mode & (0400 >> i))) m[1 + i] = '-';
    if (st.mode & 01000) m[9] = st.mode & 1 ? 't' : 'T';
    struct os_user me;
    char owner[16];
    getuser(&me);
    if (st.uid == 0) snprintf(owner, sizeof(owner), "root");
    else if (st.uid == me.uid) snprintf(owner, sizeof(owner), "%s", me.name);
    else snprintf(owner, sizeof(owner), "%u", st.uid);
    char when[20] = "";
    if (st.mtime) {                                      /* days since 1970 -> a date (Howard Hinnant's) */
        long t = (long)st.mtime + local_offset * 60L, z = t / 86400 + 719468, secs = t % 86400;
        long era = z / 146097, doe = z - era * 146097, yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
        long doy = doe - (365 * yoe + yoe / 4 - yoe / 100), mp = (5 * doy + 2) / 153;
        long d = doy - (153 * mp + 2) / 5 + 1, mo = mp < 10 ? mp + 3 : mp - 9, y = yoe + era * 400 + (mo <= 2);
        snprintf(when, sizeof(when), "%04ld-%02ld-%02ld %02ld:%02ld", y, mo, d, secs / 3600, secs / 60 % 60);
    }
    printf("  %s %-8s %16s ", m, owner, when);
}

/* Minutes local time is ahead of UTC. */
static int local_offset(void) {
    struct os_time l, u;
    if (gettime(&l) != 0 || gettime_utc(&u) != 0) return 0;
    int d = l.day != u.day ? (l.day == u.day + 1 || l.day == 1 ? 1440 : -1440) : 0;
    return d + (l.hour - u.hour) * 60 + (l.minute - u.minute);
}

int main(int argc, char **argv) {
    char path[256];
    int longer = argc > 1 && !strcmp(argv[1], "-l"), offset = longer ? local_offset() : 0;
    args_join(path, sizeof(path), argc, argv, 1 + longer);   /* ls My Folder */
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
        if (longer) long_info(path, e[i].name, offset);
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
