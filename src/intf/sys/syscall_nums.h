/* syscall_nums.h - the system call interface, shared by the kernel
 * (sys/process.c) and the user C library (user/lib).
 *
 * Calling convention (like Linux): `int 0x80` with the number in rax and the
 * arguments in rdi, rsi, rdx, r10, r8; the result comes back in rax. Negative
 * results are errors (SYSERR_*). */
#ifndef SYSCALL_NUMS_H
#define SYSCALL_NUMS_H

#define SYS_EXIT     0   /* exit(code)                                   -> no return  */
#define SYS_WRITE    1   /* write(fd, buf, len)                          -> bytes      */
#define SYS_READ     2   /* read(fd, buf, len); fd 0 = keyboard line     -> bytes, 0 at EOF */
#define SYS_OPEN     3   /* open(path, mode) mode: OPEN_READ/WRITE/APPEND -> fd        */
#define SYS_CLOSE    4   /* close(fd); a written file is saved here      -> 0          */
#define SYS_SBRK     5   /* sbrk(increment)                              -> old break  */
#define SYS_SLEEP    6   /* sleep(milliseconds)                          -> 0          */
#define SYS_UPTIME   7   /* uptime()                                     -> milliseconds since boot */
#define SYS_TIME     8   /* time(struct os_time *, utc) local date and time (UTC if utc) -> 0 */
#define SYS_GETKEY   9   /* getkey(): next key, 0 if none (no waiting)   -> key code   */
#define SYS_CONSOLE 10   /* console(op, a, b), op = CON_*                -> see below  */
#define SYS_READDIR 11   /* readdir(index, struct os_dirent *) current dir -> 1, or 0 past the end */
#define SYS_UNLINK  12   /* unlink(path): a file, or an empty folder     -> 0          */
#define SYS_GETUSER 13   /* getuser(struct os_user *) who runs this      -> 0          */
#define SYS_GETPID  14   /* getpid()                                     -> process id */
#define SYS_YIELD   15   /* yield(): let other programs run now          -> 0          */
#define SYS_SPAWN   16   /* spawn(path, argv, as_user): start a program  -> pid        */
#define SYS_WAIT    17   /* wait(pid, int *status, flags), pid -1 = any  -> pid that ended, 0 (WAIT_NOHANG) */
#define SYS_KILL    18   /* kill(pid): end a program of yours            -> 0          */
#define SYS_TASKINFO 19  /* taskinfo(slot, struct os_task *)             -> 1 used, 0 free slot, <0 past the end */
#define SYS_CHDIR   20   /* chdir(path)                                  -> 0          */
#define SYS_GETCWD  21   /* getcwd(buf, size)                            -> length     */
#define SYS_KCOMMAND 22  /* kcommand(line): run a built-in kernel command -> 1, 0 if unknown */
#define SYS_CTRLC   23   /* ctrlc(mode): what Ctrl+C does to this program -> 0         */
#define SYS_UNAME   24   /* uname(struct os_uname *)                     -> 0          */
#define SYS_MOUSE   25   /* getmouse(struct os_mouse *): takes the mouse while in the foreground -> 1, 0 if no mouse */
#define SYS_GFX     26   /* gfx(op, arg): pixels on the screen, op = GFX_* -> see below */

/* TCP sockets (the 4th argument goes in r10). A socket belongs to the program
 * that opened it and is closed when the program ends. */
#define SYS_RESOLVE   27 /* resolve(host, uint8_t ip[4]): DNS             -> 0          */
#define SYS_CONNECT   28 /* connect(ip as a.b.c.d = a<<24.., port, timeout ms) -> socket */
#define SYS_LISTEN    29 /* listen(port)                                  -> socket     */
#define SYS_ACCEPT    30 /* accept(socket, timeout ms; 0 = no limit)      -> socket     */
#define SYS_SEND      31 /* send(socket, buf, len): all of it, waiting    -> len        */
#define SYS_RECV      32 /* recv(socket, buf, len, timeout ms; 0 = none)  -> bytes, 0 = closed by the peer */
#define SYS_SOCKCLOSE 33 /* closesocket(socket)                           -> 0          */
#define SYS_SEEK      34 /* seek(fd, offset, whence) a file opened for reading -> new position */
#define SYS_SOUND     35 /* sound(op, a, b), op = SOUND_*: a 48 kHz 16-bit stereo stream -> see below */

/* Files and folders */
#define SYS_MKDIR     36 /* mkdir(path)                                   -> 0          */
#define SYS_STAT      37 /* stat(path, struct os_stat *)                  -> 0          */
#define SYS_LISTDIR   38 /* listdir(path, struct os_dirent *, max): a folder, 0 = current -> count */
#define SYS_MEMINFO   39 /* meminfo(struct os_meminfo *)                  -> 0          */

/* Memory (stage 1): regions filled in on first use */
#define SYS_MMAP      40 /* mmap(len, prot, fd): fd -1 = zeroed memory, else the file (private) -> address */
#define SYS_MUNMAP    41 /* munmap(addr): a whole mapping                 -> 0          */
#define SYS_SHMMAP    42 /* shm_map(name, size): shared memory, by name   -> address    */

