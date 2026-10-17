// shmcheck: shared memory as file descriptors (memfd_create's kernel side),
// mapped by two programs at once; and a program over 8 MB (this one, with its
// table below), which the kernel maps from its file page by page.
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <terminalos/syscall.h>
#include <unistd.h>

#define SYS_SHMFD 68
#define SYS_MMAP2 69
#define SHMFD_NEW 0
#define SHMFD_RESIZE 1
#define SHMFD_SIZE 2
#define SHMFD_UNLINK 3

static int failures;

static void check(const char *what, int ok) {
    printf("%s  %s\n", ok ? "ok    " : "FAILED", what);
    if (!ok) failures++;
}

// 9 MB of initialized data: the file is big, and its bytes must be right.
#define BIG (9 << 20)
__attribute__((used)) volatile unsigned char table[BIG] = {[0] = 1, [BIG / 2] = 2, [BIG - 1] = 3};
static unsigned char zeros[4 << 20];                 // (bss after it)

static long sc(long n, long a, long b, long c, long d) { return tos_syscall(n, a, b, c, d); }

int main(void) {
    printf("shmcheck: shared memory descriptors, a big program\n");
    check("big program: data from its file", table[0] == 1 && table[BIG / 2] == 2 && table[BIG - 1] == 3 &&
                                                 table[12345] == 0);
    zeros[sizeof(zeros) - 1] = 7;
    check("big program: zeroed memory after it", zeros[0] == 0 && zeros[sizeof(zeros) - 1] == 7);

    long fd = sc(SYS_SHMFD, SHMFD_NEW, 0, 0, 0);
    check("new shared memory (size 0)", fd >= 0 && sc(SYS_SHMFD, SHMFD_SIZE, fd, 0, 0) == 0);
    check("resize to 64 KiB", sc(SYS_SHMFD, SHMFD_RESIZE, fd, 65536, 0) == 0 &&
                                  sc(SYS_SHMFD, SHMFD_SIZE, fd, 0, 0) == 65536);
    char *m = (char *)sc(SYS_MMAP2, 65536, 3, fd, 0);
    check("map it", (long)m > 0);
    strcpy(m, "parent");

    // Through a socket to a forked child, which maps it at an offset and writes.
    int sv[2];
    socketpair(AF_UNIX, SOCK_STREAM, 0, sv);
    pid_t child = fork();
    if (child == 0) {
        char space[CMSG_SPACE(sizeof(int))], c;
        struct iovec iov = {&c, 1};
        struct msghdr h = {0};
        h.msg_iov = &iov;
        h.msg_iovlen = 1;
        h.msg_control = space;
        h.msg_controllen = sizeof(space);
        recvmsg(sv[1], &h, 0);
        int got;
        memcpy(&got, CMSG_DATA(CMSG_FIRSTHDR(&h)), sizeof(int));
        char *page = (char *)sc(SYS_MMAP2, 4096, 3, got, 4096);   // its second page
        char *all = (char *)sc(SYS_MMAP2, 0, 1, got, 0);
        strcpy(page, all[0] == 'p' ? "child saw parent" : "child saw nothing");
        _exit(0);
    }
    char space[CMSG_SPACE(sizeof(int))];
    memset(space, 0, sizeof(space));
    struct iovec iov = {"x", 1};
    struct msghdr h = {0};
    h.msg_iov = &iov;
    h.msg_iovlen = 1;
    h.msg_control = space;
    h.msg_controllen = sizeof(space);
    struct cmsghdr *c = CMSG_FIRSTHDR(&h);
    c->cmsg_level = SOL_SOCKET;
    c->cmsg_type = SCM_RIGHTS;
    c->cmsg_len = CMSG_LEN(sizeof(int));
    int ifd = (int)fd;
    memcpy(CMSG_DATA(c), &ifd, sizeof(int));
    sendmsg(sv[0], &h, 0);
    waitpid(child, 0, 0);
    printf("        page 2: \"%s\"\n", m + 4096);
    check("another program wrote into it (passed over a socket)", strcmp(m + 4096, "child saw parent") == 0);

    long named = sc(SYS_SHMFD, SHMFD_NEW, (long)"shmcheck", 8192, 0);
    long again = sc(SYS_SHMFD, SHMFD_NEW, (long)"shmcheck", 0, 0);
    char *a = (char *)sc(SYS_MMAP2, 8192, 3, named, 0), *b = (char *)sc(SYS_MMAP2, 8192, 3, again, 0);
    a[100] = 42;
    check("a named one, opened twice: the same memory", named >= 0 && again >= 0 && b[100] == 42);
    check("unlink the name", sc(SYS_SHMFD, SHMFD_UNLINK, (long)"shmcheck", 0, 0) == 0);

    printf("shmcheck: %s\n", failures ? "FAILED" : "all passed");
    return failures;
}
