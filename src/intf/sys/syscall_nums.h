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
#define SYS_MUNMAP    41 /* munmap(addr, len): those pages; len 0 = the whole mapping at addr -> 0 */
#define SYS_SHMMAP    42 /* shm_map(name, size): shared memory, by name   -> address    */

/* Programs (stage 4) */
#define SYS_FORK      43 /* fork(): a copy (copy-on-write) of this program -> child pid, 0 in the child */
#define SYS_EXEC      44 /* exec(path, argv, envp): replace this program (envp 0: none) -> only on error */

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
#define SYS_TCB_SET   63 /* tcb_set(pointer): this thread's FS base (thread-local storage) -> 0 */
#define SYS_SOCKINFO  64 /* sockinfo(socket, struct os_sockinfo *): for poll() -> flags (SOCK_*) */
#define SYS_FDINFO    65 /* fdinfo(fd): for poll() -> SOCK_READABLE | SOCK_WRITABLE | SOCK_HANGUP (| SOCK_LOCAL) */
#define SYS_USOCK     66 /* usock(op, a, b, c): local (Unix-domain) sockets, op = USOCK_* */
#define SYS_SHMFD     68 /* shmfd(op, a, b): shared memory as a file descriptor, op = SHMFD_* */
#define SYS_MMAP2     69 /* mmap2(len, prot, fd, offset): as SYS_MMAP; fd may be a shared memory fd
                            (shared), offset page aligned -> address */
#define SYS_FDFLAGS   70 /* fdflags(fd, set): -1 reads; else FD_CLOEXEC (1) on/off -> the flags */
#define SYS_EXEPATH   71 /* exepath(buf, size): this program's file (/proc/self/exe) -> length */
#define SYS_FDPATH    73 /* fdpath(fd, buf, size): the path of an open file (one received from another program too) -> length */
#define SYS_MMAPAT    72 /* mmapat(addr, len, prot, fd): MAP_FIXED; fd -1 zeroed, or shared memory -> addr */
#define SYS_POLLWAIT  67 /* pollwait(gen, ms, net): gen -1 -> the I/O counter; else sleep until it is
                            not gen (a pipe or local socket changed), the next interrupt (net = 1:
                            TCP sockets are watched too) or ms pass -> the counter */
#define SYS_SYSTEM    62 /* system(op, a, b): Wi-Fi, volume, battery, power, op = SYSTEM_* */

#define SYSTEM_WIFI_STATUS  0  /* a = struct os_wifi * (the network joined)  -> 1 connected, 0 not, SYSERR_NOENT no card */
#define SYSTEM_WIFI_SCAN    1  /* a = struct os_wifi *, b = max: networks heard, strongest first -> count */
#define SYSTEM_WIFI_CONNECT 2  /* a = name, b = password ("" for open)   -> 0 */
#define SYSTEM_WIFI_OFF     3  /* leave the network                         -> 0 */
#define SYSTEM_VOLUME       4  /* a = 0-100 to set, -1 to read               -> volume, SYSERR_NOENT no sound */
#define SYSTEM_BATTERY      5  /* -> percent | 0x100 charging, SYSERR_NOENT no battery */
#define SYSTEM_POWER        6  /* a = 0 shut down, 1 restart                 -> only on failure */
#define SYSTEM_SETUP        7  /* a = name, b = password: the first accounts -> 0; a = 0: 1 if needed */
#define SYSTEM_SETUP_TEXT   8  /* the first-start questions on the console   -> 0 */
#define SYSTEM_LOG          9  /* a = struct os_log *: the system log         -> bytes copied */
#define SYSTEM_CPUS        10  /* -> how many cores run programs */
#define SYSTEM_POINTER     11  /* a = struct os_pointer *, b = 1 set, 0 read -> 0 */
#define SYSTEM_PAGES       12  /* a = 0 all, 1 free: memory in 4 KiB pages */

/* The mouse and the touchpad (System Settings). Speeds 0..100, 50 = normal. */
struct os_pointer {
    int mouse_speed, pad_speed;
    int accel;             /* faster movement goes further */
    int natural;           /* touchpad: the content follows the fingers */
    int wheel_reverse;     /* mouse: the wheel the other way */
    int scroll_speed;      /* 1..4: lines (wheel steps) per step */
    int tap_click;         /* touchpad: tap to click */
    int tap_right;         /* touchpad: tap with two fingers for a right click */
    int tap_drag;          /* touchpad: tap, then touch again and move, to drag */
    int swap_buttons;      /* mouse: the right button is the primary one */
    int has_mouse, has_pad;   /* read only: what was found */
};

