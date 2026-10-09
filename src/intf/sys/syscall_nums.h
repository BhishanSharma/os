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

/* open() modes */
#define OPEN_READ    0
#define OPEN_WRITE   1   /* create or truncate */
#define OPEN_APPEND  2   /* create, or add to the end */

/* console() operations */
#define CON_CLEAR    0   /* clear the screen, cursor to 0,0 */
#define CON_GOTO     1   /* a = column, b = row */
#define CON_COLOR    2   /* a = foreground, b = background (VGA colours 0-15) */
#define CON_RESET    3   /* back to the theme colours */
#define CON_SIZE     4   /* -> columns << 16 | rows */
#define CON_CURSOR   5   /* a = 1 show, 0 hide the cursor */

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

struct os_time {
    int year, month, day, hour, minute, second;
    char zone[8];          /* "IST" */
};

struct os_dirent {
    char name[13];         /* 8.3 name, e.g. "HELLO.ELF" */
    unsigned char is_dir;
    unsigned int size;
};

#endif
