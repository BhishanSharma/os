/* fontcheck: what Ladybird does to find its fonts (stat, readdir, realpath,
 * mmap of a font file), each step printed.   fontcheck [folder] */
#include <dirent.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <errno.h>

int main(int argc, char **argv) {
    const char *dir = argc > 1 ? argv[1] : "/ladybird/share/fonts";
    char exe[PATH_MAX] = "";
    ssize_t n = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
    printf("exe: %s (%zd)\n", exe, n);

    struct stat st;
    int r = stat(dir, &st);
    printf("stat %s: %d errno %d dir %d\n", dir, r, r ? errno : 0, r == 0 && S_ISDIR(st.st_mode));

    DIR *d = opendir(dir);
    printf("opendir: %s errno %d\n", d ? "ok" : "failed", d ? 0 : errno);
    if (!d) return 1;
    struct dirent *e;
    int files = 0;
    while ((e = readdir(d))) {
        files++;
        if (files > 3) continue;
        char path[PATH_MAX], real[PATH_MAX];
        snprintf(path, sizeof(path), "%s/%s", dir, e->d_name);
        printf("entry %s type %d\n", e->d_name, e->d_type);
        printf("  realpath: %s errno %d\n", realpath(path, real) ? real : "(failed)", errno);
        int fd = open(path, O_RDONLY);
        if (fd < 0 || fstat(fd, &st)) { printf("  open/fstat failed errno %d\n", errno); continue; }
        void *m = mmap(0, st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
        printf("  size %lld mmap %s first bytes %02x %02x %02x %02x\n", (long long)st.st_size,
               m == MAP_FAILED ? "failed" : "ok", m == MAP_FAILED ? 0 : ((unsigned char *)m)[0],
               m == MAP_FAILED ? 0 : ((unsigned char *)m)[1], m == MAP_FAILED ? 0 : ((unsigned char *)m)[2],
               m == MAP_FAILED ? 0 : ((unsigned char *)m)[3]);
        close(fd);
    }
    closedir(d);
    printf("entries: %d\n", files);
    return 0;
}
