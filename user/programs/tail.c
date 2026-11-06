// tail [-n N | -N] [file]... - the last N lines (10) of files, or of what comes in
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <os.h>

static void tail(int fd, long lines) {
    size_t cap = 65536, len = 0;
    char *all = malloc(cap);
    if (!all) return;
    long n;
    for (;;) {
        if (len + 4096 > cap) {
            char *more = realloc(all, cap * 2);
            if (!more) break;
            all = more;
            cap *= 2;
        }
        if ((n = read(fd, all + len, 4096)) <= 0) break;
        len += (size_t)n;
    }
    size_t start = len;
    long seen = 0;
    if (start && all[start - 1] == '\n') start--;           /* (the last line's own newline) */
    while (start > 0) {
        if (all[start - 1] == '\n' && ++seen >= lines) break;
        start--;
    }
    if (lines <= 0) start = len;
    write(1, all + start, len - start);
    free(all);
}

int main(int argc, char **argv) {
    long lines = 10;
    int i = 1;
    for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
        if (!strcmp(argv[i], "-n") && i + 1 < argc) lines = atol(argv[++i]);
        else if (argv[i][1] == 'n') lines = atol(argv[i] + 2);
        else lines = atol(argv[i] + 1);
    }
    if (i >= argc) {
        tail(0, lines);
        return 0;
    }
    int status = 0, many = argc - i > 1;
    for (; i < argc; i++) {
        int fd = open(argv[i], OPEN_READ);
        if (fd < 0) {
            printf("tail: %s: %s\n", argv[i], os_strerror(fd));
            status = 1;
            continue;
        }
        if (many) printf("==> %s <==\n", argv[i]);
        tail(fd, lines);
        close(fd);
    }
    return status;
}