struct os_log {
    unsigned long from;    /* in: the byte to start at (since power-on); out: past the last copied */
    char *buf;
    int max;
};

struct os_wifi {
    char name[33];
    int signal;            /* dBm: -50 strong, -80 weak */
    int secured;
    char address[16];      /* SYSTEM_WIFI_STATUS: our IP address */
};

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
#define SYSERR_WOULDBLOCK -29 /* non-blocking: nothing to read, no room, nobody to accept */

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
#define GFX_BEGIN    1   /* take the screen below the status bar (cleared to black); arg 1: the whole screen -> 0 */
#define GFX_BLIT     2   /* arg = struct os_blit *: 0x00RRGGBB pixels -> 0 */
#define GFX_END      3   /* give it back: the console redraws (also when the program ends) */
#define GFX_PRESENT  5   /* show what was blitted at the next refresh, after the frame before -> 1, 0: no page flipping */
#define GFX_SURFACE  6   /* arg = struct os_blit * (the whole screen, x = y = 0): the frame the
                            blits come from; the GPU copies from it -> 0, SYSERR_NOENT: no blitter */
#define GFX_SYNC     7   /* wait until the GPU has read the frame given to GFX_SURFACE (draw after) -> 0 */
#define GFX_COPY     8   /* arg = struct os_copy_list *: window contents into that frame by the GPU,
                            done at the next GFX_PRESENT (after your drawing) -> 0, else draw them */
#define GFX_CURSOR   4   /* arg = struct os_cursor *: the kernel draws the pointer (0: stop) -> 0 */
#define GFX_GPU      9   /* arg = struct os_gpu_op *: do it with the GPU's shaders (docs/GPU.md)
                            -> 0 done, else do it on the CPU (no GPU, or not now) */
#define GFX_WAIT     11  /* wait until the frame presented last is on the screen (then read input, draw) -> 0 */
#define GFX_DEPTH    12  /* -> the bits a colour the screen shows (6 on many laptop panels: dither for it; 8 if unknown) */
#define GFX_GPU_DRAW 10  /* arg = struct os_gpu_draws *: drawings onto a canvas by the GPU, in order,
                            one batch -> 0 done, else draw them all on the CPU */

struct os_gfx_info {
    int width, height;     /* the whole screen, in pixels */
    int top;               /* first row below the status bar */
    int cell_w, cell_h;    /* one text cell */
};

struct os_cursor {
    int w, h;              /* at most 64 x 64 */
    int hot_x, hot_y;      /* the point that is the pointer's position */
    const unsigned int *pixels;   /* 0xAARRGGBB, w * h */
};

struct os_blit {
    int x, y, w, h;        /* where on the screen, and the size */
    int stride;            /* pixels from one row of `pixels` to the next */
    const unsigned int *pixels;
};

struct os_copy {           /* GFX_COPY: from (sx, sy) in `pixels` to (x, y) in the frame, w x h */
    const unsigned int *pixels;
    int stride, rows;      /* the source: its row length (pixels) and number of rows */
    int x, y, w, h, sx, sy;
};

struct os_copy_list {
    int n;                 /* at most 64 */
    const struct os_copy *items;
};

/* GFX_GPU. GPU_MATERIAL (frosted glass, the window server's material()):
 * samp (bw x bh pixels, 0x00RRGGBB) is blurred (four 3-tap box passes;
 * scratch is the same size), saturated (sat %), tinted (tint over it, alpha
 * 0-255), then rows j0..j1 of a w-wide picture bilinearly upscaled from it
 * (with `bleed`) go to out, row j at out + j * w. GPU_FADE: w x h pixels of
 * out (`pitch` a row) become old (w x h, packed) blended towards them by f/256. */
enum { GPU_MATERIAL = 1, GPU_OP_FADE = 2 };
struct os_gpu_op {
    int op;
    unsigned int *samp, *scratch, *out;
    const unsigned int *old;
    int bw, bh, w, h, j0, j1, bleed, pitch;
    unsigned int sat, tint, alpha, f;
};

