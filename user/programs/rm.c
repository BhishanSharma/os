// rm <file> - delete a file or an empty folder
#include <stdio.h>
#include <os.h>
#include <args.h>

int main(int argc, char **argv) {
    if (argc < 2) {
        printf("usage: rm <file>\n");
        return 2;
    }
    char name[256];
    args_join(name, sizeof(name), argc, argv, 1);
    int r = unlink(name);
    if (r < 0) {
        printf("rm: %s: %s\n", name, os_strerror(r));
        return 1;
    }
    printf("Deleted: %s\n", name);
    return 0;
}