/* Programs (stage 4) */
#define SYS_FORK      43 /* fork(): a copy (copy-on-write) of this program -> child pid, 0 in the child */
#define SYS_EXEC      44 /* exec(path, argv): replace this program        -> only on error */

/* Threads (stage 2) */
#define SYS_THREAD    45 /* thread(entry, arg, stack_top)                  -> thread id (wait() joins it) */
#define SYS_THREAD_EXIT 46 /* thread_exit(code): only this thread          -> no return  */
#define SYS_FUTEX     47 /* futex(op, addr, value): FUTEX_WAIT while *addr == value, FUTEX_WAKE -> 0 */

/* Ports (stage 3), see sys/port.h */
#define SYS_PORT_CREATE   48 /* port_create()                              -> name (receive right) */
#define SYS_PORT_SEND     49 /* port_send(name, msg, len, reply_name or -1) -> 0         */
#define SYS_PORT_RECEIVE  50 /* port_receive(name, buf, max, timeout ms, int *reply) -> length */
#define SYS_PORT_REGISTER 51 /* port_register(service, name)               -> 0          */
#define SYS_PORT_LOOKUP   52 /* port_lookup(service)                       -> name (send right) */
#define SYS_PORT_CLOSE    53 /* port_close(name)                           -> 0          */

/* File descriptors and signals (stage 4) */
#define SYS_PIPE      54 /* pipe(int fds[2]): [0] reads, [1] writes       -> 0          */
#define SYS_DUP       55 /* dup(fd)                                        -> new fd     */
#define SYS_DUP2      56 /* dup2(fd, new_fd)                               -> new_fd     */
#define SYS_SIGNAL    57 /* signal(sig, handler, trampoline): 0 handler = default -> 0 */
#define SYS_SIGSEND   58 /* sigsend(pid, sig)                              -> 0          */
#define SYS_SIGRETURN 59 /* the end of a signal handler (the C library calls it) */

#define SYS_REGISTRY  60 /* registry(index, struct os_regnode *): the device tree -> 1, 0 past the end */
#define SYS_AUTH      61 /* auth(name, password, struct os_user *): root only (login) -> 0 */

#define PROT_READ   1
#define PROT_WRITE  2
#define FUTEX_WAIT  0
#define FUTEX_WAKE  1

#define SIGINT    2
#define SIGKILL   9
#define SIGUSR1  10
#define SIGSEGV  11
#define SIGPIPE  13
#define SIGTERM  15
#define SIGCHLD  17

/* seek() whence */
#define SEEK_SET  0
#define SEEK_CUR  1
#define SEEK_END  2

/* sound() operations. One program at a time has the sound. */
#define SOUND_OPEN    0  /* start it                                      -> 0, SYSERR_NOENT (no sound), SYSERR_INUSE */
#define SOUND_WRITE   1  /* (frames, count): queue them, waiting for room  -> frames queued (fewer after Ctrl+C) */
#define SOUND_QUEUED  2  /* frames written but not heard yet               -> count */
#define SOUND_CLOSE   3  /* stop                                           -> 0 */

/* open() modes */
#define OPEN_READ    0
#define OPEN_WRITE   1   /* create or truncate */
#define OPEN_APPEND  2   /* create, or add to the end */

/* spawn(): `as_user` 0 = the caller's user; a name runs the program as that
 * user (like su) after asking for their password, unless the caller is root. */

/* wait() flags */
#define WAIT_NOHANG     1   /* return 0 at once if it has not ended */
#define WAIT_FOREGROUND 2   /* give it the keyboard (and Ctrl+C) until it ends */

/* ctrlc() modes */
#define CTRLC_END    0   /* default: Ctrl+C ends the program (exit code 130) */
#define CTRLC_KEY    1   /* Ctrl+C arrives as key 3 from getkey(); read() returns an empty line */

/* console() operations */
#define CON_CLEAR    0   /* clear the screen, cursor to 0,0 */
#define CON_GOTO     1   /* a = column, b = row */
#define CON_COLOR    2   /* a = foreground, b = background (VGA colours 0-15) */
#define CON_RESET    3   /* back to the theme colours */
#define CON_SIZE     4   /* -> columns << 16 | rows */
#define CON_CURSOR   5   /* a = 1 show, 0 hide the cursor */
#define CON_THEME    6   /* a = THEME_*: a colour of the current theme */
#define CON_COLUMN   7   /* -> the cursor's column */
#define CON_ECHO     8   /* a = 0: read(0) does not show what is typed (passwords) */

#define THEME_TEXT     0
#define THEME_ACCENT   1
#define THEME_GOOD     2
#define THEME_BAD      3
#define THEME_WARN     4

/* Key codes from getkey()/read() beyond plain ASCII */
#define KEYCODE_UP     0x101
#define KEYCODE_DOWN   0x102
#define KEYCODE_LEFT   0x103
#define KEYCODE_RIGHT  0x104

