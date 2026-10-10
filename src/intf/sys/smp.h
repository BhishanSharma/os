// smp.h - several CPU cores
//
// The boot core starts the others (from the ACPI MADT's list) with the
// INIT-SIPI-SIPI sequence; they climb from 16-bit real mode to long mode
// through a small trampoline at 0x8000 and join the scheduler. User programs
// then run in parallel on every core.
//
// Kernel code is not written for parallelism (it guards its data by turning
// interrupts off), so it runs under one big kernel lock: whichever core is in
// kernel mode holds it (recursively: interrupts on the holding core nest).
// It is let go while kernel code waits (cpu_wait) and when a core returns to
// a user program. Device interrupts all go to the boot core; the other cores
// only take their own timer interrupt (for preemption).
#ifndef SMP_H
#define SMP_H

#include <stdint.h>

#define MAX_CPUS 16

/* Start the other cores. `enabled` 0 (the `nosmp` boot option) keeps one.
 * Returns how many cores run. Call after the timer and the local APIC. */
int smp_init(int enabled);

int smp_cpu_count(void);
int cpu_index(void);               /* 0 = the boot core */

/* The big kernel lock (no-ops while one core runs). */
void bkl_enter(void);
void bkl_leave(void);

/* Wait for an interrupt, letting other cores into the kernel meanwhile.
 * Use instead of `hlt` in kernel code. Turns interrupts on. */
void cpu_wait(void);

/* `cpus`: every core, what it runs. */
void smp_print(void);

#endif
