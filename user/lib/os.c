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
int waitkey(void)                               { return (int)os_syscall(SYS_GETKEY, 1, 0, 0); }
int readdir(int index, struct os_dirent *e)     { return (int)os_syscall(SYS_READDIR, index, (long)e, 0); }
int getuser(struct os_user *u)                  { return (int)os_syscall(SYS_GETUSER, (long)u, 0, 0); }
int getpid(void)                                { return (int)os_syscall(SYS_GETPID, 0, 0, 0); }
void yield(void)                                { os_syscall(SYS_YIELD, 0, 0, 0); }

int spawn(const char *path, char *const argv[], const char *as_user) {
    return (int)os_syscall(SYS_SPAWN, (long)path, (long)argv, (long)as_user);
}
int wait(int pid, int *status, int flags)       { return (int)os_syscall(SYS_WAIT, pid, (long)status, flags); }
int kill(int pid)                               { return (int)os_syscall(SYS_KILL, pid, 0, 0); }
int taskinfo(int slot, struct os_task *t)       { return (int)os_syscall(SYS_TASKINFO, slot, (long)t, 0); }
int chdir(const char *path)                     { return (int)os_syscall(SYS_CHDIR, (long)path, 0, 0); }
int getcwd(char *buf, size_t size)              { return (int)os_syscall(SYS_GETCWD, (long)buf, (long)size, 0); }
int kcommand(const char *line)                  { return (int)os_syscall(SYS_KCOMMAND, (long)line, 0, 0); }
int ctrlc(int mode)                             { return (int)os_syscall(SYS_CTRLC, mode, 0, 0); }
int uname(struct os_uname *u)                   { return (int)os_syscall(SYS_UNAME, (long)u, 0, 0); }

const char *os_strerror(int err) {
    switch (err) {
        case SYSERR_BADCALL: return "invalid request";
        case SYSERR_FAULT:   return "bad address";
        case SYSERR_BADFD:   return "bad file descriptor";
        case SYSERR_NOENT:   return "no such file or directory";
        case SYSERR_NOMEM:   return "out of memory";
        case SYSERR_MFILE:   return "too many open files";
        case SYSERR_IO:      return "disk error";
        case SYSERR_PERM:    return "permission denied";
        case SYSERR_NOEXEC:  return "not an executable for this OS";
        case SYSERR_AGAIN:   return "too many programs running";
        case SYSERR_CHILD:   return "no such child program";
        case SYSERR_NOUSER:  return "no such user";
        case SYSERR_AUTH:    return "authentication failure";
        case SYSERR_SRCH:    return "no such process";
        default:             return "error";
    }
}

void clear_screen(void)                         { os_syscall(SYS_CONSOLE, CON_CLEAR, 0, 0); }
void gotoxy(int col, int row)                   { os_syscall(SYS_CONSOLE, CON_GOTO, col, row); }
void set_color(int fg, int bg)                  { os_syscall(SYS_CONSOLE, CON_COLOR, fg, bg); }
void reset_color(void)                          { os_syscall(SYS_CONSOLE, CON_RESET, 0, 0); }
void show_cursor(int visible)                   { os_syscall(SYS_CONSOLE, CON_CURSOR, visible, 0); }
void set_theme_color(int role)                  { os_syscall(SYS_CONSOLE, CON_THEME, role, 0); }
int cursor_column(void)                         { return (int)os_syscall(SYS_CONSOLE, CON_COLUMN, 0, 0); }

void console_size(int *cols, int *rows) {
    long r = os_syscall(SYS_CONSOLE, CON_SIZE, 0, 0);
    if (cols) *cols = (int)(r >> 16);
    if (rows) *rows = (int)(r & 0xFFFF);
}
