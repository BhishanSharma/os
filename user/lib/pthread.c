// pthread.c - POSIX threads over the system's threads and futexes.
//
// A thread's stack is 1 MiB (FFmpeg's decoders want more than the 256 KiB
// of thread_create). Joining waits on a futex the thread sets as it ends;
// the system frees the ended thread itself.
#include <pthread.h>
#include <errno.h>
#include <stdlib.h>
#include <os.h>

#define STACK_DEFAULT (1024 * 1024)

struct pthread_info {
    void *(*fn)(void *);
    void *arg, *result;
    volatile int done;
};

static struct pthread_info main_thread;

static void entry(struct pthread_info *t) {
    t->result = t->fn(t->arg);
    t->done = 1;
    futex(FUTEX_WAKE, &t->done, 1);
    thread_exit(0);
}

static long raw_thread(struct pthread_info *t, size_t size) {
    char *stack = mmap(size, PROT_READ | PROT_WRITE, -1);
    if (!stack) return SYSERR_NOMEM;
    return os_syscall(SYS_THREAD, (long)entry, (long)t, (long)(stack + size));
}

int pthread_create(pthread_t *out, const pthread_attr_t *attr, void *(*fn)(void *), void *arg) {
    struct pthread_info *t = calloc(1, sizeof(*t));
    if (!t) return EAGAIN;
    t->fn = fn;
    t->arg = arg;
    size_t size = attr && attr->stack ? attr->stack : STACK_DEFAULT;
    if (size < STACK_DEFAULT) size = STACK_DEFAULT;
    if (raw_thread(t, (size + 4095) & ~(size_t)4095) < 0) {
        free(t);
        return EAGAIN;
    }
    *out = t;
    return 0;
}

int pthread_join(pthread_t t, void **result) {
    while (!t->done) futex(FUTEX_WAIT, &t->done, 0);
    if (result) *result = t->result;
    free(t);                                   /* (its stack stays mapped: it ran on it to the end) */
    return 0;
}

pthread_t pthread_self(void) { return &main_thread; }        /* (no thread-local storage: one answer) */

int pthread_attr_init(pthread_attr_t *a) { a->stack = 0; return 0; }
int pthread_attr_destroy(pthread_attr_t *a) { (void)a; return 0; }
int pthread_attr_setstacksize(pthread_attr_t *a, size_t size) { a->stack = size; return 0; }

/* ---- Mutexes: os.h's (0 free, 1 locked, 2 locked with waiters) ---------------------- */

int pthread_mutex_init(pthread_mutex_t *m, const pthread_mutexattr_t *attr) { (void)attr; m->v = 0; return 0; }
int pthread_mutex_destroy(pthread_mutex_t *m) { (void)m; return 0; }
int pthread_mutex_lock(pthread_mutex_t *m) { mutex_lock((mutex_t *)m); return 0; }
int pthread_mutex_unlock(pthread_mutex_t *m) { mutex_unlock((mutex_t *)m); return 0; }
int pthread_mutex_trylock(pthread_mutex_t *m) {
    return __sync_val_compare_and_swap(&m->v, 0, 1) == 0 ? 0 : EBUSY;
}

/* ---- Condition variables: a counter that changes on every signal ------------------ */

int pthread_cond_init(pthread_cond_t *c, const pthread_condattr_t *attr) { (void)attr; c->seq = 0; return 0; }
int pthread_cond_destroy(pthread_cond_t *c) { (void)c; return 0; }

int pthread_cond_wait(pthread_cond_t *c, pthread_mutex_t *m) {
    int seq = c->seq;
    pthread_mutex_unlock(m);
    futex(FUTEX_WAIT, &c->seq, seq);           /* returns at once if it was signalled meanwhile */
    pthread_mutex_lock(m);
    return 0;
}

int pthread_cond_timedwait(pthread_cond_t *c, pthread_mutex_t *m, const struct timespec *until) {
    struct timespec now;
    clock_gettime(CLOCK_REALTIME, &now);
    if (now.tv_sec > until->tv_sec || (now.tv_sec == until->tv_sec && now.tv_nsec >= until->tv_nsec))
        return ETIMEDOUT;
    pthread_cond_wait(c, m);                   /* (wakes within 100 ms anyway: the caller checks again) */
    return 0;
}

int pthread_cond_signal(pthread_cond_t *c) {
    __sync_fetch_and_add(&c->seq, 1);
    futex(FUTEX_WAKE, &c->seq, 1);
    return 0;
}

int pthread_cond_broadcast(pthread_cond_t *c) { return pthread_cond_signal(c); }   /* (the futex wakes all) */

/* ---- Once ----------------------------------------------------------------------- */

int pthread_once(pthread_once_t *once, void (*fn)(void)) {
    if (*once == 2) return 0;
    if (__sync_val_compare_and_swap(once, 0, 1) == 0) {
        fn();
        *once = 2;
        futex(FUTEX_WAKE, (volatile int *)once, 1);
        return 0;
    }
    while (*once != 2) futex(FUTEX_WAIT, (volatile int *)once, 1);
    return 0;
}
