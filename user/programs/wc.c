// wc [file]... - count lines, words and bytes; without files, of what comes
// in (`ls | wc`, `wc < notes.txt`)
#include <stdio.h>
#include <os.h>

static void count(int fd, const char *name) {
    long lines = 0, words = 0, bytes = 0;
    int in_word = 0;
    char buf[512];
    long n;
    while ((n = read(fd, buf, sizeof(buf))) > 0) {
        for (long j = 0; j < n; j++) {
            char c = buf[j];
            bytes++;
            if (c == '\n') lines++;
            int space = c == ' ' || c == '\n' || c == '\t' || c == '\r';
            if (!space && !in_word) words++;
            in_word = !space;
        }
    }
    printf("%7ld %7ld %7ld %s\n", lines, words, bytes, name);
}

int main(int argc, char **argv) {
    if (argc < 2) {
        count(0, "");
        return 0;
    }
    int status = 0;
    for (int i = 1; i < argc; i++) {
        int fd = open(argv[i], OPEN_READ);
        if (fd < 0) {
            printf("wc: %s: no such file\n", argv[i]);
            status = 1;
            continue;
        }
        count(fd, argv[i]);
        close(fd);
    }
    return status;
}
