// task.h - tasks and the scheduler
//
// A task is a kernel stack plus saved registers: the shell (the boot context),
// the idle task, and one per running user program. Kernel code is never
// preempted: it gives up the CPU only where it waits (task_yield, keyboard,
// sleep). User code is preempted by the timer every TASK_SLICE ticks. So the
// kernel's drivers and file system need no locks.
#ifndef TASK_H
#define TASK_H

#include <stdint.h>
#include "sys/users.h"

#define MAX_TASKS   32
#define TASK_SLICE  5         /* timer ticks (50 ms) a program runs before the next one */

typedef enum { TASK_FREE, TASK_READY, TASK_SLEEPING, TASK_ZOMBIE } task_state_t;

typedef struct task {
    int pid;
    task_state_t state;
    char name[16];
    uint8_t *kstack;          /* kmalloc'd kernel stack (0 for the boot task) */
    uint64_t kstack_top;
    uint64_t rsp;             /* saved while not running */
    uint64_t root;            /* page tables (CR3); the kernel's for kernel tasks */
    int is_user;              /* runs a user program */
    int parent;               /* pid that waits for it */
    int exit_code;
    uint32_t wake_tick;       /* TASK_SLEEPING until then */
    uint32_t ticks;           /* timer ticks spent running (for ps) */
    uint32_t start_tick;
    int kill_code;            /* nonzero: end the program at the next chance */
    user_t user;              /* who runs it */
    void *process;            /* sys/process.c state of a user program */
    int running_on;           /* the core running it now, -1 if none (sys/smp.h) */
    int pinned;               /* kernel tasks: the only core that may run it */
    int is_idle;
    int bkl_depth;            /* its kernel-lock nesting while switched out */
    uint8_t fpu[512] __attribute__((aligned(16)));   /* x87/SSE registers (fxsave) */
} task_t;

/* The boot context becomes task 1 ("shell"); also starts the idle task (pid 0). */
void task_init(void);
/* A core that just started: its boot context becomes its idle task. */
void task_init_ap(int cpu);
int task_running(void);       /* 1 once task_init has run */

task_t *task_current(void);
task_t *task_by_pid(int pid);
task_t *task_at(int index);   /* 0..MAX_TASKS-1, for ps; FREE slots included */

/* A user program: `root` page tables, starts at `entry` with stack `user_sp`.
 * The task is READY when this returns. 0 if there is no free slot or memory. */
task_t *task_create_user(const char *name, uint64_t root, uint64_t entry, uint64_t user_sp);

void task_yield(void);                     /* let others run, stay ready */
void task_sleep(uint32_t ms);              /* not scheduled for `ms` */
void task_exit(int code) __attribute__((noreturn));   /* becomes a zombie */
void task_reap(task_t *t);                 /* free a zombie's slot and stack */

/* Timer interrupt: account the tick to the running task. */
void task_tick(void);
/* Timer interrupt that interrupted user code: switch if its slice is used up. */
void task_preempt(void);

#endif
