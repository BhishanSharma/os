// mkdir <folder> - create a folder
#include <stdio.h>
#include <os.h>
#include <args.h>

int main(int argc, char **argv) {
    if (argc < 2) {
        printf("usage: mkdir <folder>\n");
        return 2;
    }
    char name[256];
    args_join(name, sizeof(name), argc, argv, 1);
    int r = mkdir(name);
    if (r < 0) {
        printf("mkdir: %s: %s\n", name, os_strerror(r));
        return 1;
    }
    printf("Created directory: %s\n", name);
    return 0;
}
