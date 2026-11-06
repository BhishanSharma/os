// cp [-r] <from>... <to> - copy files; into <to> if it is a folder. With -r
// folders too, with everything in them.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <os.h>

static int recursive, failed;

static const char *base_name(const char *p) {
    const char *s = strrchr(p, '/');
    return s && s[1] ? s + 1 : p;
}

static void copy_file(const char *from, const char *to) {
    int in = open(from, OPEN_READ);
    if (in < 0) {
        printf("cp: %s: %s\n", from, os_strerror(in));
        failed = 1;
        return;
    }
    int out = open(to, OPEN_WRITE);
    if (out < 0) {
        printf("cp: %s: %s\n", to, os_strerror(out));
        close(in);
        failed = 1;
        return;
    }
    static char buf[16384];
    long n;
    while ((n = read(in, buf, sizeof(buf))) > 0) {
        if (write(out, buf, (size_t)n) != n) {
            printf("cp: %s: could not write it all (is the disk full?)\n", to);
            failed = 1;
            break;
        }
    }
    if (n < 0) {
        printf("cp: %s: %s\n", from, os_strerror((int)n));
        failed = 1;
    }
    close(in);
    close(out);
}

static void copy_path(const char *from, const char *to, int depth) {
    struct os_stat st;
    if (stat(from, &st) != 0) {
        printf("cp: %s: no such file or folder\n", from);
        failed = 1;
        return;
    }
    if (!st.is_dir) {
        copy_file(from, to);
        return;
    }
    if (!recursive) {
        printf("cp: %s: is a folder (cp -r copies it and what is in it)\n", from);
        failed = 1;
        return;
    }
    if (depth > 32) {
        printf("cp: %s: folders too deep\n", from);
        failed = 1;
        return;
    }
    struct os_stat there;
    if (stat(to, &there) != 0 && mkdir(to) < 0) {
        printf("cp: %s: cannot make the folder\n", to);
        failed = 1;
        return;
    }
    struct os_dirent *e = malloc(1024 * sizeof(*e));
    if (!e) return;
    int n = listdir(from, e, 1024);
    for (int i = 0; i < n; i++) {
        if (!strcmp(e[i].name, ".") || !strcmp(e[i].name, "..")) continue;
        char a[512], b[512];
        snprintf(a, sizeof(a), "%s/%s", from, e[i].name);
        snprintf(b, sizeof(b), "%s/%s", to, e[i].name);
        copy_path(a, b, depth + 1);
    }
    free(e);
}

int main(int argc, char **argv) {
    int i = 1;
    for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++)
        for (const char *f = argv[i] + 1; *f; f++)
            if (*f == 'r' || *f == 'R' || *f == 'a') recursive = 1;
    if (argc - i < 2) {
        printf("usage: cp [-r] <from>... <to>   (quote names with spaces)\n");
        return 2;
    }
    const char *dest = argv[argc - 1];
    struct os_stat st;
    int into = stat(dest, &st) == 0 && st.is_dir;
    if (argc - i > 2 && !into) {
        printf("cp: %s: not a folder (several files go into a folder)\n", dest);
        return 1;
    }
    for (; i < argc - 1; i++) {
        char to[512];
        if (into) snprintf(to, sizeof(to), "%s/%s", dest, base_name(argv[i]));
        else snprintf(to, sizeof(to), "%s", dest);
        copy_path(argv[i], to, 0);
    }
    return failed;
}