/* Errors */
#define SYSERR_BADCALL  -1   /* unknown system call */
#define SYSERR_FAULT    -2   /* bad pointer */
#define SYSERR_BADFD    -3   /* fd not open, or wrong direction */
#define SYSERR_NOENT    -4   /* no such file */
#define SYSERR_NOMEM    -5   /* out of memory */
#define SYSERR_MFILE    -6   /* too many open files */
#define SYSERR_IO       -7   /* disk error */
#define SYSERR_PERM     -8   /* not allowed for this user (outside their home folder) */
#define SYSERR_NOEXEC   -9   /* not an executable for this OS */
#define SYSERR_AGAIN   -10   /* too many programs running */
#define SYSERR_CHILD   -11   /* no such child program to wait for */
#define SYSERR_NOUSER  -12   /* no such user */
#define SYSERR_AUTH    -13   /* wrong password */
#define SYSERR_SRCH    -14   /* no such process */
#define SYSERR_TIMEOUT -15   /* nothing happened in time */
#define SYSERR_REFUSED -16   /* connection refused */
#define SYSERR_RESET   -17   /* connection reset by the peer */
#define SYSERR_CLOSED  -18   /* the connection is closed */
#define SYSERR_NOROUTE -19   /* no network, or the gateway does not answer */
#define SYSERR_INUSE   -20   /* port already in use */
#define SYSERR_NOHOST  -21   /* DNS: no such host */
#define SYSERR_EXIST   -22   /* already exists */
#define SYSERR_BADNAME -23   /* not a valid file name */
#define SYSERR_NOTEMPTY -24  /* the folder is not empty */
#define SYSERR_NOSPC   -25   /* the disk is full */
#define SYSERR_ROFS    -26   /* read-only volume */
#define SYSERR_DEAD    -27   /* the port's receiver is gone */
#define SYSERR_PIPE    -28   /* nobody reads the pipe any more */

struct os_time {
    int year, month, day, hour, minute, second;
    char zone[8];          /* "IST" */
};

struct os_user {
    unsigned int uid;      /* 0 = root */
    char name[16];
    char home[48];         /* "/HOME/ALICE" */
};

struct os_dirent {
    char name[256];        /* the long name, e.g. "My Notes.txt" */
    unsigned char is_dir;
    unsigned int size;
};

struct os_stat {
    unsigned int size;     /* bytes (0 for a folder) */
    unsigned char is_dir;
};

struct os_meminfo {
    unsigned long ram;           /* usable RAM, bytes */
    unsigned long heap_total;    /* the kernel heap */
    unsigned long heap_used;
    unsigned long heap_blocks;   /* allocations */
    unsigned long pages_total;   /* the rest of RAM, for programs */
    unsigned long pages_used;
};

/* taskinfo() */
#define TASKSTATE_RUNNING  0
#define TASKSTATE_READY    1
#define TASKSTATE_SLEEPING 2
#define TASKSTATE_DONE     3

struct os_task {
    int pid, parent;
    int state;             /* TASKSTATE_* */
    int is_program;        /* 0: part of the kernel */
    int foreground;        /* has the keyboard */
    unsigned int cpu_ms;   /* time spent running */
    char name[16];
    char user[16];
};

/* gfx() operations. Coordinates are screen pixels; the status bar stays the
 * console's (rows above `top`). Only the foreground program may draw. */
#define GFX_INFO     0   /* arg = struct os_gfx_info *        -> 0, SYSERR_NOENT in VGA text mode */
#define GFX_BEGIN    1   /* take the screen below the status bar (cleared to black) -> 0 */
#define GFX_BLIT     2   /* arg = struct os_blit *: 0x00RRGGBB pixels -> 0 */
#define GFX_END      3   /* give it back: the console redraws (also when the program ends) */

struct os_gfx_info {
    int width, height;     /* the whole screen, in pixels */
    int top;               /* first row below the status bar */
    int cell_w, cell_h;    /* one text cell */
};

struct os_blit {
    int x, y, w, h;        /* where on the screen, and the size */
    int stride;            /* pixels from one row of `pixels` to the next */
    const unsigned int *pixels;
};

/* getmouse() */
#define MOUSE_BUTTON_LEFT    1
#define MOUSE_BUTTON_RIGHT   2
#define MOUSE_BUTTON_MIDDLE  4

struct os_mouse {
    int col, row;          /* the cell under the pointer (gotoxy coordinates; row -1 = status bar) */
    int buttons;           /* MOUSE_BUTTON_* held now */
    int wheel;             /* wheel steps since the last call: negative = up */
    int x, y;              /* the pointer in screen pixels (gfx_blit coordinates) */
};

struct os_regnode {
    int id, parent;        /* parent -1: the root */
    char name[32];         /* "NVMe drive" */
    char cls[16];          /* "storage", "pci", "volume", ... */
    char info[96];         /* details, e.g. the model */
};

struct os_uname {
    char sysname[24];      /* "Terminal OS" */
    char release[16];      /* "0.9" */
    char hostname[32];     /* "terminal-os" */
};

#endif
