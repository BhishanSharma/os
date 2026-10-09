// wc <file>... - count lines, words and bytes
#include <stdio.h>
#include <os.h>

int main(int argc, char **argv) {
    if (argc < 2) {
        printf("usage: wc <file>...\n");
        return 1;
    }
    int status = 0;
    for (int i = 1; i < argc; i++) {
        int fd = open(argv[i], OPEN_READ);
        if (fd < 0) {
            printf("wc: %s: no such file\n", argv[i]);
            status = 1;
            continue;
        }
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
        close(fd);
        printf("%7ld %7ld %7ld %s\n", lines, words, bytes, argv[i]);
    }
    return status;
}
