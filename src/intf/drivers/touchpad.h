// touchpad.h - I2C touchpads (HID over I2C), as on most laptops since ~2016
//
// The touchpad sits on one of the chipset's I2C controllers (Intel LPSS,
// a Synopsys DesignWare core). Windows and Linux learn which controller and
// address from the firmware's ACPI tables; here every controller is scanned
// and each answering address is asked for a HID descriptor. The touchpad
// starts in "mouse mode" (relative movement and buttons), which is what is
// used. Its interrupt line is not used: it is polled.
#ifndef TOUCHPAD_H
#define TOUCHPAD_H

/* Find and set up the touchpad. Needs the timer (call after `sti`).
 * Returns 0 if one was found. */
int touchpad_init(void);

/* "ELAN 04f3:3195 on I2C" for the boot screen, or why there is none. */
const char *touchpad_description(void);

/* Read a report if one is waiting (called by mouse_poll). */
void touchpad_poll(void);

/* `touchpad`: controllers, addresses that answer, the HID descriptor and
 * report layout, then the raw reports for a few seconds. */
void touchpad_diagnose(void);

#endif
