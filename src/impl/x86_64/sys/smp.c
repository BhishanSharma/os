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
#define TLB_VECTOR       0x42
#define AP_STACK_SIZE    (64 * 1024)
#define MSR_TSC_AUX      0xC0000103

extern uint8_t trampoline_start[], trampoline_end[], trampoline_params[];
extern void ap_timer_stub(void);
extern void tlb_ipi_stub(void);
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
static void tlb_catch_up(int me);

/* Statistics (`bklstat`): each core's waits; the time it was held, by
 * what the task running on the holder was doing: a system call (0-79), a
 * user program outside any (its page faults, its timer ticks), an idle core,
 * a kernel task (the shell, logd), or before tasks. The scheduler moves the
 * time to the next task at each switch. Only the owner writes st_since and
 * st_slot. */
#define SL_USER  96                        /* (after the system calls' numbers) */
#define SL_IDLE  97
#define SL_KTASK 98
#define SL_BOOT  99
#define BKL_SLOTS 100
static uint64_t st_held[BKL_SLOTS], st_takes[BKL_SLOTS], st_wait[BKL_SLOTS];
static uint64_t st_spin[MAX_CPUS], st_waits[MAX_CPUS], st_got[MAX_CPUS];
static uint64_t st_since, st_tsc0;
static uint32_t st_ms0;
static int st_slot;

static inline uint64_t tsc_now(void) {
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return (uint64_t)hi << 32 | lo;
}

static int holder_slot(void) {
    task_t *t = task_current();
    if (!t) return SL_BOOT;
    if (t->is_idle) return SL_IDLE;
    if (!t->is_user) return SL_KTASK;
    int n = t->sysno - 1;
    return n >= 0 && n < SL_USER ? n : SL_USER;
}

/* Taking, leaving and dropping the lock change the owner and the depth in two
 * steps, and an interrupt on this core takes the lock too (isr.c, the timer):
 * between the steps it saw the lock as its own at depth 0, took it "again",
 * let go of it, and the second step then cleared the owner under another
 * core that had just taken it (two cores in the kernel). So: interrupts off. */
static inline uint64_t bkl_irq_off(void) {
    uint64_t flags;
    __asm__ volatile("pushfq; pop %0; cli" : "=r"(flags) :: "memory");
    return flags;
}

static inline void bkl_irq_on(uint64_t flags) {
    if (flags & (1u << 9)) __asm__ volatile("sti" ::: "memory");
}

/* Spins with interrupts off, so it also answers TLB shootdowns (below):
 * the lock's owner may be waiting for this core to flush. */
static void bkl_acquire(int me) {
    if (__sync_val_compare_and_swap(&bkl_owner, -1, me) != -1) {
        uint64_t t0 = tsc_now();
        while (__sync_val_compare_and_swap(&bkl_owner, -1, me) != -1) {
            tlb_catch_up(me);
            __asm__ volatile("pause");
        }
        uint64_t spun = tsc_now() - t0;
        st_spin[me] += spun;
        st_waits[me]++;
        st_wait[holder_slot()] += spun;
    }
    st_got[me]++;
    st_since = tsc_now();
    st_slot = holder_slot();
}

static void bkl_release(void) {
    st_held[st_slot] += tsc_now() - st_since;
    st_takes[st_slot]++;
    __atomic_store_n(&bkl_owner, -1, __ATOMIC_RELEASE);
}

/* The scheduler switched tasks with the lock held: from now on it is held
 * for the task that runs. */
void bkl_switched(void) {
    if (!smp_on || bkl_owner != cpu_index()) return;
    uint64_t now = tsc_now();
    st_held[st_slot] += now - st_since;
    st_since = now;
    st_slot = holder_slot();
}

static const char *slot_name(int i) {
    static const char *const extra[] = { "[user: faults, ticks]", "[idle cores]", "[kernel tasks]", "[boot]" };
    extern const char *const bkl_sys_names[];
    return i >= SL_USER ? extra[i - SL_USER] : bkl_sys_names[i] ? bkl_sys_names[i] : "?";
}

static void top_slots(const uint64_t *v, const uint64_t *count, uint64_t per_ms, uint32_t ms, int rows) {
    int shown[BKL_SLOTS] = { 0 };
    for (int k = 0; k < rows; k++) {
        int best = -1;
        for (int i = 0; i < BKL_SLOTS; i++)
            if (!shown[i] && v[i] && (best < 0 || v[i] > v[best])) best = i;
        if (best < 0) break;
        shown[best] = 1;
        uint64_t t = v[best] / per_ms;
        kprintf("    %-22s %6lu ms %3lu%%", slot_name(best), t, ms ? t * 100 / ms : 0);
        if (count) kprintf("  %8lu times, %lu us each", count[best], count[best] ? v[best] * 1000 / per_ms / count[best] : 0);
        kprintf("\n");
    }
}

