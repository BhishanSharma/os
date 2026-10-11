// hexdump [-a] <file> - the first 256 bytes of a file in hex (-a: all of it)
#include <stdio.h>
#include <string.h>
#include <os.h>
#include <args.h>

int main(int argc, char **argv) {
    int all = argc > 1 && strcmp(argv[1], "-a") == 0;
    if (argc < 2 + all) {
        printf("usage: hexdump [-a] <file>\n");
        return 2;
    }
    char name[256];
    args_join(name, sizeof(name), argc, argv, 1 + all);
    int fd = open(name, OPEN_READ);
    if (fd < 0) {
        if (fd == SYSERR_NOENT) printf("File not found: %s\n", name);
        else printf("hexdump: %s: %s\n", name, os_strerror(fd));
        return 1;
    }
    unsigned char row[16];
    unsigned long offset = 0, limit = all ? ~0ul : 256;
    if (!all) printf("=== Hex Dump (first 256 bytes) ===\n");
    long got;
    while (offset < limit && (got = read(fd, row, sizeof(row))) > 0) {
        printf("%08lx: ", offset);
        for (long j = 0; j < 16; j++) {
            if (j < got) printf("%02X ", row[j]);
            else printf("   ");                          /* keep the text column aligned */
        }
        printf(" | ");
        for (long j = 0; j < got; j++) putchar(row[j] >= 32 && row[j] <= 126 ? row[j] : '.');
        putchar('\n');
        offset += (unsigned long)got;
    }
    close(fd);
    return 0;
}
