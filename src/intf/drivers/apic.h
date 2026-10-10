#ifndef APIC_H
#define APIC_H

#include <stdint.h>

/* Make sure timer interrupts arrive; call right after `sti`.
 *
 * VMs deliver the PIT's IRQ 0 through the 8259 PIC straight to the CPU.
 * Real PCs booted by UEFI often do not: the local APIC may block the PIC's
 * line (LINT0 masked), and newer Intel chipsets may switch the PIT off. This
 * checks for ticks and, if none come, tries in turn: routing the PIC through
 * the local APIC ("virtual wire" mode), then the local APIC's own timer.
 *
 * Returns 0 if ticks arrive (and fills `how`, e.g. "PIT via 8259 PIC"),
 * -1 if nothing worked (`how` then says what was found). */
int timer_check(char *how, int size);

/* Called at the start of every timer interrupt: acknowledges it to the local
 * APIC when the APIC timer drives the ticks. */
void apic_timer_ack(void);

#endif
