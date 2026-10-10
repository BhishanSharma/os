#ifndef PCI_H
#define PCI_H
#include <stdint.h>

uint32_t pci_config_read_dword(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset);
void pci_config_write_dword(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset, uint32_t value);
int pci_find_device(uint16_t vendor_id, uint16_t device_id, uint8_t *bus, uint8_t *slot, uint8_t *func);
uint8_t pci_config_read_byte(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset);
void pci_config_write_word(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset, uint16_t value);

/* One PCI function, as found by pci_scan. */
typedef struct {
    uint8_t bus, slot, func;
    uint16_t vendor, device;
    uint8_t class_code, subclass, prog_if, revision;
    uint16_t sub_vendor, sub_device;   /* the board maker's IDs (e.g. the laptop vendor) */
    uint8_t irq;                       /* legacy interrupt line, 0xFF = none */
    uint64_t bar0;                     /* first memory or I/O window, 0 if none */
} pci_device_t;

/* Every function on every bus, up to `max`. Returns how many were found. */
int pci_scan(pci_device_t *out, int max);

/* "Ethernet controller", "USB controller (xHCI)", ... */
const char *pci_class_name(uint8_t class_code, uint8_t subclass, uint8_t prog_if);

/* "Intel", "Realtek", ...; 0 if unknown */
const char *pci_vendor_name(uint16_t vendor);

/* Give a device that the firmware left without one a memory address for
 * BAR 0 (in a free spot after the firmware's assignments). Returns it, or 0. */
uint64_t pci_assign_bar0(uint8_t bus, uint8_t slot, uint8_t func);
/* Where a BAR of `size` would go (a dry run). */
uint64_t pci_find_free_window(uint64_t size);
/* How the last assignment was decided, for diagnostics. */
const char *pci_assign_note(void);

/* Wake a device to D0 (PCI power management). Returns the power register
 * before (bits 0-1: D-state), and the one after in *after. */
uint32_t pci_power_on(uint8_t bus, uint8_t slot, uint8_t func, uint32_t *after);

#endif
