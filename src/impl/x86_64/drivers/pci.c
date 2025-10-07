#include "drivers/pci.h"
#include "../lib/ports.h" // inb/outb
#include <stdint.h>

#define PCI_CONFIG_ADDRESS 0xCF8
#define PCI_CONFIG_DATA    0xCFC

uint32_t pci_config_read_dword(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset) {
    uint32_t address =
        (uint32_t)((uint32_t)bus << 16) |
        (uint32_t)((uint32_t)slot << 11) |
        (uint32_t)((uint32_t)func << 8) |
        (uint32_t)(offset & 0xFC) |
        (uint32_t)0x80000000;
    outl(PCI_CONFIG_ADDRESS, address);
    return inl(PCI_CONFIG_DATA + (offset & 3));
}

void pci_config_write_dword(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset, uint32_t value) {
    uint32_t address =
        (uint32_t)((uint32_t)bus << 16) |
        (uint32_t)((uint32_t)slot << 11) |
        (uint32_t)((uint32_t)func << 8) |
        (uint32_t)(offset & 0xFC) |
        (uint32_t)0x80000000;
    outl(PCI_CONFIG_ADDRESS, address);
    outl(PCI_CONFIG_DATA + (offset & 3), value);
}

uint8_t pci_config_read_byte(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset) {
    uint32_t d = pci_config_read_dword(bus, slot, func, offset & 0xFC);
    return (d >> ((offset & 3) * 8)) & 0xFF;
}

void pci_config_write_word(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset, uint16_t value) {
    uint32_t d = pci_config_read_dword(bus, slot, func, offset & 0xFC);
    uint32_t shift = (offset & 3) * 8;
    uint32_t mask = 0xFFFFu << shift;
    d = (d & ~mask) | ((uint32_t)value << shift);
    pci_config_write_dword(bus, slot, func, offset, d);
}

int pci_find_device(uint16_t vendor_id, uint16_t device_id, uint8_t *bus_out, uint8_t *slot_out, uint8_t *func_out) {
    for (uint8_t bus = 0; bus < 256; bus++) {
        for (uint8_t slot = 0; slot < 32; slot++) {
            uint32_t d = pci_config_read_dword(bus, slot, 0, 0x00);
            uint16_t vid = d & 0xFFFF;
            if (vid == 0xFFFF) continue;
            uint16_t did = (d >> 16) & 0xFFFF;
            if (vid == vendor_id && did == device_id) {
                if (bus_out) *bus_out = bus;
                if (slot_out) *slot_out = slot;
                if (func_out) *func_out = 0;
                return 0;
            }
            // Check functions 1..7 (if multi-function)
            uint32_t hdr = pci_config_read_dword(bus, slot, 0, 0x0C);
            if ((hdr >> 16) & 0x80) { // multi-function bit
                for (uint8_t func = 1; func < 8; func++) {
                    d = pci_config_read_dword(bus, slot, func, 0x00);
                    vid = d & 0xFFFF;
                    if (vid == vendor_id) {
                        did = (d >> 16) & 0xFFFF;
                        if (did == device_id) {
                            if (bus_out) *bus_out = bus;
                            if (slot_out) *slot_out = slot;
                            if (func_out) *func_out = func;
                            return 0;
                        }
                    }
                }
            }
        }
    }
    return -1;
}
