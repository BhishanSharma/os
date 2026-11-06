// ptytest - checks the pseudo-terminals (USOCK_PTY): a cooked line, Ctrl+D
// as an end of file, raw mode with echo, Ctrl+C as SIGINT, the window size.
// Prints "ptytest: all ok" (system test `kernel`).
#include <stdio.h>
#include <string.h>
#include <os.h>

static int failures;
static volatile int got_int;

static void check(const char *what, int ok) {
    printf("ptytest: %-22s %s\n", what, ok ? "ok" : "FAILED");
    if (!ok) failures++;
}

static void on_int(int sig) { (void)sig; got_int = 1; }

int main(void) {
    int fd[2];
    if (pty(fd) < 0) {
        printf("ptytest: no pty\n");
        return 1;
    }
    int m = fd[0], s = fd[1];
    char buf[64];
    check("an end each side", (os_syscall(SYS_FDINFO, m, 0, 0) & SOCK_PTY) && (os_syscall(SYS_FDINFO, s, 0, 0) & SOCK_PTY));

    write(m, "hello\n", 6);                            /* cooked: the window sends a line */
    long n = read(s, buf, sizeof(buf));
    check("a cooked line", n == 6 && !memcmp(buf, "hello\n", 6));

    write(m, "\x04", 1);                               /* Ctrl+D: one read returns 0 */
    check("Ctrl+D: end of file", read(s, buf, sizeof(buf)) == 0);

    struct os_termios t;
    pty_mode(s, &t);
    t.lflag &= ~PTY_ICANON;                            /* raw, with echo and Ctrl+C */
    os_syscall(SYS_USOCK, USOCK_TCSET, s, (long)&t);
    struct os_termios now;
    check("raw mode set", pty_mode(m, &now) == 0 && !(now.lflag & PTY_ICANON));
    write(m, "ab", 2);
    n = read(s, buf, sizeof(buf));
    check("raw keys", n == 2 && !memcmp(buf, "ab", 2));
    n = read(m, buf, sizeof(buf));
    check("raw echo", n == 2 && !memcmp(buf, "ab", 2));

    signal(SIGINT, on_int);
    write(m, "\x03", 1);                               /* Ctrl+C: SIGINT for who set the mode */
    for (int i = 0; i < 20 && !got_int; i++) sleep_ms(5);
    check("Ctrl+C: SIGINT", got_int);

    pty_size(m, 132, 43);
    struct os_winsize w;
    os_syscall(SYS_USOCK, USOCK_WINSZ, s, (long)&w);
    check("window size", w.cols == 132 && w.rows == 43);

    close(m);
    check("master closed: eof", read(s, buf, sizeof(buf)) == 0);
    close(s);
    printf("ptytest: %s\n", failures ? "FAILED" : "all ok");
    return failures != 0;
}
