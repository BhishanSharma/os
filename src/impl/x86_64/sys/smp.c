// smp.c - starting the other cores, the big kernel lock (see sys/smp.h)
#include "sys/smp.h"
#include "sys/task.h"
#include "core/gdt.h"
#include "core/idt.h"
#include "drivers/apic.h"
#include "drivers/acpi.h"
#include "drivers/paging.h"
#include "drivers/heap.h"
#include "drivers/timer.h"
#include "lib/print.h"
#include "lib/string.h"

#define TRAMPOLINE       0x8000
#define AP_TIMER_VECTOR  0x41
#define AP_STACK_SIZE    (64 * 1024)
#define MSR_TSC_AUX      0xC0000103

extern uint8_t trampoline_start[], trampoline_end[], trampoline_params[];
extern void ap_timer_stub(void);
void user_check_interrupt(void);          // sys/process.c

static int ncpus = 1, smp_on, have_rdtscp;
static uint32_t cpu_apic[MAX_CPUS];
static volatile int cpu_up[MAX_CPUS];
static uint64_t fatal_stack_top[MAX_CPUS];  /* allocated by the boot core (the heap needs the lock) */
static uint32_t timer_rate;
static const char *smp_note = "one core";

static void wrmsr(uint32_t msr, uint64_t v) {
    __asm__ volatile("wrmsr" : : "c"(msr), "a"((uint32_t)v), "d"((uint32_t)(v >> 32)));
}

int smp_cpu_count(void) { return ncpus; }

int cpu_index(void) {
    if (!smp_on) return 0;
    if (have_rdtscp) {                              /* TSC_AUX holds the core's number */
        uint32_t aux, lo, hi;
        __asm__ volatile("rdtscp" : "=c"(aux), "=a"(lo), "=d"(hi));
        return (int)aux;
    }
    uint32_t id = apic_id();
    for (int i = 0; i < ncpus; i++)
        if (cpu_apic[i] == id) return i;
    return 0;
}

/* ---- The big kernel lock ---------------------------------------------------------- */

static volatile int bkl_owner = -1;
static volatile int bkl_depth;

static void bkl_acquire(int me) {
    while (__sync_val_compare_and_swap(&bkl_owner, -1, me) != -1) __asm__ volatile("pause");
}

static void bkl_release(void) {
    __atomic_store_n(&bkl_owner, -1, __ATOMIC_RELEASE);
}

void bkl_enter(void) {
    if (!smp_on) return;
    int me = cpu_index();
    if (bkl_owner == me) {
        bkl_depth++;
        return;
    }
    bkl_acquire(me);
    bkl_depth = 1;
}

void bkl_leave(void) {
    if (!smp_on || bkl_owner != cpu_index()) return;
    if (--bkl_depth <= 0) {
        bkl_depth = 0;
        bkl_release();
    }
}

/* For the scheduler: the nesting travels with the task (sys/task.c). */
int bkl_depth_get(void) { return smp_on ? bkl_depth : 0; }
void bkl_depth_set(int depth) {
    if (smp_on) bkl_depth = depth;
}

/* taskswitch.asm: a new program's first return to user mode. */
void bkl_user_entry(void) {
    if (smp_on && bkl_owner == cpu_index()) {
        bkl_depth = 0;
        bkl_release();
    }
}

void cpu_wait(void) {
    if (!smp_on) {
        __asm__ volatile("sti; hlt");
        return;
    }
    int me = cpu_index(), saved = 0;
    __asm__ volatile("cli");
    if (bkl_owner == me) {
        saved = bkl_depth;
        bkl_depth = 0;
        bkl_release();
    }
    __asm__ volatile("sti; hlt");                   /* an interrupt (a tick at least) wakes us */
    if (saved) {
        __asm__ volatile("cli");
        bkl_acquire(me);
        bkl_depth = saved;
        __asm__ volatile("sti");
    }
}

/* ---- The other cores ---------------------------------------------------------------- */

/* Their timer: preemption of user programs, like the boot core's. */
void ap_timer_interrupt(uint64_t cs) {
    apic_eoi();
    bkl_enter();
    task_tick();
    if (cs & 3) user_check_interrupt();
    bkl_leave();
}

