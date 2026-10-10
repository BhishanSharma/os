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

#endif
