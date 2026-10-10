// task.c - tasks and a round-robin scheduler, on every core
//
// Each core has its own current task, idle task and time slice. A task that
// runs somewhere has running_on set, so no other core picks it. User
// programs may run on any core; kernel tasks (login, the idle tasks) only on
// the core they are pinned to. Everything here runs under the big kernel
// lock (sys/smp.h), which the switching core keeps across task_switch: the
// task it leaves cannot be picked elsewhere before its registers are saved.
#include "sys/task.h"
#include "sys/smp.h"
#include "core/gdt.h"
#include "core/exceptions.h"
#include "drivers/heap.h"
#include "drivers/paging.h"
#include "drivers/timer.h"
#include "lib/string.h"

extern void task_switch(uint64_t *save_rsp, uint64_t new_rsp);   // taskswitch.asm
extern void task_start_user(void);
extern void task_start_kernel(void);

int bkl_depth_get(void);                  // sys/smp.c
void bkl_depth_set(int depth);

#define KSTACK_SIZE (64 * 1024)   // kernel commands run on it too (kcommand: TLS, FAT32)

static task_t tasks[MAX_TASKS];
static task_t *cur[MAX_CPUS];
static task_t *idle_of[MAX_CPUS];
static int slice_left[MAX_CPUS];
static int next_pid = 2;
static uint8_t fpu_template[512] __attribute__((aligned(16)));   /* a clean x87/SSE state */

static uint64_t irq_save(void) {
    uint64_t flags;
    __asm__ volatile("pushfq; pop %0; cli" : "=r"(flags) :: "memory");
    return flags;
}

static void irq_restore(uint64_t flags) {
    if (flags & 0x200) __asm__ volatile("sti" ::: "memory");
}

int task_running(void) { return cur[0] != 0; }
task_t *task_current(void) { return cur[cpu_index()]; }
task_t *task_at(int index) { return index >= 0 && index < MAX_TASKS ? &tasks[index] : 0; }

task_t *task_by_pid(int pid) {
    for (int i = 0; i < MAX_TASKS; i++)
        if (tasks[i].state != TASK_FREE && tasks[i].pid == pid) return &tasks[i];
    return 0;
}

static task_t *alloc_task(const char *name) {
    for (int i = 0; i < MAX_TASKS; i++) {
        if (tasks[i].state != TASK_FREE) continue;
        task_t *t = &tasks[i];
        memset(t, 0, sizeof(*t));
        k_snprintf(t->name, sizeof(t->name), "%s", name);
        t->root = paging_kernel_root();
        t->start_tick = get_tick();
        t->running_on = -1;
        t->bkl_depth = 1;                   /* a new task starts inside the kernel */
        memcpy(t->fpu, fpu_template, sizeof(t->fpu));
        return t;
    }
    return 0;
}

/* Kernel stack with the frame task_switch pops: r15 r14 r13 r12 rbp rbx, then
 * the return address. Returns the stack pointer to save in t->rsp. */
static uint64_t *initial_stack(task_t *t, uint64_t *top, void (*start)(void), uint64_t r12) {
    *--top = (uint64_t)start;   // task_switch's ret
    *--top = 0;                 // rbx
    *--top = 0;                 // rbp
    *--top = r12;               // r12
    *--top = 0;                 // r13
    *--top = 0;                 // r14
    *--top = 0;                 // r15
    t->rsp = (uint64_t)top;
    return top;
}

static int alloc_stack(task_t *t) {
    t->kstack = kmalloc(KSTACK_SIZE);
    if (!t->kstack) return -1;
    t->kstack_top = ((uint64_t)t->kstack + KSTACK_SIZE) & ~(uint64_t)15;
    return 0;
}

static void idle_loop(void) {
    for (;;) {
        cpu_wait();
        task_yield();
    }
}

void task_init(void) {
    __asm__ volatile("fninit; fxsave %0" : "=m"(fpu_template));
    // The code running now (kernel_main -> shell_run) becomes task 1: it logs
    // users in and starts their shell.
    task_t *shell = alloc_task("login");
    shell->pid = 1;
    shell->state = TASK_READY;
    shell->pinned = 0;
    shell->running_on = 0;
    cur[0] = shell;
    slice_left[0] = TASK_SLICE;

    task_t *idle = alloc_task("idle");
    idle->pid = 0;
    idle->is_idle = 1;
    idle->pinned = 0;
    if (alloc_stack(idle) != 0) kpanic("no memory for the idle task");
    // After the ret into task_start_kernel the stack must be 16-byte aligned for `call`.
    initial_stack(idle, (uint64_t *)idle->kstack_top, task_start_kernel, (uint64_t)idle_loop);
    idle->state = TASK_READY;
    idle_of[0] = idle;
}

