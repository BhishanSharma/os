/* os.h - Terminal OS system calls for user programs.
 * The numbers and structures are shared with the kernel (sys/syscall_nums.h). */
#ifndef OS_H
#define OS_H

#include <stddef.h>
#include <stdint.h>
#include "sys/syscall_nums.h"

/* Raw system call: int 0x80, number in rax, arguments in rdi, rsi, rdx. */
static inline long os_syscall(long n, long a, long b, long c) {
    long ret;
    __asm__ volatile("int $0x80"
                     : "=a"(ret)
                     : "a"(n), "D"(a), "S"(b), "d"(c)
                     : "memory");
    return ret;
}

void exit(int code) __attribute__((noreturn));
long write(int fd, const void *buf, size_t len);
long read(int fd, void *buf, size_t len);   /* fd 0: one keyboard line, with '\n' */
int open(const char *path, int mode);        /* OPEN_READ, OPEN_WRITE, OPEN_APPEND */
int close(int fd);                           /* writes are saved to disk here */
int unlink(const char *path);
void *sbrk(long increment);                  /* (void *)-1 on failure */

void sleep_ms(unsigned long ms);
unsigned long uptime_ms(void);
int gettime(struct os_time *t);              /* local date and time */

int getkey(void);                            /* next key or 0, does not wait */
int readdir(int index, struct os_dirent *entry);   /* 1 = filled, 0 = no more */
int getuser(struct os_user *user);           /* who is running this program */
int getpid(void);                            /* this program's process id */
void yield(void);                            /* let other programs run now */

/* Console */
void clear_screen(void);
void gotoxy(int col, int row);
void set_color(int fg, int bg);              /* VGA colours 0-15, see below */
void reset_color(void);
void console_size(int *cols, int *rows);
void show_cursor(int visible);

enum {
    COLOR_BLACK, COLOR_BLUE, COLOR_GREEN, COLOR_CYAN, COLOR_RED, COLOR_MAGENTA,
    COLOR_BROWN, COLOR_LIGHT_GRAY, COLOR_DARK_GRAY, COLOR_LIGHT_BLUE,
    COLOR_LIGHT_GREEN, COLOR_LIGHT_CYAN, COLOR_LIGHT_RED, COLOR_PINK,
    COLOR_YELLOW, COLOR_WHITE
};

#endif
