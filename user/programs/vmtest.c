// vmtest [overflow] - virtual memory: pages on first use, a file mapping,
// shared memory between two programs, copy-on-write (fork); `overflow`
// recurses until the stack runs out (the kernel says "Stack overflow").
#include <stdio.h>
#include <string.h>
#include <os.h>

static int failed;

static void check(int ok, const char *what) {
    printf("  %-44s %s\n", what, ok ? "ok" : "FAILED");
    if (!ok) failed = 1;
}

#pragma GCC diagnostic ignored "-Winfinite-recursion"
static int deep(int n) {
    volatile char frame[1024];
    frame[0] = (char)n;
    return deep(n + 1) + frame[0];
}

int main(int argc, char **argv) {
    if (argc > 1 && strcmp(argv[1], "overflow") == 0) return deep(0);
    if (argc > 1 && strcmp(argv[1], "child") == 0) {          /* the other side of the shared memory */
        int *s = shm_map("vmtest", 4096);
        if (!s) return 1;
        s[1] = s[0] * 2;
        return 0;
    }

    struct os_meminfo before, after;
    meminfo(&before);
    char *big = mmap(512u << 20, PROT_READ | PROT_WRITE, -1);
    for (int i = 0; big && i < 8; i++) big[(long)i << 26] = (char)(i + 1);
    meminfo(&after);
    int sparse_ok = big != 0;
    for (int i = 0; big && i < 8; i++) sparse_ok &= big[(long)i << 26] == (char)(i + 1);
    unsigned long used = (after.pages_used - before.pages_used) >> 10;
    printf("512 MiB mapped, 8 pages touched: %lu KiB of memory used\n", used);
    check(sparse_ok && used < 1024, "memory is used only where it is touched");
    munmap(big);

    int fd = open("vmfile.txt", OPEN_WRITE);
    write(fd, "hello from a mapped file", 24);
    close(fd);
    fd = open("vmfile.txt", OPEN_READ);
    char *m = mmap(0, PROT_READ, fd);
    close(fd);
    check(m && memcmp(m, "hello from a mapped file", 24) == 0, "a file mapped into memory");
    unlink("vmfile.txt");

    int *s = shm_map("vmtest", 4096);
    if (s) s[0] = 21;
    char *args[] = { "vmtest", "child", 0 };
    int pid = spawn("vmtest", args, 0), status = -1;
    if (pid > 0) wait(pid, &status, 0);
    check(s && status == 0 && s[1] == 42, "shared memory between two programs");

    int x = 1;
    pid = fork();
    if (pid == 0) {
        x = 2;                                               /* the child's own copy */
        exit(x);
    }
    status = -1;
    if (pid > 0) wait(pid, &status, 0);
    check(pid > 0 && status == 2 && x == 1, "fork: copy-on-write");

    printf(failed ? "vmtest: FAILED\n" : "vmtest: all good\n");
    return failed;
}