const char *const bkl_sys_names[SL_USER] = {
    [0] = "exit", [1] = "write", [2] = "read", [3] = "open", [4] = "close", [5] = "sbrk",
    [6] = "sleep", [7] = "uptime", [8] = "time", [9] = "getkey", [10] = "console", [11] = "readdir",
    [12] = "unlink", [13] = "getuser", [14] = "getpid", [15] = "yield", [16] = "spawn",
    [17] = "wait", [18] = "kill", [19] = "taskinfo", [20] = "chdir", [21] = "getcwd",
    [22] = "kcommand", [23] = "ctrlc", [24] = "uname", [25] = "mouse", [26] = "gfx",
    [27] = "resolve", [28] = "connect", [29] = "listen", [30] = "accept", [31] = "send",
    [32] = "recv", [33] = "sockclose", [34] = "seek", [35] = "sound", [36] = "mkdir", [37] = "stat",
    [38] = "listdir", [39] = "meminfo", [40] = "mmap", [41] = "munmap", [42] = "shmmap",
    [43] = "fork", [44] = "exec", [45] = "thread", [46] = "thread_exit", [47] = "futex",
    [48] = "port_create", [49] = "port_send", [50] = "port_receive", [51] = "port_register",
    [52] = "port_lookup", [53] = "port_close", [54] = "pipe", [55] = "dup", [56] = "dup2",
    [57] = "signal", [58] = "sigsend", [59] = "sigreturn", [60] = "registry", [61] = "auth",
    [62] = "system", [63] = "tcb_set", [64] = "sockinfo", [65] = "fdinfo", [66] = "usock",
    [67] = "pollwait", [68] = "shmfd", [69] = "mmap2", [70] = "fdflags", [71] = "exepath",
    [72] = "mmapat", [73] = "fdpath", [74] = "statx", [75] = "chmod", [76] = "chown",
    [77] = "rename", [78] = "sandbox", [79] = "job", [80] = "poll"
};

void bkl_stats_print(int reset) {
    uint32_t ms = timer_ms() - st_ms0;
    uint64_t cycles = tsc_now() - st_tsc0;
    uint64_t per_ms = ms ? cycles / ms : 1;
    if (!per_ms) per_ms = 1;
    kprintf("The big kernel lock over %u.%u s, %d core%s:\n", ms / 1000, ms % 1000 / 100, ncpus, ncpus == 1 ? "" : "s");
    for (int i = 0; i < ncpus; i++) {
        uint64_t w = st_spin[i] / per_ms;
        kprintf("  core %d: waited %lu ms (%lu.%lu%%) in %lu waits; took it %lu times\n", i, w,
                ms ? w * 100 / ms : 0, ms ? w * 1000 / ms % 10 : 0, st_waits[i], st_got[i]);
    }
    kprintf("  held, by what the task on that core was doing:\n");
    top_slots(st_held, st_takes, per_ms, ms, 14);
    kprintf("  waited for, by what the waiting task was doing:\n");
    top_slots(st_wait, 0, per_ms, ms, 10);
    if (reset) {
        __asm__ volatile("cli");
        for (int i = 0; i < BKL_SLOTS; i++) st_held[i] = st_takes[i] = st_wait[i] = 0;
        for (int i = 0; i < MAX_CPUS; i++) st_spin[i] = st_waits[i] = st_got[i] = 0;
        st_tsc0 = tsc_now();
        st_ms0 = timer_ms();
        __asm__ volatile("sti");
        kprintf("  (counting again from now)\n");
    }
}

void bkl_enter(void) {
    if (!smp_on) return;
    uint64_t flags = bkl_irq_off();
    int me = cpu_index();
    if (bkl_owner == me) {
        bkl_depth++;
    } else {
        bkl_acquire(me);
        bkl_depth = 1;
    }
    bkl_irq_on(flags);
}

int bkl_try_enter(void) {
    if (!smp_on) return 1;
    int me = cpu_index();
    if (bkl_owner == me || __sync_val_compare_and_swap(&bkl_owner, -1, me) != -1) return 0;
    bkl_depth = 1;
    return 1;
}

void bkl_leave(void) {
    if (!smp_on) return;
    uint64_t flags = bkl_irq_off();
    if (bkl_owner == cpu_index() && --bkl_depth <= 0) {
        bkl_depth = 0;
        bkl_release();
    }
    bkl_irq_on(flags);
}

int bkl_drop(void) {
    if (!smp_on) return 0;
    uint64_t flags = bkl_irq_off();                 /* (as cpu_wait) */
    int depth = 0;
    if (bkl_owner == cpu_index()) {
        depth = bkl_depth;
        bkl_depth = 0;
        bkl_release();
    }
    bkl_irq_on(flags);
    return depth;
}

