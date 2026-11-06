// sort [-r] [-n] [-u] [file]... - the lines in order (-r backwards, -n by
// number, -u each once); without files, what comes in.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <os.h>

static int reverse, numeric, unique;
static char *all;
static size_t len, cap;
static char **lines;
static long n_lines;

static void slurp(int fd) {
    long n;
    for (;;) {
        if (len + 4097 > cap) {
            size_t want = cap ? cap * 2 : 65536;
            char *more = realloc(all, want);
            if (!more) return;
            all = more;
            cap = want;
        }
        if ((n = read(fd, all + len, 4096)) <= 0) return;
        len += (size_t)n;
    }
}

static int cmp(const void *a, const void *b) {
    const char *x = *(char *const *)a, *y = *(char *const *)b;
    int r;
    if (numeric) {
        long long dx = atoll(x), dy = atoll(y);              /* (programs here have no floating point) */
        r = dx < dy ? -1 : dx > dy ? 1 : strcmp(x, y);
    } else {
        r = strcmp(x, y);
    }
    return reverse ? -r : r;
}

int main(int argc, char **argv) {
    int i = 1;
    for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++)
        for (const char *f = argv[i] + 1; *f; f++) {
            if (*f == 'r') reverse = 1;
            else if (*f == 'n') numeric = 1;
            else if (*f == 'u') unique = 1;
        }
    if (i >= argc) slurp(0);
    for (; i < argc; i++) {
        int fd = open(argv[i], OPEN_READ);
        if (fd < 0) {
            printf("sort: %s: %s\n", argv[i], os_strerror(fd));
            return 1;
        }
        slurp(fd);
        close(fd);
        if (len && all[len - 1] != '\n') all[len++] = '\n';
    }
    if (!len) return 0;
    if (all[len - 1] != '\n') all[len++] = '\n';
    for (size_t k = 0; k < len; k++)
        if (all[k] == '\n') n_lines++;
    lines = malloc((size_t)n_lines * sizeof(char *));
    if (!lines) return 1;
    long at = 0;
    char *start = all;
    for (size_t k = 0; k < len; k++)
        if (all[k] == '\n') {
            all[k] = 0;
            lines[at++] = start;
            start = all + k + 1;
        }
    qsort(lines, (size_t)n_lines, sizeof(char *), cmp);
    for (long k = 0; k < n_lines; k++) {
        if (unique && k && !strcmp(lines[k], lines[k - 1])) continue;
        printf("%s\n", lines[k]);
    }
    return 0;
}
