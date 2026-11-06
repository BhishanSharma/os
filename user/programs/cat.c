// cat [file]... - print files as they are, one after another; without files
// (or with "-"), what comes in (`echo hi | cat`). A name with spaces works
// without quotes (`cat my notes.txt`).
#include <stdio.h>
#include <string.h>
#include <os.h>
#include <args.h>

static int copy(int fd) {
    static char buf[8192];
    long n;
    while ((n = read(fd, buf, sizeof(buf))) > 0) write(1, buf, (size_t)n);
    return n < 0 ? 1 : 0;
}

static int cat(const char *name) {
    if (!strcmp(name, "-")) return copy(0);
    struct os_stat st;
    if (stat(name, &st) != 0) {
        printf("cat: %s: not found (no such file)\n", name);
        return 1;
    }
    if (st.is_dir) {
        printf("cat: %s: is a folder\n", name);
        return 1;
    }
    int fd = open(name, OPEN_READ);
    if (fd < 0) {
        printf("cat: %s: %s\n", name, fd == SYSERR_PERM ? "permission denied (only root can read it)" : os_strerror(fd));
        return 1;
    }
    int r = copy(fd);
    close(fd);
    return r;
}

int main(int argc, char **argv) {
    if (argc < 2) return copy(0);
    char joined[256];
    struct os_stat st;
    if (argc > 2 && stat(args_join(joined, sizeof(joined), argc, argv, 1), &st) == 0) return cat(joined);
    int status = 0;
    for (int i = 1; i < argc; i++) status |= cat(argv[i]);
    return status;
}
