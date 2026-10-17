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

static inline long os_syscall4(long n, long a, long b, long c, long d) {
    long ret;
    register long r10 __asm__("r10") = d;
    __asm__ volatile("int $0x80"
                     : "=a"(ret)
                     : "a"(n), "D"(a), "S"(b), "d"(c), "r"(r10)
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
int gettime_utc(struct os_time *t);          /* the same in UTC */

int getkey(void);                            /* next key or 0, does not wait */
int waitkey(void);                           /* next key, waits for one */
int readdir(int index, struct os_dirent *entry);   /* 1 = filled, 0 = no more */
int listdir(const char *path, struct os_dirent *entries, int max);   /* path 0: current -> count */
int mkdir(const char *path);
int stat(const char *path, struct os_stat *st);
int meminfo(struct os_meminfo *m);
int getuser(struct os_user *user);           /* who is running this program */
int getpid(void);                            /* this program's process id */
void yield(void);                            /* let other programs run now */

/* Programs that run programs */
int spawn(const char *path, char *const argv[], const char *as_user);  /* -> pid */
int wait(int pid, int *status, int flags);   /* WAIT_NOHANG, WAIT_FOREGROUND; pid -1 = any child */
int kill(int pid);
int taskinfo(int slot, struct os_task *task);   /* slots 0.. until < 0; 1 = used */
int chdir(const char *path);
int getcwd(char *buf, size_t size);
int kcommand(const char *line);              /* a command built into the kernel: 1 ran, 0 unknown */
int ctrlc(int mode);                         /* CTRLC_END (default) or CTRLC_KEY */
int uname(struct os_uname *u);
int getmouse(struct os_mouse *m);            /* 1 if there is a mouse; takes it from the console */

/* Pixels (framebuffer console only): see GFX_* in sys/syscall_nums.h */
int gfx_info(struct os_gfx_info *info);      /* 0, or SYSERR_NOENT in VGA text mode */
int gfx_begin(void);                         /* take the screen below the status bar */
int gfx_begin_full(void);
/* The screen-sized frame your blits come from: the GPU copies straight from it
 * (no copy by the CPU) -> 0; SYSERR_AGAIN: draw all of it once first. */
int gfx_surface(const unsigned int *pixels, int w, int h, int stride);
int gfx_sync(void);                          /* the GPU is done reading that frame: draw */
/* Copies into that frame by the GPU, after your drawing (at gfx_present) -> 0;
 * otherwise draw them yourself. */
int gfx_copy(const struct os_copy *items, int n);                    /* the whole screen, status bar too (the desktop) */
int gfx_blit(int x, int y, int w, int h, const unsigned int *pixels, int stride);   /* 0x00RRGGBB */
int gfx_end(void);
int gfx_present(void);                       /* after blits: shown at the next refresh (waits for the last) -> 1, 0 */
/* The kernel draws the pointer (0xAARRGGBB picture), moving it as the mouse
 * moves: smooth whatever the program is doing. -> 0 */
int gfx_cursor(int w, int h, int hot_x, int hot_y, const unsigned int *argb);                           /* the console redraws itself */

/* Files opened for reading: move the read position (SEEK_SET/CUR/END). */
long lseek(int fd, long offset, int whence); /* -> the new position */

/* Sound: a 48 kHz, 16-bit stereo stream (left, right, left, ...). One
 * program at a time; it ends with the program. */
int sound_open(void);                        /* 0, or SYSERR_NOENT / SYSERR_INUSE */
long sound_write(const short *frames, long count);   /* waits for room -> frames queued */
long sound_queued(void);                     /* frames not heard yet (about 1/3 s at most) */
void sound_close(void);

/* TCP sockets. Results < 0 are SYSERR_* codes (SYSERR_TIMEOUT, SYSERR_REFUSED...). */
int resolve(const char *host, unsigned char ip[4]);        /* DNS (or "a.b.c.d") */
int net_connect(const unsigned char ip[4], int port);      /* -> socket */
int net_listen(int port);                                  /* -> listening socket */
int net_accept(int listener, unsigned long timeout_ms);    /* 0 = wait as long as it takes */
long net_send(int sock, const void *buf, size_t len);      /* sends all of it */
long net_recv(int sock, void *buf, size_t len, unsigned long timeout_ms);   /* 0 = peer closed */
int net_close(int sock);
const char *net_strerror(int err);
const char *os_strerror(int err);            /* text for a negative SYSERR_* result */

/* Memory: pages appear when first touched. */
void *mmap(size_t len, int prot, int fd);    /* fd -1: zeroed memory; else that file (read-only copy) -> 0 on error */
int munmap(void *addr);                      /* a whole mapping */
void *shm_map(const char *name, size_t size);   /* shared memory: every program mapping `name` sees the same */

/* Programs */
int fork(void);                              /* -> child pid in the parent, 0 in the child */
int exec(const char *path, char *const argv[]);  /* returns only on error */

/* Threads: share the program's memory; wait(tid, ...) joins one. */
int thread_create(void (*fn)(void *), void *arg);   /* -> thread id */
void thread_exit(int code) __attribute__((noreturn));
int futex(int op, volatile int *addr, int value);   /* FUTEX_WAIT / FUTEX_WAKE */
typedef struct { volatile int v; } mutex_t;  /* = { 0 } */
void mutex_lock(mutex_t *m);
void mutex_unlock(mutex_t *m);

/* Ports (Mach-style messages): see sys/port.h in the kernel. */
int port_create(void);                       /* -> a port you receive on */
int port_send(int port, const void *msg, size_t len, int reply_port);   /* reply_port -1: none */
int port_receive(int port, void *buf, size_t max, unsigned long timeout_ms, int *reply);  /* -> length */
int port_register(const char *service, int port);
int port_lookup(const char *service);        /* -> a port you can send to */
int port_close(int port);
int port_call(int port, const void *msg, size_t len, void *reply_buf, size_t max, unsigned long timeout_ms);

/* Descriptors: 0 in, 1 out, 2 errors; programs started with spawn get yours. */
int pipe(int fds[2]);                        /* fds[0] reads, fds[1] writes */
int dup(int fd);
int dup2(int fd, int to);

/* Signals (SIGINT, SIGTERM, SIGUSR1, SIGCHLD, ...): a handler runs when the
 * program next makes a system call. */
int signal(int sig, void (*handler)(int));   /* handler 0: the default */
int sigsend(int pid, int sig);

int registry(int index, struct os_regnode *node);   /* the device tree: 1, 0 past the end */
int auth(const char *name, const char *password, struct os_user *u);   /* root only */

/* The desktop's system menus (Wi-Fi, sound, battery, power) */
int wifi_status(struct os_wifi *w);           /* 1 connected (w filled), 0 not, SYSERR_NOENT no card */
int wifi_scan(struct os_wifi *list, int max); /* networks heard, strongest first -> count (takes seconds) */
int wifi_connect(const char *name, const char *password);
int wifi_disconnect(void);
int volume(int set);                          /* 0-100 sets, -1 reads -> the volume */
int battery(void);                            /* percent | 0x100 charging, SYSERR_NOENT none */
int power(int restart);                       /* 0 shut down, 1 restart: returns only on failure */
int setup_needed(void);                       /* 1: no accounts yet (first start) */
int setup_accounts(const char *name, const char *password);   /* 0; -2 bad name, -3 bad password */
void setup_text(void);                        /* the first-start questions on the console */
int cpu_count(void);                       /* cores that run programs (threads can use them all) */
int pointer_settings(struct os_pointer *p, int set);   /* mouse and touchpad: set 1 writes, 0 reads */
int syslog_read(unsigned long *from, char *buf, int max);   /* the system log from byte *from -> bytes */
void console_echo(int on);                   /* off: read(0) does not show the typing */

/* Console */
void clear_screen(void);
void gotoxy(int col, int row);
void set_color(int fg, int bg);              /* VGA colours 0-15, see below */
void reset_color(void);
void console_size(int *cols, int *rows);
void show_cursor(int visible);
void set_theme_color(int role);
int cursor_column(void);              /* THEME_TEXT, THEME_ACCENT, THEME_GOOD, ... */

enum {
    COLOR_BLACK, COLOR_BLUE, COLOR_GREEN, COLOR_CYAN, COLOR_RED, COLOR_MAGENTA,
    COLOR_BROWN, COLOR_LIGHT_GRAY, COLOR_DARK_GRAY, COLOR_LIGHT_BLUE,
    COLOR_LIGHT_GREEN, COLOR_LIGHT_CYAN, COLOR_LIGHT_RED, COLOR_PINK,
    COLOR_YELLOW, COLOR_WHITE
};

#endif