/* A core arrives here from the trampoline: interrupts off, on its own stack. */
void ap_main(uint64_t cpu) {
    gdt_init_ap((int)cpu, fatal_stack_top[cpu]);
    idt_load_ap();
    if (have_rdtscp) wrmsr(MSR_TSC_AUX, cpu);
    apic_setup_core(timer_rate, AP_TIMER_VECTOR);
    __asm__ volatile("fninit");
    bkl_enter();
    task_init_ap((int)cpu);
    cpu_up[cpu] = 1;
    for (;;) {                                      /* this core's idle task */
        task_yield();
        cpu_wait();
    }
}

int smp_init(int enabled) {
    cpu_apic[0] = apic_id();
    uint32_t a, b, c, d;
    __asm__ volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(0x80000001), "c"(0));
    have_rdtscp = (d >> 27) & 1;
    if (have_rdtscp) wrmsr(MSR_TSC_AUX, 0);
    if (!enabled) {
        smp_note = "one core (`nosmp`)";
        return 1;
    }
    static uint32_t ids[64];
    int n = acpi_list_cpus(ids, 64);
    if (n <= 1) {
        smp_note = n < 0 ? "one core (no ACPI MADT)" : "one core (the firmware lists one)";
        return 1;
    }
    timer_rate = apic_timer_per_second();
    if (!timer_rate) {
        smp_note = "one core (no local APIC timer)";
        return 1;
    }
    idt_set_entry(AP_TIMER_VECTOR, ap_timer_stub, 0x8E);

    /* The trampoline below 1 MiB, identity mapped (it turns paging on mid-way). */
    map_page(TRAMPOLINE, TRAMPOLINE, PAGE_PRESENT | PAGE_RW);
    uint64_t size = (uint64_t)(trampoline_end - trampoline_start);
    memcpy((void *)TRAMPOLINE, trampoline_start, size);
    uint64_t *params = (uint64_t *)(TRAMPOLINE + (trampoline_params - trampoline_start));

    /* From here on the kernel lock is real: this core holds it. */
    __asm__ volatile("cli");
    smp_on = 1;
    bkl_owner = 0;
    bkl_depth = 1;
    __asm__ volatile("sti");

    for (int i = 0; i < n && ncpus < MAX_CPUS; i++) {
        if (ids[i] == cpu_apic[0]) continue;
        int idx = ncpus;
        uint8_t *stack = kmalloc(AP_STACK_SIZE), *fatal = kmalloc(8192);
        if (!stack || !fatal) break;
        fatal_stack_top[idx] = ((uint64_t)fatal + 8192) & ~15ull;
        cpu_apic[idx] = ids[i];
        cpu_up[idx] = 0;
        params[0] = paging_kernel_root();
        params[1] = ((uint64_t)stack + AP_STACK_SIZE) & ~15ull;
        params[2] = (uint64_t)ap_main;
        params[3] = (uint64_t)idx;
        __asm__ volatile("mfence" ::: "memory");
        ncpus++;                                    /* cpu_index() must know it */
        apic_start_core(ids[i], TRAMPOLINE >> 12);
        uint32_t start = get_tick();
        while (!cpu_up[idx] && (uint32_t)(get_tick() - start) < TIMER_FREQ / 2) cpu_wait();
        if (!cpu_up[idx]) {                         /* it did not come: forget it */
            ncpus--;
            kfree(stack);
            kprintf("Core with APIC ID %u did not start\n", ids[i]);
        }
    }
    smp_note = ncpus > 1 ? "cores" : "one core (the others did not start)";
    return ncpus;
}

void smp_print(void) {
    kprintf("%d %s%s\n", ncpus, ncpus > 1 ? "cores running" : smp_note, have_rdtscp ? "" : " (no RDTSCP)");
    for (int c = 0; c < ncpus; c++) {
        const char *what = "idle";
        int pid = 0;
        for (int i = 0; i < MAX_TASKS; i++) {
            task_t *t = task_at(i);
            if (t->state != TASK_FREE && t->running_on == c && !t->is_idle) {
                what = t->name;
                pid = t->pid;
            }
        }
        kprintf("  core %-2d  APIC ID %-3u  %s", c, cpu_apic[c], c == 0 ? "boot core (devices)  " : "                     ");
        if (pid) kprintf("running %s (pid %d)\n", what, pid);
        else kprintf("%s\n", what);
    }
}
