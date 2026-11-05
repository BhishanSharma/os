// mv <from> <to> - rename a file or folder, or move it into a folder
//
// Within one volume; a file already called <to> is replaced. FAT32 volumes
// (/SYS, /USB) cannot rename yet.
#include <stdio.h>
#include <string.h>
#include <os.h>

int main(int argc, char **argv) {
    if (argc != 3) {
        printf("usage: mv <from> <to>   (quote names with spaces)\n");
        return 2;
    }
    char to[512];
    struct os_statx st;
    if (statx(argv[2], &st) == 0 && st.is_dir) {           /* into that folder, same name */
        const char *base = strrchr(argv[1], '/');
        snprintf(to, sizeof(to), "%s/%s", argv[2], base ? base + 1 : argv[1]);
    } else {
        snprintf(to, sizeof(to), "%s", argv[2]);
    }
    int r = rename(argv[1], to);
    if (r < 0) {
        printf("mv: %s: %s\n", argv[1], os_strerror(r));
        return 1;
    }
    return 0;
}
