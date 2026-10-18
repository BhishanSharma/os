/* Terminal OS system calls, as mlibc makes them: int 0x80, the number in rax,
 * arguments in rdi, rsi, rdx, r10. The numbers and structures are the
 * kernel's (src/intf/sys/syscall_nums.h); a negative result is an error. */
#ifndef _TERMINALOS_SYSCALL_H
#define _TERMINALOS_SYSCALL_H

#define TOS_EXIT        0
#define TOS_WRITE       1
#define TOS_READ        2
#define TOS_OPEN        3
#define TOS_CLOSE       4
#define TOS_SLEEP       6
#define TOS_UPTIME      7
#define TOS_TIME        8
#define TOS_UNLINK     12
#define TOS_GETUSER    13
#define TOS_GETPID     14
#define TOS_YIELD      15
#define TOS_WAIT       17
#define TOS_CHDIR      20
#define TOS_GETCWD     21
#define TOS_UNAME      24
#define TOS_RESOLVE    27
#define TOS_CONNECT    28
#define TOS_LISTEN     29
#define TOS_ACCEPT     30
#define TOS_SEND       31
#define TOS_RECV       32
#define TOS_SOCKCLOSE  33
#define TOS_SEEK       34
#define TOS_MKDIR      36
#define TOS_STAT       37
#define TOS_LISTDIR    38
#define TOS_MMAP       40
#define TOS_MUNMAP     41
#define TOS_FORK       43
#define TOS_EXEC       44
#define TOS_THREAD     45
#define TOS_THREAD_EXIT 46
#define TOS_FUTEX      47
#define TOS_PIPE       54
#define TOS_DUP        55
#define TOS_DUP2       56
#define TOS_SIGNAL     57
#define TOS_SIGSEND    58
#define TOS_SIGRETURN  59
#define TOS_SYSTEM     62
#define TOS_TCB_SET    63
#define TOS_SOCKINFO   64
#define TOS_FDINFO     65
#define TOS_USOCK      66
#define TOS_POLLWAIT   67
#define TOS_SHMFD      68
#define TOS_MMAP2      69
#define TOS_FDFLAGS    70
#define TOS_EXEPATH    71
#define TOS_MMAPAT     72
#define TOS_FDPATH     73

// sound(op, a, b): one 48 kHz, 16-bit stereo output stream (kernel SYS_SOUND).
#define TOS_SOUND      35
#define TOS_SOUND_OPEN    0   // -> 0, or an error (no sound card, or in use)
#define TOS_SOUND_WRITE   1   // (frames, count): waits for room -> frames queued
#define TOS_SOUND_QUEUED  2   // -> frames written but not heard yet
#define TOS_SOUND_CLOSE   3

#define TOS_SOCK_READABLE  1
#define TOS_SOCK_WRITABLE  2
#define TOS_SOCK_HANGUP    4
#define TOS_SOCK_ERROR     8
#define TOS_SOCK_LISTENING 16
#define TOS_SOCK_LOCAL     32
#define TOS_SOCK_SHM       64

#define TOS_SHMFD_NEW      0
#define TOS_SHMFD_RESIZE   1
#define TOS_SHMFD_SIZE     2
#define TOS_SHMFD_UNLINK   3
#define TOS_SHMFD_EXISTS   4

#define TOS_USOCK_NEW      0
#define TOS_USOCK_PAIR     1
#define TOS_USOCK_BIND     2
#define TOS_USOCK_LISTEN   3
#define TOS_USOCK_CONNECT  4
#define TOS_USOCK_ACCEPT   5
#define TOS_USOCK_SENDFDS  6
#define TOS_USOCK_RECVFDS  7
#define TOS_USOCK_NONBLOCK 8
#define TOS_USOCK_NAME     9
#define TOS_USOCK_MAX_FDS 16

#define TOS_SYSTEM_CPUS 10
#define TOS_SYSTEM_PAGES 12

#define TOS_OPEN_READ   0
#define TOS_OPEN_WRITE  1
#define TOS_OPEN_APPEND 2
#define TOS_WAIT_NOHANG 1
#define TOS_FUTEX_WAIT  0
#define TOS_FUTEX_WAKE  1

#define TOS_ERR_BADCALL  -1
#define TOS_ERR_FAULT    -2
#define TOS_ERR_BADFD    -3
#define TOS_ERR_NOENT    -4
#define TOS_ERR_NOMEM    -5
#define TOS_ERR_MFILE    -6
#define TOS_ERR_IO       -7
#define TOS_ERR_PERM     -8
#define TOS_ERR_NOEXEC   -9
#define TOS_ERR_AGAIN   -10
#define TOS_ERR_CHILD   -11
#define TOS_ERR_NOUSER  -12
#define TOS_ERR_AUTH    -13
#define TOS_ERR_SRCH    -14
#define TOS_ERR_TIMEOUT -15
#define TOS_ERR_REFUSED -16
#define TOS_ERR_RESET   -17
#define TOS_ERR_CLOSED  -18
#define TOS_ERR_NOROUTE -19
#define TOS_ERR_INUSE   -20
#define TOS_ERR_PIPE    -28
#define TOS_ERR_WOULDBLOCK -29

struct tos_sockinfo { unsigned int available; unsigned short lport, rport; unsigned char rip[4]; };

struct tos_time { int year, month, day, hour, minute, second; char zone[8]; };
struct tos_user { unsigned int uid; char name[16]; char home[48]; };
struct tos_dirent { char name[256]; unsigned char is_dir; unsigned int size; };
struct tos_stat { unsigned int size; unsigned char is_dir; };
struct tos_uname { char sysname[24]; char release[16]; char hostname[32]; };

static inline long tos_syscall(long n, long a, long b, long c, long d) {
	long ret;
	register long r10 __asm__("r10") = d;
	__asm__ volatile("int $0x80" : "=a"(ret) : "a"(n), "D"(a), "S"(b), "d"(c), "r"(r10) : "memory");
	return ret;
}

#endif
