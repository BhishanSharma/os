// grep [-i] [-n] [-v] [-c] [-r] <pattern> [file]... - the lines that match
//
// The pattern: text, with ^ (the line's start), $ (its end), . (any
// character) and * (the one before, any number of times). -i ignores case,
// -n numbers the lines, -v shows those that do not match, -c only counts,
// -r looks through folders. Without files, what comes in (`ls | grep txt`).
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <os.h>

static int icase, number, invert, count_only, recursive, many, found_any;

static int same(char a, char b) {
    if (icase) {
        if (a >= 'A' && a <= 'Z') a += 32;
        if (b >= 'A' && b <= 'Z') b += 32;
    }
    return a == b;
}

/* Kernighan and Pike's matcher (The Practice of Programming, 9.2). */
static int match_here(const char *re, const char *s);
static int match_star(char c, const char *re, const char *s) {
    do {
        if (match_here(re, s)) return 1;
    } while (*s && (c == '.' || same(*s, c)) && s++);
    return 0;
}
static int match_here(const char *re, const char *s) {
    if (!re[0]) return 1;
    if (re[1] == '*') return match_star(re[0], re + 2, s);
    if (re[0] == '$' && !re[1]) return !*s;
    if (*s && (re[0] == '.' || same(re[0], *s))) return match_here(re + 1, s + 1);
    return 0;
}
static int match(const char *re, const char *s) {
    if (re[0] == '^') return match_here(re + 1, s);
    do {
        if (match_here(re, s)) return 1;
    } while (*s++);
    return 0;
}

static long lineno, hits;

static void check(char *line, long len, const char *re, const char *name) {
    line[len] = 0;
    if (len && line[len - 1] == '\r') line[len - 1] = 0;
    lineno++;
    if (match(re, line) == invert) return;
    hits++;
    if (count_only) return;
    if (many) printf("%s:", name);
    if (number) printf("%ld:", lineno);
    printf("%s\n", line);
}

static void grep_fd(const char *re, int fd, const char *name) {
    static char line[4096];
    char buf[4096];
    long n, len = 0;
    lineno = hits = 0;
    while ((n = read(fd, buf, sizeof(buf))) > 0) {
        for (long k = 0; k < n; k++) {
            if (buf[k] != '\n') {
                if (len < (long)sizeof(line) - 1) line[len++] = buf[k];
                continue;
            }
            check(line, len, re, name);
            len = 0;
        }
    }
    if (len) check(line, len, re, name);
    if (count_only) {
        if (many) printf("%s:", name);
        printf("%ld\n", hits);
    }
    if (hits) found_any = 1;
}

static void grep_path(const char *re, const char *path, int depth) {
    struct os_stat st;
    if (stat(path, &st) != 0) {
        printf("grep: %s: no such file\n", path);
        return;
    }
    if (st.is_dir) {
        if (!recursive) {
            printf("grep: %s: is a folder (grep -r looks inside)\n", path);
            return;
        }
        if (depth > 16) return;
        struct os_dirent *e = malloc(512 * sizeof(*e));
        if (!e) return;
        int n = listdir(path, e, 512);
        for (int i = 0; i < n; i++) {
            if (!strcmp(e[i].name, ".") || !strcmp(e[i].name, "..")) continue;
            char sub[512];
            snprintf(sub, sizeof(sub), "%s/%s", path, e[i].name);
            grep_path(re, sub, depth + 1);
        }
        free(e);
        return;
    }
    int fd = open(path, OPEN_READ);
    if (fd < 0) {
        printf("grep: %s: %s\n", path, os_strerror(fd));
        return;
    }
    grep_fd(re, fd, path);
    close(fd);
}

int main(int argc, char **argv) {
    int i = 1;
    for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++)
        for (const char *f = argv[i] + 1; *f; f++) {
            if (*f == 'i') icase = 1;
            else if (*f == 'n') number = 1;
            else if (*f == 'v') invert = 1;
            else if (*f == 'c') count_only = 1;
            else if (*f == 'r' || *f == 'R') recursive = 1;
        }
    if (i >= argc) {
        printf("usage: grep [-i] [-n] [-v] [-c] [-r] <pattern> [file]...\n");
        return 2;
    }
    const char *re = argv[i++];
    if (i >= argc) {
        if (recursive) {
            many = 1;
            grep_path(re, ".", 0);
        } else {
            grep_fd(re, 0, "");
        }
        return found_any ? 0 : 1;
    }
    many = argc - i > 1 || recursive;
    for (; i < argc; i++) grep_path(re, argv[i], 0);
    return found_any ? 0 : 1;
}
