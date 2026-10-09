// os.c - system call wrappers
#include <os.h>

void exit(int code) {
    os_syscall(SYS_EXIT, code, 0, 0);
    for (;;) { }
}

long write(int fd, const void *buf, size_t len) { return os_syscall(SYS_WRITE, fd, (long)buf, (long)len); }
long read(int fd, void *buf, size_t len)        { return os_syscall(SYS_READ, fd, (long)buf, (long)len); }
int open(const char *path, int mode)            { return (int)os_syscall(SYS_OPEN, (long)path, mode, 0); }
int close(int fd)                               { return (int)os_syscall(SYS_CLOSE, fd, 0, 0); }
int unlink(const char *path)                    { return (int)os_syscall(SYS_UNLINK, (long)path, 0, 0); }

void *sbrk(long increment) {
    long r = os_syscall(SYS_SBRK, increment, 0, 0);
    return r < 0 ? (void *)-1 : (void *)r;
}

void sleep_ms(unsigned long ms)                 { os_syscall(SYS_SLEEP, (long)ms, 0, 0); }
unsigned long uptime_ms(void)                   { return (unsigned long)os_syscall(SYS_UPTIME, 0, 0, 0); }
int gettime(struct os_time *t)                  { return (int)os_syscall(SYS_TIME, (long)t, 0, 0); }
int getkey(void)                                { return (int)os_syscall(SYS_GETKEY, 0, 0, 0); }
int readdir(int index, struct os_dirent *e)     { return (int)os_syscall(SYS_READDIR, index, (long)e, 0); }
int getuser(struct os_user *u)                  { return (int)os_syscall(SYS_GETUSER, (long)u, 0, 0); }

void clear_screen(void)                         { os_syscall(SYS_CONSOLE, CON_CLEAR, 0, 0); }
void gotoxy(int col, int row)                   { os_syscall(SYS_CONSOLE, CON_GOTO, col, row); }
void set_color(int fg, int bg)                  { os_syscall(SYS_CONSOLE, CON_COLOR, fg, bg); }
void reset_color(void)                          { os_syscall(SYS_CONSOLE, CON_RESET, 0, 0); }
void show_cursor(int visible)                   { os_syscall(SYS_CONSOLE, CON_CURSOR, visible, 0); }

void console_size(int *cols, int *rows) {
    long r = os_syscall(SYS_CONSOLE, CON_SIZE, 0, 0);
    if (cols) *cols = (int)(r >> 16);
    if (rows) *rows = (int)(r & 0xFFFF);
}