void task_init_ap(int cpu) {
    uint64_t flags = irq_save();
    char name[16];
    k_snprintf(name, sizeof(name), "idle %d", cpu);
    task_t *t = alloc_task(name);
    if (!t) kpanic("no task slot for a core's idle task");
    t->pid = 0;
    t->is_idle = 1;
    t->pinned = cpu;
    t->running_on = cpu;
    t->state = TASK_READY;
    cur[cpu] = t;
    idle_of[cpu] = t;
    slice_left[cpu] = TASK_SLICE;
    irq_restore(flags);
}

task_t *task_create_user(const char *name, uint64_t root, uint64_t entry, uint64_t user_sp) {
    uint64_t flags = irq_save();
    task_t *t = alloc_task(name);
    if (!t || alloc_stack(t) != 0) {
        if (t) t->state = TASK_FREE;
        irq_restore(flags);
        return 0;
    }
    task_t *me = task_current();
    t->pid = next_pid++;
    t->root = root;
    t->is_user = 1;
    t->parent = me ? me->pid : 1;

    // iretq frame for task_start_user: rip, cs, rflags, rsp, ss.
    uint64_t *top = (uint64_t *)t->kstack_top;
    *--top = 0x33;              // ss: user data, RPL 3
    *--top = user_sp;
    *--top = 0x202;             // rflags: IF
    *--top = 0x2B;              // cs: user code, RPL 3
    *--top = entry;
    initial_stack(t, top, task_start_user, 0);
    t->state = TASK_READY;
    irq_restore(flags);
    return t;
}

static int may_run_here(const task_t *t, int me) {
    if (t->state != TASK_READY || t->running_on >= 0 || t->is_idle) return 0;
    return t->is_user || t->pinned == me;
}

static void schedule(void) {
    uint64_t flags = irq_save();
    int me = cpu_index();
    task_t *prev = cur[me];
    uint32_t now = get_tick();
    for (int i = 0; i < MAX_TASKS; i++)
        if (tasks[i].state == TASK_SLEEPING && (int32_t)(now - tasks[i].wake_tick) >= 0)
            tasks[i].state = TASK_READY;

    // Round robin from the task after this one; this core's idle task only if
    // nothing else can run (the task running now may go on: it is "ours").
    int start = (int)(prev - tasks);
    task_t *next = 0;
    for (int i = 1; i <= MAX_TASKS; i++) {
        task_t *t = &tasks[(start + i) % MAX_TASKS];
        if (t == prev ? (t->state == TASK_READY && !t->is_idle) : may_run_here(t, me)) {
            next = t;
            break;
        }
    }
    if (!next) next = idle_of[me];

    slice_left[me] = TASK_SLICE;
    if (next != prev) {
        prev->running_on = -1;
        next->running_on = me;
        cur[me] = next;
        if (next->kstack_top) gdt_set_kernel_stack(next->kstack_top);
        paging_switch(next->root);
        prev->bkl_depth = bkl_depth_get();
        bkl_depth_set(next->bkl_depth);
        /* Its floating point registers go with the task (it may resume on another core). */
        __asm__ volatile("fxsave %0" : "=m"(prev->fpu));
        __asm__ volatile("fxrstor %0" : : "m"(next->fpu));
        task_switch(&prev->rsp, next->rsp);
    }
    irq_restore(flags);
}

void task_yield(void) {
    if (task_current()) schedule();
}

void task_sleep(uint32_t ms) {
    task_t *me = task_current();
    if (!me) {
        sleep(ms);
        return;
    }
    me->wake_tick = get_tick() + (ms * TIMER_FREQ + 999) / 1000;
    me->state = TASK_SLEEPING;
    schedule();
}

void task_exit(int code) {
    __asm__ volatile("cli");
    task_t *me = task_current();
    me->exit_code = code;
    me->state = TASK_ZOMBIE;
    paging_switch(paging_kernel_root());
    schedule();
    kpanic("a finished task was scheduled again");
    for (;;) { }
}

/* A kernel task's function returned (taskswitch.asm). */
void task_exit_kernel(void) {
    task_exit(0);
}

void task_reap(task_t *t) {
    if (!t || t->state != TASK_ZOMBIE || t->running_on >= 0) return;
    uint64_t flags = irq_save();
    kfree(t->kstack);
    t->kstack = 0;
    t->state = TASK_FREE;
    irq_restore(flags);
}

void task_tick(void) {
    task_t *me = task_current();
    if (me) me->ticks++;
}

void task_preempt(void) {
    int me = cpu_index();
    if (cur[me] && --slice_left[me] <= 0) schedule();
}
