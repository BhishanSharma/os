// cat <file> - show a text file (any size: it is read as it is shown)
#include <stdio.h>
#include <os.h>
#include <args.h>

int main(int argc, char **argv) {
    char name[256];
    if (argc < 2) {
        printf("usage: cat <file>\n");
        return 2;
    }
    args_join(name, sizeof(name), argc, argv, 1);
    struct os_stat st;
    if (stat(name, &st) != 0) {
        printf("File not found: %s\n", name);
        return 1;
    }
    if (st.is_dir) {
        printf("cat: %s: is a folder\n", name);
        return 1;
    }
    if (st.size == 0) {
        printf("Empty file\n");
        return 0;
    }
    int fd = open(name, OPEN_READ);
    if (fd < 0) {
        printf("cat: %s: %s\n", name, fd == SYSERR_PERM ? "permission denied (only root can read it)" : os_strerror(fd));
        return 1;
    }
    // Drop CR (web pages use CRLF), expand tabs, and show other control
    // bytes (binary files) as '.'.
    printf("=== File Contents ===\n");
    static char in[4096], out[4096 * 4 + 8];
    long got;
    while ((got = read(fd, in, sizeof(in))) > 0) {
        long n = 0;
        for (long i = 0; i < got; i++) {
            unsigned char c = (unsigned char)in[i];
            if (c == '\r') continue;
            if (c == '\t') { out[n++] = ' '; out[n++] = ' '; out[n++] = ' '; out[n++] = ' '; }
            else if (c == '\n' || (c >= 32 && c != 127)) out[n++] = (char)c;
            else out[n++] = '.';
        }
        write(1, out, (size_t)n);
    }
    close(fd);
    printf("\n=== End === (Shift+Up/Down scrolls)\n");
    return got < 0 ? 1 : 0;
}
