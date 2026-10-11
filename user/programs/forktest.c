// forktest - fork, exec, wait, pipes between processes, and signals
#include <stdio.h>
#include <string.h>
#include <os.h>

static int failed;
static volatile int got_usr1, got_chld;

static void check(int ok, const char *what) {
    printf("  %-44s %s\n", what, ok ? "ok" : "FAILED");
    if (!ok) failed = 1;
}

static void on_usr1(int sig) { got_usr1 = sig; }
static void on_chld(int sig) { (void)sig; got_chld = 1; }

int main(void) {
    int status = -1, pid = fork();
    if (pid == 0) exit(7);
    if (pid > 0) wait(pid, &status, 0);
    check(pid > 0 && status == 7, "fork, then the child's exit code");

    int fds[2];
    pipe(fds);
    pid = fork();
    if (pid == 0) {                                     /* the child writes into the pipe */
        close(fds[0]);
        write(fds[1], "through a pipe", 14);
        exit(0);
    }
    close(fds[1]);
    char buf[32] = { 0 };
    long n = read(fds[0], buf, sizeof(buf) - 1);
    long end = read(fds[0], buf + 20, 4);               /* the writer is gone: end of file */
    close(fds[0]);
    wait(pid, &status, 0);
    check(n == 14 && strcmp(buf, "through a pipe") == 0 && end == 0, "a pipe from a child");

    signal(SIGCHLD, on_chld);
    pid = fork();
    if (pid == 0) {
        char *args[] = { "echo", "  (printed by echo, started with exec)", 0 };
        exec("echo", args);
        exit(99);                                       /* exec failed */
    }
    wait(pid, &status, 0);
    getpid();                                           /* a system call: pending signals run */
    check(status == 0, "exec");
    check(got_chld, "SIGCHLD when a child ends");

    signal(SIGUSR1, on_usr1);
    sigsend(getpid(), SIGUSR1);
    getpid();
    check(got_usr1 == SIGUSR1, "a signal handler");

    printf(failed ? "forktest: FAILED\n" : "forktest: all good\n");
    return failed;
}
