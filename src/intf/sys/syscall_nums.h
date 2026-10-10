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
#define SYS_TIME     8   /* time(struct os_time *) local date and time   -> 0          */
#define SYS_GETKEY   9   /* getkey(): next key, 0 if none (no waiting)   -> key code   */
#define SYS_CONSOLE 10   /* console(op, a, b), op = CON_*                -> see below  */
#define SYS_READDIR 11   /* readdir(index, struct os_dirent *)           -> 1, or 0 past the end */
#define SYS_UNLINK  12   /* unlink(path)                                 -> 0          */
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
    char name[13];         /* 8.3 name, e.g. "HELLO.ELF" */
    unsigned char is_dir;
    unsigned int size;
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

/* getmouse() */
#define MOUSE_BUTTON_LEFT    1
#define MOUSE_BUTTON_RIGHT   2
#define MOUSE_BUTTON_MIDDLE  4

struct os_mouse {
    int col, row;          /* the cell under the pointer (gotoxy coordinates; row -1 = status bar) */
    int buttons;           /* MOUSE_BUTTON_* held now */
    int wheel;             /* wheel steps since the last call: negative = up */
};

struct os_uname {
    char sysname[24];      /* "Terminal OS" */
    char release[16];      /* "0.9" */
    char hostname[32];     /* "terminal-os" */
};

#endif
