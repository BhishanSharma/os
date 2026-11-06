// head [-n N | -N] [file]... - the first N lines (10) of files, or of what comes in
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <os.h>

static void head(int fd, long lines) {
    char buf[4096];
    long n, seen = 0;
    while (seen < lines && (n = read(fd, buf, sizeof(buf))) > 0) {
        long upto = 0;
        while (upto < n && seen < lines)
            if (buf[upto++] == '\n') seen++;
        write(1, buf, (size_t)upto);
    }
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
        head(0, lines);
        return 0;
    }
    int status = 0, many = argc - i > 1;
    for (; i < argc; i++) {
        int fd = open(argv[i], OPEN_READ);
        if (fd < 0) {
            printf("head: %s: %s\n", argv[i], os_strerror(fd));
            status = 1;
            continue;
        }
        if (many) printf("==> %s <==\n", argv[i]);
        head(fd, lines);
        close(fd);
    }
    return status;
}
