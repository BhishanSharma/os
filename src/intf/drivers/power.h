// power.h - ACPI power management (through uACPI): power off, the power
// button, the battery and the AC adapter
#ifndef POWER_H
#define POWER_H

#include <stdint.h>

/* Load the firmware's ACPI tables and AML and start the embedded controller.
 * Needs the heap, PCI and the timer ticks. Fills `how` for the boot screen.
 * Returns 0 when ACPI is up. */
int power_init(char *how, int how_len);

/* From the idle loop: ACPI events (power button, lid, battery, embedded
 * controller), polled, and sleep. The power button and closing the lid put
 * the computer to sleep; the power button, opening the lid or a key wake it. */
void power_poll(void);

/* Sleep, as laptops with Modern Standby (S0ix) do it rather than ACPI S3,
 * which they lack: the screen and its backlight off, programs stopped at
 * their next return to user mode, the cores halted between interrupts, the
 * file systems marked clean (a battery that runs out loses nothing). Works
 * without ACPI too (a key wakes it). Starts at the next power_poll. */
void power_sleep(void);
int power_asleep(void);

/* Switch off (ACPI S5). Returns only if that failed. */
int power_off(void);

/* For the status bar: "BAT 87%", "BAT 87% charging"; empty without a battery. */
void power_battery_text(char *out, int len);
int power_battery(int *charging);      /* percent, -1 without a battery */

/* `battery`: every battery and the AC adapter in detail. */
void power_print(void);

#endif
