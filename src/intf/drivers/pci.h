#ifndef PCI_H
#define PCI_H
#include <stdint.h>

uint32_t pci_config_read_dword(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset);
void pci_config_write_dword(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset, uint32_t value);
int pci_find_device(uint16_t vendor_id, uint16_t device_id, uint8_t *bus, uint8_t *slot, uint8_t *func);
uint8_t pci_config_read_byte(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset);
void pci_config_write_word(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset, uint16_t value);

#endif
