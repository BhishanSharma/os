// ipccheck: local (Unix-domain) sockets and poll(), the way a browser's
// processes talk: socketpair across fork, a named listener, files passed with
// SCM_RIGHTS, non-blocking reads, poll() waiting. The exit code is the number
// of failures.
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static int failures;

static void check(const char *what, int ok) {
    printf("%s  %s\n", ok ? "ok    " : "FAILED", what);
    if (!ok) failures++;
}

static long ms_now(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

static int send_fd(int sock, int fd, const char *msg) {
    char space[CMSG_SPACE(sizeof(int))];
    memset(space, 0, sizeof(space));
    struct iovec iov = {(void *)msg, strlen(msg)};
    struct msghdr h = {0};
    h.msg_iov = &iov;
    h.msg_iovlen = 1;
    h.msg_control = space;
    h.msg_controllen = sizeof(space);
    struct cmsghdr *c = CMSG_FIRSTHDR(&h);
    c->cmsg_level = SOL_SOCKET;
    c->cmsg_type = SCM_RIGHTS;
    c->cmsg_len = CMSG_LEN(sizeof(int));
    memcpy(CMSG_DATA(c), &fd, sizeof(int));
    return sendmsg(sock, &h, 0) == (ssize_t)strlen(msg) ? 0 : -1;
}

static int recv_fd(int sock, char *buf, size_t len) {
    char space[CMSG_SPACE(sizeof(int))];
    struct iovec iov = {buf, len - 1};
    struct msghdr h = {0};
    h.msg_iov = &iov;
    h.msg_iovlen = 1;
    h.msg_control = space;
    h.msg_controllen = sizeof(space);
    ssize_t n = recvmsg(sock, &h, 0);
    buf[n > 0 ? n : 0] = 0;
    struct cmsghdr *c = CMSG_FIRSTHDR(&h);
    if (n <= 0 || !c || c->cmsg_type != SCM_RIGHTS) return -1;
    int fd;
    memcpy(&fd, CMSG_DATA(c), sizeof(int));
    return fd;
}

int main(void) {
    printf("ipccheck: local sockets, file passing, poll\n");
    char buf[64];

    // socketpair across fork: the child echoes in capitals.
    int sv[2];
    check("socketpair", socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
    pid_t child = fork();
    if (child == 0) {
        close(sv[0]);
        ssize_t n = read(sv[1], buf, sizeof(buf));
        for (ssize_t i = 0; i < n; i++)
            if (buf[i] >= 'a' && buf[i] <= 'z') buf[i] -= 32;
        write(sv[1], buf, n);
        // A pipe end, passed back: the parent reads what is written here.
        int p[2];
        pipe(p);
        send_fd(sv[1], p[0], "pipe");
        close(p[0]);
        usleep(300 * 1000);                       // (the parent's poll waits for this)
        write(p[1], "through the pipe", 16);
        close(p[1]);
        close(sv[1]);
        _exit(0);
    }
    close(sv[1]);
    write(sv[0], "hello", 5);
    ssize_t n = read(sv[0], buf, sizeof(buf) - 1);
    buf[n > 0 ? n : 0] = 0;
    check("socketpair across fork: \"hello\" -> \"HELLO\"", strcmp(buf, "HELLO") == 0);

    int got = recv_fd(sv[0], buf, sizeof(buf));
    check("SCM_RIGHTS: a pipe passed from the child", got >= 0 && strcmp(buf, "pipe") == 0);
    if (got >= 0) {
        fcntl(got, F_SETFL, O_NONBLOCK);
        check("non-blocking read: EAGAIN", read(got, buf, sizeof(buf)) < 0 && errno == EAGAIN);
        struct pollfd pf = {got, POLLIN, 0};
        long t0 = ms_now();
        int r = poll(&pf, 1, 3000);
        long waited = ms_now() - t0;
        n = read(got, buf, sizeof(buf) - 1);
        buf[n > 0 ? n : 0] = 0;
        printf("        poll waited %ld ms\n", waited);
        check("poll() wakes when the data comes", r == 1 && (pf.revents & POLLIN) && waited >= 200 &&
                                                       waited < 2000 && strcmp(buf, "through the pipe") == 0);
        close(got);
    }
    int status = 0;
    waitpid(child, &status, 0);
    struct pollfd pf = {sv[0], POLLIN, 0};
    check("hang-up seen when the child ends", poll(&pf, 1, 1000) == 1 && read(sv[0], buf, 1) == 0);
    close(sv[0]);

    // A named listener: bind, listen, connect, accept.
    struct sockaddr_un addr = {0};
    addr.sun_family = AF_UNIX;
    strcpy(addr.sun_path, "/tmp/ipccheck.sock");
    int ls = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0);
    int ok = ls >= 0 && bind(ls, (struct sockaddr *)&addr, sizeof(addr)) == 0 && listen(ls, 4) == 0;
    check("bind + listen", ok);
    check("accept with nobody waiting: EAGAIN", accept(ls, 0, 0) < 0 && errno == EAGAIN);
    int c = socket(AF_UNIX, SOCK_STREAM, 0);
    check("connect", connect(c, (struct sockaddr *)&addr, sizeof(addr)) == 0);
    pf = (struct pollfd){ls, POLLIN, 0};
    check("poll: the listener is readable", poll(&pf, 1, 1000) == 1);
    int s = accept(ls, 0, 0);
    write(c, "ping", 4);
    n = read(s, buf, sizeof(buf) - 1);
    buf[n > 0 ? n : 0] = 0;
    check("accept + talk", s >= 0 && strcmp(buf, "ping") == 0);
    int c2 = socket(AF_UNIX, SOCK_STREAM, 0);
    struct sockaddr_un none = addr;
    strcpy(none.sun_path, "/tmp/nobody.sock");
    check("connect to no one: ECONNREFUSED",
          connect(c2, (struct sockaddr *)&none, sizeof(none)) < 0 && errno == ECONNREFUSED);
    close(c2);
    close(s);
    close(c);
    close(ls);

    printf("ipccheck: %s\n", failures ? "FAILED" : "all passed");
    return failures;
}