/* GFX_GPU_DRAW: each as ui.c / the window server draws it, clipped.
 * GD_SHADOW: ui_shadow_ex of x, y, w, h (r, size, alpha; flags 1: outside only).
 * GD_OVER: a picture with premultiplied alpha (0xAARRGGBB) at x, y (w x h,
 *   src_stride a row) over the canvas, except the hole [hx0, hx1) x [hy0, hy1).
 * GD_ROUND: ui_copy_round of src (src_stride) to x, y, w x h (r; flags: corners).
 * GD_SCALED: the src_w x src_h picture and its shape (mask: 255 inside, the
 *   same layout) bilinear into x, y, w x h with opacity alpha; under (or 0): what
 *   was there before, the clipped rectangle's size, packed.
 * GD_FILL: ui_round_ex of x, y, w, h (r, color, alpha; flags: corners).
 * GD_KEEP in flags: src and mask unchanged since the GPU last read them. */
enum { GD_SHADOW = 1, GD_OVER, GD_ROUND, GD_SCALED, GD_FILL };
#define GD_KEEP 0x100
struct os_gpu_draw {
    int op, flags;
    int x, y, w, h;
    int r, size, alpha;
    unsigned int color;
    const unsigned int *src, *mask, *under;
    int src_w, src_h, src_stride;
    int hx0, hy0, hx1, hy1;
};
#define GD_MAX 12
struct os_gpu_draws {
    unsigned int *px;                  /* the canvas: pixel (0, 0) */
    int stride, cx0, cy0, cx1, cy1;    /* pixels a row; the clip */
    int n;
    struct os_gpu_draw op[GD_MAX];
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

#define SOCK_READABLE  1   /* data waiting, the peer closed, or an error */
#define SOCK_WRITABLE  2   /* connected, room to send */
#define SOCK_HANGUP    4   /* the peer closed */
#define SOCK_ERROR     8   /* reset, refused */
#define SOCK_LISTENING 16
#define SOCK_LOCAL     32   /* (fdinfo) a local socket, not a pipe or file */
#define SOCK_SHM       64   /* (fdinfo) shared memory (SYS_SHMFD) */

/* Shared memory descriptors (memfd_create, shm_open): mmap2 maps them shared;
 * they pass to other programs like any descriptor. */
#define SHMFD_NEW      0   /* a = name (0: anonymous; else opened if it exists), b = size -> fd */
#define SHMFD_RESIZE   1   /* a = fd, b = size (ftruncate) -> 0 */
#define SHMFD_SIZE     2   /* a = fd -> size */
#define SHMFD_UNLINK   3   /* a = name -> 0, SYSERR_NOENT */
#define SHMFD_EXISTS   4   /* a = name -> 1 or 0 */

/* Local sockets: kernel descriptors; data with read()/write(). A name is only
 * a key in the kernel's table, not a file. */
#define USOCK_NEW      0   /* -> fd, not connected yet */
#define USOCK_PAIR     1   /* a = int fds[2], connected to each other -> 0 */
#define USOCK_BIND     2   /* a = fd, b = name -> 0 */
#define USOCK_LISTEN   3   /* a = fd (bound) -> 0, SYSERR_INUSE */
#define USOCK_CONNECT  4   /* a = fd, b = name -> 0, SYSERR_REFUSED */
#define USOCK_ACCEPT   5   /* a = listening fd -> new fd, SYSERR_WOULDBLOCK */
#define USOCK_SENDFDS  6   /* a = fd, b = const int *fds, c = count: they arrive with the next data -> 0 */
#define USOCK_RECVFDS  7   /* a = fd, b = int *fds, c = room -> count received */
#define USOCK_NONBLOCK 8   /* a = fd (any pipe or local socket), b = 1 on / 0 off -> 0 */
#define USOCK_NAME     9   /* a = fd, b = char[108]: its name, or the listener it connected to -> 0 */
#define USOCK_MAX_FDS 16   /* passed files waiting, each way */

struct os_sockinfo {
    unsigned int available;    /* bytes waiting to be read */
    unsigned short lport, rport;
    unsigned char rip[4];      /* the peer's address */
};

struct os_uname {
    char sysname[24];      /* "Terminal OS" */
    char release[16];      /* "0.9" */
    char hostname[32];     /* "terminal-os" */
};

#endif
