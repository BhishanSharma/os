// os2.c - the system calls of stages 1-6: memory, programs, threads, ports,
// descriptors, signals, the device tree, login
#include <os.h>

static inline long os_syscall5(long n, long a, long b, long c, long d, long e) {
    long ret;
    register long r10 __asm__("r10") = d;
    register long r8 __asm__("r8") = e;
    __asm__ volatile("int $0x80" : "=a"(ret) : "a"(n), "D"(a), "S"(b), "d"(c), "r"(r10), "r"(r8) : "memory");
    return ret;
}

/* ---- Memory ---- */

void *mmap(size_t len, int prot, int fd) {
    long r = os_syscall(SYS_MMAP, (long)len, prot, fd);
    return r < 0 ? (void *)0 : (void *)r;
}
int munmap(void *addr)                          { return (int)os_syscall(SYS_MUNMAP, (long)addr, 0, 0); }
void *shm_map(const char *name, size_t size) {
    long r = os_syscall(SYS_SHMMAP, (long)name, (long)size, 0);
    return r < 0 ? (void *)0 : (void *)r;
}

/* ---- Programs ---- */

int fork(void)                                  { return (int)os_syscall(SYS_FORK, 0, 0, 0); }
int exec(const char *path, char *const argv[])  { return (int)os_syscall(SYS_EXEC, (long)path, (long)argv, 0); }

/* ---- Threads ---- */

struct thread_start { void (*fn)(void *); void *arg; };

static void thread_entry(struct thread_start *s) {
    s->fn(s->arg);
    thread_exit(0);
}

int thread_create(void (*fn)(void *), void *arg) {
    const size_t size = 256 * 1024;
    char *stack = mmap(size, PROT_READ | PROT_WRITE, -1);
    if (!stack) return SYSERR_NOMEM;
    struct thread_start *s = (struct thread_start *)(stack + size) - 1;   /* at the top of its stack */
    s->fn = fn;
    s->arg = arg;
    return (int)os_syscall(SYS_THREAD, (long)thread_entry, (long)s, (long)s);
}

void thread_exit(int code) {
    os_syscall(SYS_THREAD_EXIT, code, 0, 0);
    for (;;) { }
}

int futex(int op, volatile int *addr, int value) { return (int)os_syscall(SYS_FUTEX, op, (long)addr, value); }

/* 0 free, 1 locked, 2 locked with waiters (Drepper, "Futexes Are Tricky"). */
void mutex_lock(mutex_t *m) {
    int c = __sync_val_compare_and_swap(&m->v, 0, 1);
    if (c == 0) return;
    if (c != 2) c = __sync_lock_test_and_set(&m->v, 2);
    while (c != 0) {
        futex(FUTEX_WAIT, &m->v, 2);
        c = __sync_lock_test_and_set(&m->v, 2);
    }
}

void mutex_unlock(mutex_t *m) {
    if (__sync_fetch_and_sub(&m->v, 1) != 1) {
        m->v = 0;
        futex(FUTEX_WAKE, &m->v, 1);
    }
}

/* ---- Ports ---- */

int port_create(void)                           { return (int)os_syscall(SYS_PORT_CREATE, 0, 0, 0); }
int port_send(int port, const void *msg, size_t len, int reply) {
    return (int)os_syscall4(SYS_PORT_SEND, port, (long)msg, (long)len, reply);
}
int port_receive(int port, void *buf, size_t max, unsigned long timeout_ms, int *reply) {
    return (int)os_syscall5(SYS_PORT_RECEIVE, port, (long)buf, (long)max, (long)timeout_ms, (long)reply);
}
int port_register(const char *service, int port) { return (int)os_syscall(SYS_PORT_REGISTER, (long)service, port, 0); }
int port_lookup(const char *service)            { return (int)os_syscall(SYS_PORT_LOOKUP, (long)service, 0, 0); }
int port_close(int port)                        { return (int)os_syscall(SYS_PORT_CLOSE, port, 0, 0); }

/* A request and its reply: a reply port made once per program. */
int port_call(int port, const void *msg, size_t len, void *reply_buf, size_t max, unsigned long timeout_ms) {
    static int reply_port = -1;
    if (reply_port < 0 && (reply_port = port_create()) < 0) return reply_port;
    int r = port_send(port, msg, len, reply_port);
    if (r < 0) return r;
    int dummy;
    return port_receive(reply_port, reply_buf, max, timeout_ms, &dummy);
}

/* ---- Descriptors ---- */

int pipe(int fds[2])                            { return (int)os_syscall(SYS_PIPE, (long)fds, 0, 0); }
int dup(int fd)                                 { return (int)os_syscall(SYS_DUP, fd, 0, 0); }
int dup2(int fd, int to)                        { return (int)os_syscall(SYS_DUP2, fd, to, 0); }

/* ---- Signals ---- */

static void signal_trampoline(int sig, void (*handler)(int), void *frame) {
    handler(sig);
    os_syscall(SYS_SIGRETURN, (long)frame, 0, 0);
    for (;;) { }
}

int signal(int sig, void (*handler)(int)) {
    return (int)os_syscall(SYS_SIGNAL, sig, (long)handler, (long)signal_trampoline);
}
int sigsend(int pid, int sig)                   { return (int)os_syscall(SYS_SIGSEND, pid, sig, 0); }

/* ---- The device tree, login ---- */

int registry(int index, struct os_regnode *node) { return (int)os_syscall(SYS_REGISTRY, index, (long)node, 0); }
int auth(const char *name, const char *password, struct os_user *u) {
    return (int)os_syscall(SYS_AUTH, (long)name, (long)password, (long)u);
}
void console_echo(int on)                       { os_syscall(SYS_CONSOLE, CON_ECHO, on, 0); }

/* ---- System menus ---- */

int wifi_status(struct os_wifi *w)              { return (int)os_syscall(SYS_SYSTEM, SYSTEM_WIFI_STATUS, (long)w, 0); }
int wifi_scan(struct os_wifi *list, int max)    { return (int)os_syscall(SYS_SYSTEM, SYSTEM_WIFI_SCAN, (long)list, max); }
int wifi_connect(const char *name, const char *password) {
    return (int)os_syscall(SYS_SYSTEM, SYSTEM_WIFI_CONNECT, (long)name, (long)(password ? password : ""));
}
int wifi_disconnect(void)                       { return (int)os_syscall(SYS_SYSTEM, SYSTEM_WIFI_OFF, 0, 0); }
int volume(int set)                             { return (int)os_syscall(SYS_SYSTEM, SYSTEM_VOLUME, set, 0); }
int battery(void)                               { return (int)os_syscall(SYS_SYSTEM, SYSTEM_BATTERY, 0, 0); }
int power(int restart)                          { return (int)os_syscall(SYS_SYSTEM, SYSTEM_POWER, restart, 0); }
int setup_needed(void)                          { return (int)os_syscall(SYS_SYSTEM, SYSTEM_SETUP, 0, 0); }
int setup_accounts(const char *name, const char *password) {
    return (int)os_syscall(SYS_SYSTEM, SYSTEM_SETUP, (long)name, (long)password);
}
void setup_text(void)                           { os_syscall(SYS_SYSTEM, SYSTEM_SETUP_TEXT, 0, 0); }
