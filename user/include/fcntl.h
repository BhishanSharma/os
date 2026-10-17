/* fcntl.h - POSIX open flags, for ported code. open() is the system's
 * (os.h): read (O_RDONLY) or create/truncate for writing (O_WRONLY). */
#ifndef FCNTL_H
#define FCNTL_H
#define O_RDONLY 0
#define O_WRONLY 1
#define O_RDWR 2
#define O_CREAT 0x40
#define O_EXCL 0x80
#define O_TRUNC 0x200
#define O_APPEND 0x400
#define O_NONBLOCK 0x800
#define O_BINARY 0
#define O_CLOEXEC 0x80000
int open(const char *path, int flags, ...);
#endif
