// write <file> <text> - put text in a file, replacing what it held
// ("quote" a name with spaces: write "my notes.txt" hello)
#include <stdio.h>
#include <string.h>
#include <os.h>
#include <args.h>

int main(int argc, char **argv) {
    if (argc < 3) {
        printf("Usage: write <filename> <content>\n");
        return 2;
    }
    char text[512];
    args_join(text, sizeof(text), argc, argv, 2);
    int fd = open(argv[1], OPEN_WRITE);
    if (fd < 0) {
        printf("write: %s: %s\n", argv[1], os_strerror(fd));
        return 1;
    }
    long n = write(fd, text, strlen(text));
    int r = close(fd);
    if (n < 0 || r < 0) {
        printf("write: %s: %s\n", argv[1], os_strerror(n < 0 ? (int)n : r));
        return 1;
    }
    printf("Wrote %ld bytes to %s\n", n, argv[1]);
    return 0;
}
