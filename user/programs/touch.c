// touch <file> - create an empty file (an existing one is left as it is)
#include <stdio.h>
#include <os.h>
#include <args.h>

int main(int argc, char **argv) {
    if (argc < 2) {
        printf("usage: touch <file>\n");
        return 2;
    }
    char name[256];
    args_join(name, sizeof(name), argc, argv, 1);
    struct os_stat st;
    if (stat(name, &st) == 0) return 0;
    int fd = open(name, OPEN_WRITE);
    int r = fd < 0 ? fd : close(fd);
    if (r < 0) {
        printf("touch: %s: %s\n", name, os_strerror(r));
        return 1;
    }
    printf("Created file: %s\n", name);
    return 0;
}
