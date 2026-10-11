// fileinfo <file> - a file's name and size, or that it is a folder
#include <stdio.h>
#include <os.h>
#include <args.h>

int main(int argc, char **argv) {
    if (argc < 2) {
        printf("usage: fileinfo <file>\n");
        return 2;
    }
    char name[256];
    args_join(name, sizeof(name), argc, argv, 1);
    struct os_stat st;
    if (stat(name, &st) != 0) {
        printf("File not found: %s\n", name);
        return 1;
    }
    if (st.is_dir) {
        printf("Folder: %s\n", name);
        return 0;
    }
    printf("File: %s\n", name);
    printf("Size: %u bytes (%u KB)\n", st.size, st.size / 1024);
    return 0;
}
