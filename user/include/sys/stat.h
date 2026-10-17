/* sys/stat.h - only a file's size, for ported code */
#ifndef SYS_STAT_H
#define SYS_STAT_H
#include <sys/types.h>
#define S_IFMT 0170000
#define S_IFDIR 0040000
#define S_IFREG 0100000
#define S_ISDIR(m) (((m) & S_IFMT) == S_IFDIR)
#define S_ISREG(m) (((m) & S_IFMT) == S_IFREG)
struct stat {
    mode_t st_mode;
    off_t st_size;
    time_t st_mtime;
};
int fstat(int fd, struct stat *st);
#ifndef OS_H                               /* (os.h has its own: mkdir(path)) */
int mkdir(const char *path, mode_t mode);   /* the mode is ignored */
#endif
#endif
