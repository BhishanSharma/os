// chmod <mode> <file> - change who may read, write and run a file or folder
//
// The mode is octal (755, 600, 1777) or u/g/o/a then + - = then r w x
// ("go-rwx", "u+x", "a=r"). Only the owner or root may. On FAT32 volumes
// there are no permissions to change.
#include <stdio.h>
#include <string.h>
#include <os.h>
#include <args.h>

static int parse(const char *s, unsigned old, unsigned *out) {
    if (*s >= '0' && *s <= '7') {
        unsigned m = 0;
        for (; *s; s++) {
            if (*s < '0' || *s > '7') return -1;
            m = m * 8 + (unsigned)(*s - '0');
        }
        if (m > 07777) return -1;
        *out = m;
        return 0;
    }
    unsigned m = old;
    while (*s) {                                          /* "u+x,go-w" */
        unsigned who = 0;
        for (; *s == 'u' || *s == 'g' || *s == 'o' || *s == 'a'; s++)
            who |= *s == 'u' ? 0700 : *s == 'g' ? 0070 : *s == 'o' ? 0007 : 0777;
        if (!who) who = 0777;
        char op = *s++;
        if (op != '+' && op != '-' && op != '=') return -1;
        unsigned bits = 0;
        for (; *s == 'r' || *s == 'w' || *s == 'x' || *s == 't'; s++)
            bits |= *s == 'r' ? 0444 : *s == 'w' ? 0222 : *s == 'x' ? 0111 : 01000;
        unsigned mask = (bits & 0777 & who) | (bits & 01000);
        if (op == '+') m |= mask;
        else if (op == '-') m &= ~mask;
        else m = (m & ~who) | mask;
        if (*s == ',') s++;
        else if (*s) return -1;
    }
    *out = m;
    return 0;
}

int main(int argc, char **argv) {
    if (argc < 3) {
        printf("usage: chmod <mode> <file>   (mode: 644, 700, u+x, go-rwx, ...)\n");
        return 2;
    }
    char name[256];
    args_join(name, sizeof(name), argc, argv, 2);
    struct os_statx st;
    int r = statx(name, &st);
    unsigned mode;
    if (r == 0 && parse(argv[1], st.mode, &mode) != 0) {
        printf("chmod: %s: not a mode (644, 700, u+x, go-rwx, ...)\n", argv[1]);
        return 2;
    }
    if (r == 0) r = chmod(name, mode);
    if (r < 0) {
        printf("chmod: %s: %s\n", name, os_strerror(r));
        return 1;
    }
    return 0;
}
