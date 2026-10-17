/* unistd.h - POSIX names for the system calls in os.h (the same functions) */
#ifndef UNISTD_H
#define UNISTD_H
#include <sys/types.h>
long read(int fd, void *buf, size_t len);
long write(int fd, const void *buf, size_t len);
int close(int fd);
long lseek(int fd, long offset, int whence);
int unlink(const char *path);
int getpid(void);
int usleep(unsigned int us);
unsigned int sleep(unsigned int s);
#ifndef SEEK_SET
#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2
#endif
#endif
