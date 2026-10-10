// msi.h - message-signalled interrupts (PCI MSI and MSI-X)
//
// A PCI device signals an interrupt by writing a message to the local APIC's
// address; the message picks the vector. No PIC, no IRQ line routing: this
// works on every UEFI laptop. Each device gets its own vector (0x50-0x6F) and
// handler. Handlers run with interrupts off and should do what a timer poll
// would (the timer polls stay as a backstop).
#ifndef MSI_H
#define MSI_H

#include <stdint.h>

#define MSI_PREFER_MSIX 1

/* Turn the local APIC on for MSI. Call once, after the timer is running. */
int msi_init(void);

/* Route the device's interrupt to `handler`. Uses MSI (or MSI-X entry 0;
 * with MSI_PREFER_MSIX, MSI-X first). Returns 0 on success, -1 if the device
 * cannot do either (it stays polled). */
int msi_enable(uint8_t bus, uint8_t slot, uint8_t func, void (*handler)(void), const char *name, int flags);

/* Interrupts handled so far for the device named `name` (0 if none). */
uint32_t msi_count(const char *name);

/* `interrupts`: every MSI vector, its device, how often it fired. */
void msi_print(void);

#endif
