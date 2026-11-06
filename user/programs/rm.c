// rm [-r] [-f] <file>... - delete files; with -r folders too, with what is in
// them (a folder that is empty goes without -r). -f: no complaint about names
// that are not there. A name with spaces works without quotes.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <os.h>
#include <args.h>

static int recursive, force, failed;

static void remove_path(const char *path, int top) {
    struct os_stat st;
    if (stat(path, &st) != 0) {
        if (!force) {
            printf("rm: %s: no such file or folder\n", path);
            failed = 1;
        }
        return;
    }
    if (st.is_dir && recursive) {                       /* what is inside first */
        struct os_dirent *e = malloc(256 * sizeof(*e));
        if (!e) {
            printf("rm: out of memory\n");
            failed = 1;
            return;
        }
        for (int round = 0; round < 64; round++) {      /* (a big folder: 256 names at a time) */
            int n = listdir(path, e, 256), went = 0;
            for (int i = 0; i < n; i++) {
                if (!strcmp(e[i].name, ".") || !strcmp(e[i].name, "..")) continue;
                char sub[512];
                snprintf(sub, sizeof(sub), "%s/%s", path, e[i].name);
                int before = failed;
                remove_path(sub, 0);
                if (failed == before) went++;
            }
            if (n < 256 || !went) break;
        }
        free(e);
    }
    int r = unlink(path);
    if (r < 0) {
        if (st.is_dir && !recursive && r != SYSERR_PERM)
            printf("rm: %s: is a folder with things in it (rm -r deletes it and them)\n", path);
        else
            printf("rm: %s: %s\n", path, os_strerror(r));
        failed = 1;
    } else if (top) {
        printf("Deleted: %s\n", path);
    }
}

int main(int argc, char **argv) {
    int i = 1;
    for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
        for (const char *f = argv[i] + 1; *f; f++) {
            if (*f == 'r' || *f == 'R') recursive = 1;
            else if (*f == 'f') force = 1;
        }
    }
    if (i >= argc) {
        printf("usage: rm [-r] [-f] <file>...\n");
        return 2;
    }
    char joined[256];
    struct os_stat st;
    if (argc - i > 1 && stat(args_join(joined, sizeof(joined), argc, argv, i), &st) == 0) {
        remove_path(joined, 1);                          /* a name with spaces */
    } else {
        for (; i < argc; i++) remove_path(argv[i], 1);
    }
    return failed;
}
