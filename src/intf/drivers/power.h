// power.h - ACPI power management (through uACPI): power off, the power
// button, the battery and the AC adapter
#ifndef POWER_H
#define POWER_H

#include <stdint.h>

/* Load the firmware's ACPI tables and AML and start the embedded controller.
 * Needs the heap, PCI and the timer ticks. Fills `how` for the boot screen.
 * Returns 0 when ACPI is up. */
int power_init(char *how, int how_len);

/* From the idle loop: ACPI events (power button, battery, embedded
 * controller), polled. A press of the power button shuts down. */
void power_poll(void);

/* Switch off (ACPI S5). Returns only if that failed. */
int power_off(void);

/* For the status bar: "BAT 87%", "BAT 87% charging"; empty without a battery. */
void power_battery_text(char *out, int len);
int power_battery(int *charging);      /* percent, -1 without a battery */

/* `battery`: every battery and the AC adapter in detail. */
void power_print(void);

#endif
