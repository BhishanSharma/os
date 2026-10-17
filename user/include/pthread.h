/* pthread.h - POSIX threads over the system's threads and futexes (os.h),
 * for ported code (FFmpeg decodes on several cores with them). Mutexes are
 * the plain kind; condition variables may wake spuriously (as POSIX allows). */
#ifndef PTHREAD_H
#define PTHREAD_H
#include <stddef.h>
#include <time.h>

typedef struct pthread_info *pthread_t;
typedef struct { size_t stack; } pthread_attr_t;
typedef struct { volatile int v; } pthread_mutex_t;
typedef struct { int kind; } pthread_mutexattr_t;
typedef struct { volatile int seq; } pthread_cond_t;
typedef struct { int unused; } pthread_condattr_t;
typedef volatile int pthread_once_t;

#define PTHREAD_MUTEX_INITIALIZER { 0 }
#define PTHREAD_COND_INITIALIZER { 0 }
#define PTHREAD_ONCE_INIT 0

int pthread_create(pthread_t *t, const pthread_attr_t *attr, void *(*fn)(void *), void *arg);
int pthread_join(pthread_t t, void **result);
pthread_t pthread_self(void);
int pthread_attr_init(pthread_attr_t *a);
int pthread_attr_destroy(pthread_attr_t *a);
int pthread_attr_setstacksize(pthread_attr_t *a, size_t size);

int pthread_mutex_init(pthread_mutex_t *m, const pthread_mutexattr_t *attr);
int pthread_mutex_destroy(pthread_mutex_t *m);
int pthread_mutex_lock(pthread_mutex_t *m);
int pthread_mutex_trylock(pthread_mutex_t *m);
int pthread_mutex_unlock(pthread_mutex_t *m);

int pthread_cond_init(pthread_cond_t *c, const pthread_condattr_t *attr);
int pthread_cond_destroy(pthread_cond_t *c);
int pthread_cond_wait(pthread_cond_t *c, pthread_mutex_t *m);
int pthread_cond_timedwait(pthread_cond_t *c, pthread_mutex_t *m, const struct timespec *until);
int pthread_cond_signal(pthread_cond_t *c);
int pthread_cond_broadcast(pthread_cond_t *c);

int pthread_once(pthread_once_t *once, void (*fn)(void));
#endif