void bkl_take(int depth) {
    if (!depth) return;
    __asm__ volatile("cli");                        /* (as cpu_wait: no interrupt half-way) */
    bkl_acquire(cpu_index());
    bkl_depth = depth;
    __asm__ volatile("sti");
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

/* ---- TLB shootdown ------------------------------------------------------------------
 * A core keeps translations of its address space cached (no PCID: a CR3 switch
 * drops them). When the lock's owner unmaps or write-protects a program's page,
 * the other cores running that program must forget it before the page is
 * reused. Each shootdown is a new generation; a core that has flushed records
 * it. Cores take it from the interrupt, or while spinning for the lock with
 * interrupts off (the owner is the one waiting). */

static volatile uint64_t tlb_gen;
static volatile uint64_t tlb_seen[MAX_CPUS];

static void tlb_catch_up(int me) {
    uint64_t g = __atomic_load_n(&tlb_gen, __ATOMIC_ACQUIRE);
    if (tlb_seen[me] == g) return;
    paging_flush();
    __atomic_store_n(&tlb_seen[me], g, __ATOMIC_RELEASE);
}

void tlb_ipi_interrupt(void) {
    tlb_catch_up(cpu_index());
    apic_eoi();
}

void tlb_shootdown(uint64_t root) {
    if (!smp_on || ncpus < 2) return;
    int me = cpu_index(), wait[MAX_CPUS], n = 0;
    uint64_t g = __atomic_add_fetch(&tlb_gen, 1, __ATOMIC_ACQ_REL);
    tlb_seen[me] = g;                               /* the caller flushed its own (invlpg) */
    for (int i = 0; i < ncpus; i++) {
        if (i == me || !cpu_up[i] || (root && task_root_on(i) != root)) continue;
        apic_send_ipi(cpu_apic[i], TLB_VECTOR);
        wait[n++] = i;
    }
    /* A core that stopped (a panic) never answers: give up after a while
     * (counted, not timed: the ticks stop while this core has interrupts off). */
    uint64_t spins = 0;
    for (int k = 0; k < n; k++)
        while (__atomic_load_n(&tlb_seen[wait[k]], __ATOMIC_ACQUIRE) < g && spins++ < 100000000ull)
            __asm__ volatile("pause");
}

/* ---- The other cores ---------------------------------------------------------------- */

/* Their timer: preemption of user programs, like the boot core's. */
static uint8_t ap_sub[MAX_CPUS];

/* 1 kHz: in between ticks it only wakes the core (idle, it then looks for a
 * task that became ready, or whose sleep ended, without waiting 10 ms). */
void ap_timer_interrupt(uint64_t cs) {
    apic_eoi();
    int me = cpu_index();
    if (++ap_sub[me] < TIMER_HW_FREQ / TIMER_FREQ) return;
    ap_sub[me] = 0;
    task_tick();                                    /* (its task's own count: no lock) */
    if (cs & 3) user_check_interrupt();             /* (the lock only if there is work for it) */
}

/* A core arrives here from the trampoline: interrupts off, on its own stack. */
void ap_main(uint64_t cpu) {
    gdt_init_ap((int)cpu, fatal_stack_top[cpu]);
    idt_load_ap();
    if (have_rdtscp) wrmsr(MSR_TSC_AUX, cpu);
    apic_setup_core(timer_rate, AP_TIMER_VECTOR);
    paging_pat_init();                              /* the same memory types as core 0 */
    paging_cpu_protect();
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
    idt_set_entry(TLB_VECTOR, tlb_ipi_stub, 0x8E);

    /* The trampoline below 1 MiB, identity mapped while the cores start (it
     * turns paging on mid-way); written through the direct map. Only the
     * kernel's own tables get the identity page: programs' address spaces
     * never copy the lower half. */
    map_page(TRAMPOLINE, TRAMPOLINE, PAGE_PRESENT | PAGE_RW);
    uint64_t size = (uint64_t)(trampoline_end - trampoline_start);
    memcpy(phys_to_virt(TRAMPOLINE), trampoline_start, size);
    uint64_t *params = (uint64_t *)phys_to_virt(TRAMPOLINE + (trampoline_params - trampoline_start));

    /* From here on the kernel lock is real: this core holds it. */
    __asm__ volatile("cli");
    smp_on = 1;
    bkl_owner = 0;
    bkl_depth = 1;
    __asm__ volatile("sti");

    int late = 0;
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
            late = 1;
        }
    }
    if (!late) unmap_page(TRAMPOLINE);     /* a core that is late still needs it */
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
