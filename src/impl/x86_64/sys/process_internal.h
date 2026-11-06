// process_internal.h - what the system call files share with sys/process.c
// (not for the rest of the kernel)
#ifndef PROCESS_INTERNAL_H
#define PROCESS_INTERNAL_H

#include <stdint.h>
#include <stddef.h>
#include "sys/pipe.h"
#include "sys/port.h"
#include "sys/vm.h"
#include "sys/vfs.h"
#include "sys/syscall_nums.h"
#include "sys/task.h"

#define NO_LIMIT_MS 0x7FFFFFFu      /* a wait with "no limit": about 37 hours */

#define MAX_FDS          1024
#define MAX_PATH         256              /* FAT32_MAX_PATH: long names */

/* An open file: a file on disk, the console, or one end of a pipe. Shared
 * (counted) by every descriptor that refers to it: dup, fork, and the
 * standard 0-2 a program inherits from the one that started it. */
enum { F_FILE, F_CONSOLE, F_PIPE_R, F_PIPE_W, F_USOCK, F_ULISTEN, F_SHM };

/* A connected local socket: a pipe each way and the files passed each way.
 * End `side` reads pipe[side] and writes pipe[!side]. */
/* A pseudo-terminal (USOCK_PTY): its mode and size, on a local socket pair
 * whose side 0 is the master (the window), side 1 the slave. */
typedef struct {
    struct os_termios t;
    struct os_winsize ws;
    int fg;                           // who set the mode last: Ctrl+C's SIGINT goes there (raw mode)
    int eof;                          // Ctrl+D typed: slave reads that return 0
} pty_t;

typedef struct usock {
    pipe_t *pipe[2];
    pty_t *pty;                       // a pseudo-terminal (0: a plain local socket)
    struct kfile *fdq[2][USOCK_MAX_FDS];  // fdq[i]: files passed toward side i
    int fdq_n[2];
    int ends;                         // its file_t's: the last one frees it
} usock_t;

/* A listening local socket: connections waiting for accept(). */
#define MAX_LISTENERS 32
#define BACKLOG       16
typedef struct {
    struct kfile *q[BACKLOG];
    int n;
} ulisten_t;

typedef struct kfile {
    int used, mode;                   // OPEN_READ / OPEN_WRITE / OPEN_APPEND
    int refs, kind;
    int nonblock;                     // pipes and local sockets: SYSERR_WOULDBLOCK instead of waiting
    pipe_t *pipe;
    usock_t *us;                      // F_USOCK (0: not connected yet), and which end
    int side;
    ulisten_t *ul;                    // F_ULISTEN
    vm_object_t *shm;                 // F_SHM: the shared memory (counted)
    char path[MAX_PATH];              // (local sockets: the name)
    uint8_t *data;
    uint32_t size, capacity, pos;
    vfs_file_t *stream;               // a big file opened for reading: read from disk as it goes
} file_t;


#define SANDBOX_DIRS 6

typedef struct {
    int on, net;
    char dirs[SANDBOX_DIRS][96];      // absolute: "/HOME/BOSS/Apps/notes"
} sandbox_t;

typedef struct {
    uint64_t brk_start, brk;
    uint64_t stack_lo;                // the stack region's start
    uint64_t root;                    // the address space (shared by its threads)
    vm_space_t vm;
    int threads;                      // tasks running it
    port_space_t ports;
    uint64_t sig_handler[32], sig_tramp;
    uint32_t sig_pending;
    int no_echo;                      // CON_ECHO off: read(0) hides what is typed
    file_t *fds[MAX_FDS];
    uint8_t cloexec[MAX_FDS];         // FD_CLOEXEC: closed by exec
    char exe[MAX_PATH];               // the program's file
    char line_buf[256];               // keyboard input for read(0), one line at a time
    uint32_t line_len, line_pos;
    char cwd[256];                    // its own current directory
    sandbox_t sandbox, sandbox_next;  // what it may use (on: sandboxed); for the next program it starts
    int ctrlc_mode;                   // CTRLC_END / CTRLC_KEY
    pipe_t *tap_out, *tap_in;         // a kernel command run from a Terminal window: its pipes
    char tap_buf[128];
    int tap_len;
    const char *tap_echo;             // the echo of the key just read: the window showed it
} process_t;

/* The running program. */
static inline process_t *P(void) { return (process_t *)task_current()->process; }
int process_may_network(void);           /* 0 in a sandbox without the network */

/* 1 if the running program may read (or, with `write`, write) [ptr, ptr + len);
 * missing pages of its regions are brought in. */
int user_range_ok(uint64_t ptr, uint64_t len, int write);
/* Copy a NUL-terminated string from the program; 0 on success. */
int copy_user_string(char *out, uint64_t ptr, size_t max);
/* End the running program here if it was killed (Ctrl+C, kill, a fault). */
void check_killed(void);

/* Copy the program's path string at `ptr` and make it absolute. 0 on success. */
int user_path(uint64_t ptr, char *out, int size);

/* Open files and descriptors (process.c). */
file_t *file_new(int kind);
file_t *file_get(file_t *f);              /* one reference more */
int64_t file_put(file_t *f);              /* one less; the last closes it */
file_t *fd_get(uint64_t fd);              /* the running program's descriptor, 0: none */
int fd_free_slot(process_t *p, int from);
int64_t install(file_t *f);               /* a descriptor for f (or drop it) */
int64_t signal_send(int64_t pid, uint64_t sig);   /* as the sigsend call */
int signal_handled(int pid, int sig);     /* 1 if that process has a handler for sig */
int64_t sys_close(uint64_t fd);
void ends_added(file_t *f);               /* pipe ends: one more reader or writer */
void ends_closed(file_t *f);

/* syscall_usock.c: local sockets. */
int64_t sys_usock(uint64_t op, uint64_t a, uint64_t b, uint64_t c);
void usock_closed(file_t *f);
void listener_closed(file_t *f);

/* syscall_net.c: SYS_RESOLVE ... SYS_SOCKCLOSE. */
int64_t sys_net(uint64_t nr, uint64_t a, uint64_t b, uint64_t c, uint64_t d);

#endif
