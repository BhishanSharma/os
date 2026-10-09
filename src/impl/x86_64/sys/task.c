// task.c - tasks and a round-robin scheduler
#include "sys/task.h"
#include "core/gdt.h"
#include "core/exceptions.h"
#include "drivers/heap.h"
#include "drivers/paging.h"
#include "drivers/timer.h"
#include "lib/string.h"

extern void task_switch(uint64_t *save_rsp, uint64_t new_rsp);   // taskswitch.asm
extern void task_start_user(void);
extern void task_start_kernel(void);

#define KSTACK_SIZE (32 * 1024)

static task_t tasks[MAX_TASKS];
static task_t *current;
static task_t *idle;
static int next_pid = 2;
static int slice_left = TASK_SLICE;

static uint64_t irq_save(void) {
    uint64_t flags;
    __asm__ volatile("pushfq; pop %0; cli" : "=r"(flags) :: "memory");
    return flags;
}

static void irq_restore(uint64_t flags) {
    if (flags & 0x200) __asm__ volatile("sti" ::: "memory");
}

int task_running(void) { return current != 0; }
task_t *task_current(void) { return current; }
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
        __asm__ volatile("sti; hlt");
        task_yield();
    }
}

void task_init(void) {
    // The code running now (kernel_main -> shell) becomes the shell task.
    task_t *shell = alloc_task("shell");
    shell->pid = 1;
    shell->state = TASK_READY;
    current = shell;

    idle = alloc_task("idle");
    idle->pid = 0;
    if (alloc_stack(idle) != 0) kpanic("no memory for the idle task");
    // After the ret into task_start_kernel the stack must be 16-byte aligned for `call`.
    initial_stack(idle, (uint64_t *)idle->kstack_top, task_start_kernel, (uint64_t)idle_loop);
    idle->state = TASK_READY;
}

task_t *task_create_user(const char *name, uint64_t root, uint64_t entry, uint64_t user_sp) {
    uint64_t flags = irq_save();
    task_t *t = alloc_task(name);
    if (!t || alloc_stack(t) != 0) {
        if (t) t->state = TASK_FREE;
        irq_restore(flags);
        return 0;
    }
    t->pid = next_pid++;
    t->root = root;
    t->is_user = 1;
    t->parent = current ? current->pid : 1;

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

static void schedule(void) {
    uint64_t flags = irq_save();
    task_t *prev = current;
    uint32_t now = get_tick();
    for (int i = 0; i < MAX_TASKS; i++)
        if (tasks[i].state == TASK_SLEEPING && (int32_t)(now - tasks[i].wake_tick) >= 0)
            tasks[i].state = TASK_READY;

    // Round robin from the task after this one; the idle task only if nothing else can run.
    int start = (int)(prev - tasks);
    task_t *next = 0;
    for (int i = 1; i <= MAX_TASKS; i++) {
        task_t *t = &tasks[(start + i) % MAX_TASKS];
        if (t->state == TASK_READY && t != idle) {
            next = t;
            break;
        }
    }
    if (!next) next = idle;

    slice_left = TASK_SLICE;
    if (next != prev) {
        current = next;
        if (next->kstack_top) gdt_set_kernel_stack(next->kstack_top);
        paging_switch(next->root);
        task_switch(&prev->rsp, next->rsp);
    }
    irq_restore(flags);
}

void task_yield(void) {
    if (current) schedule();
}

void task_sleep(uint32_t ms) {
    if (!current) {
        sleep(ms);
        return;
    }
    current->wake_tick = get_tick() + (ms * TIMER_FREQ + 999) / 1000;
    current->state = TASK_SLEEPING;
    schedule();
}

void task_exit(int code) {
    __asm__ volatile("cli");
    current->exit_code = code;
    current->state = TASK_ZOMBIE;
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
    if (!t || t->state != TASK_ZOMBIE) return;
    uint64_t flags = irq_save();
    kfree(t->kstack);
    t->kstack = 0;
    t->state = TASK_FREE;
    irq_restore(flags);
}

void task_tick(void) {
    if (current) current->ticks++;
}

void task_preempt(void) {
    if (current && --slice_left <= 0) schedule();
}
