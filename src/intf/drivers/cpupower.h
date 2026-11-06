// cpupower.h - the processor's own power management
//
// Intel CPUs since Skylake pick their clock themselves (hardware P-states,
// HWP: turbo when busy, slow when not) once the OS turns it on and says how
// to lean (energy/performance preference: balanced). Without that the clock
// stays where the firmware left it. An idle core waits with MWAIT in a deep
// C-state (core C6) instead of HLT (C1) where the CPU says it can, with an
// APIC timer that keeps running there (ARAT). Inside a virtual machine
// neither is touched: HLT, and the host decides.
#ifndef CPUPOWER_H
#define CPUPOWER_H

/* Core 0, once: what the CPU has, and its own setup. Then each other core
 * as it starts (cpupower_init_ap). */
void cpupower_init(void);
void cpupower_init_ap(void);

/* An idle core's wait until the next interrupt (interrupts on during it). */
void cpu_idle_wait(void);

/* For the boot line: "HWP on (balanced), idle in C6", or why not. */
const char *cpupower_status(void);

/* `cpu`: the processor, its clock now (busy and idle), HWP, C-states. */
void cpupower_print(void);

#endif
