// attack <kind> - try what the kernel must refuse, and say what happened
// (the system tests run it: each line ends in "refused" or "NOT REFUSED").
//   elf        a program file whose segment offset wraps around 2^64
//   sigreturn  return from a "signal handler" to a kernel address
//   tramp      a signal trampoline in the kernel
//   port       take over the "echo" service (launchd's echod, as root)
#include <stdio.h>
#include <string.h>
#include <os.h>

static void say(const char *what, int refused) {
    printf("attack %s: %s\n", what, refused ? "refused" : "NOT REFUSED");
}

static void put64(unsigned char *p, unsigned long v) {
    for (int i = 0; i < 8; i++) p[i] = (unsigned char)(v >> (8 * i));
}

/* One PT_LOAD whose offset is 64 KiB "before" the file: offset + filesz wraps
 * to the file's own size, which the old check let through (the loader then
 * copied kernel memory into the program). */
static int evil_elf(void) {
    unsigned char f[120];
    memset(f, 0, sizeof(f));
    f[0] = 0x7F; f[1] = 'E'; f[2] = 'L'; f[3] = 'F'; f[4] = 2; f[5] = 1; f[6] = 1;
    f[16] = 2; f[18] = 62; f[20] = 1;                     /* ET_EXEC, x86-64, version 1 */
    put64(f + 24, 0x400000);                              /* entry */
    put64(f + 32, 64);                                    /* phoff */
    f[52] = 64; f[54] = 56; f[56] = 1;                    /* ehsize, phentsize, phnum */
    unsigned char *ph = f + 64;
    ph[0] = 1; ph[4] = 5;                                 /* PT_LOAD, R+X */
    put64(ph + 8, (unsigned long)-0x10000);               /* offset */
    put64(ph + 16, 0x400000);                             /* vaddr */
    put64(ph + 32, 0x10000 + sizeof(f));                  /* filesz: offset + filesz = 120 */
    put64(ph + 40, 0x10000 + sizeof(f));                  /* memsz */
    int fd = open("evilelf", OPEN_WRITE);
    if (fd < 0 || write(fd, f, sizeof(f)) != (long)sizeof(f)) return -1;
    close(fd);
    char *argv[] = { "evilelf", 0 };
    int pid = spawn("evilelf", argv, 0), status = 0;
    if (pid > 0) wait(pid, &status, 0);
    unlink("evilelf");
    return pid;
}

static void handler(int sig) { (void)sig; }

int main(int argc, char **argv) {
    const char *kind = argc > 1 ? argv[1] : "";
    if (!strcmp(kind, "elf")) {
        say("elf", evil_elf() < 0);
    } else if (!strcmp(kind, "sigreturn")) {
        unsigned long frame[22];
        memset(frame, 0, sizeof(frame));
        frame[17] = 0x8000000000000000ul;                 /* rip: not canonical */
        printf("attack sigreturn: trying\n");
        os_syscall(SYS_SIGRETURN, (long)frame, 0, 0);    /* the kernel must end only us */
        say("sigreturn", 0);
    } else if (!strcmp(kind, "tramp")) {
        long r = os_syscall(SYS_SIGNAL, SIGUSR1, (long)handler, (long)0xFFFF800000000000ul);
        say("tramp", r < 0);
    } else if (!strcmp(kind, "port")) {
        int port = port_create();
        say("port", port >= 0 && port_register("echo", port) != 0);
    } else {
        printf("attack elf|sigreturn|tramp|port\n");
        return 1;
    }
    return 0;
}
