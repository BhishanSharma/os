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
    for (int bus = 0; bus < 256; bus++) {
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

/* ---- Enumeration -------------------------------------------------------- */

static void read_function(uint8_t bus, uint8_t slot, uint8_t func, uint32_t id, pci_device_t *d) {
    uint32_t cls = pci_config_read_dword(bus, slot, func, 0x08);
    uint32_t sub = pci_config_read_dword(bus, slot, func, 0x2C);
    uint32_t irq = pci_config_read_dword(bus, slot, func, 0x3C);
    uint32_t bar = pci_config_read_dword(bus, slot, func, 0x10);
    uint8_t header = (pci_config_read_dword(bus, slot, func, 0x0C) >> 16) & 0x7F;
    d->bus = bus;
    d->slot = slot;
    d->func = func;
    d->vendor = id & 0xFFFF;
    d->device = id >> 16;
    d->revision = cls & 0xFF;
    d->prog_if = (cls >> 8) & 0xFF;
    d->subclass = (cls >> 16) & 0xFF;
    d->class_code = cls >> 24;
    d->sub_vendor = header == 0 ? (sub & 0xFFFF) : 0;
    d->sub_device = header == 0 ? (sub >> 16) : 0;
    d->irq = irq & 0xFF;
    d->bar0 = 0;
    if (bar & 1) {
        d->bar0 = bar & ~3u;                                    // I/O ports
    } else if (bar) {
        d->bar0 = bar & ~0xFu;                                  // memory
        if (((bar >> 1) & 3) == 2)                              // 64-bit BAR
            d->bar0 |= (uint64_t)pci_config_read_dword(bus, slot, func, 0x14) << 32;
    }
}

int pci_scan(pci_device_t *out, int max) {
    int n = 0;
    for (int bus = 0; bus < 256; bus++) {
        for (uint8_t slot = 0; slot < 32; slot++) {
            uint32_t id = pci_config_read_dword(bus, slot, 0, 0x00);
            if ((id & 0xFFFF) == 0xFFFF) continue;
            int functions = (pci_config_read_dword(bus, slot, 0, 0x0C) >> 16) & 0x80 ? 8 : 1;
            for (uint8_t func = 0; func < functions; func++) {
                if (func) id = pci_config_read_dword(bus, slot, func, 0x00);
                if ((id & 0xFFFF) == 0xFFFF) continue;
                if (n < max) read_function(bus, slot, func, id, &out[n]);
                n++;
            }
        }
    }
    return n < max ? n : max;
}

const char *pci_class_name(uint8_t c, uint8_t s, uint8_t p) {
    switch (c) {
        case 0x01:
            switch (s) {
                case 0x01: return "IDE controller";
                case 0x06: return "SATA controller (AHCI)";
                case 0x08: return "NVMe SSD controller";
                default:   return "Storage controller";
            }
        case 0x02:
            switch (s) {
                case 0x00: return "Ethernet controller";
                case 0x80: return "Network controller (Wi-Fi)";
                default:   return "Network controller";
            }
        case 0x03: return s == 0x00 ? "VGA display" : "Display controller";
        case 0x04: return s == 0x03 ? "Audio device (HD Audio)" : "Multimedia device";
        case 0x05: return "Memory controller";
        case 0x06:
            switch (s) {
                case 0x00: return "Host bridge";
                case 0x01: return "ISA bridge";
                case 0x04: return "PCI bridge";
                default:   return "Bridge";
            }
        case 0x07: return "Communication controller";
        case 0x08: return "System peripheral";
        case 0x09: return "Input device";
        case 0x0C:
            if (s == 0x03) {
                switch (p) {
                    case 0x00: return "USB controller (UHCI, USB 1)";
                    case 0x10: return "USB controller (OHCI, USB 1)";
                    case 0x20: return "USB controller (EHCI, USB 2)";
                    case 0x30: return "USB controller (xHCI, USB 3)";
                    default:   return "USB controller";
                }
            }
            return s == 0x05 ? "SMBus controller" : "Serial bus controller";
        case 0x0D: return s == 0x11 ? "Bluetooth controller" : "Wireless controller";
        case 0x10: return "Encryption controller";
        case 0x11: return "Signal processing controller";
        case 0x13: return "Instrumentation";
        default:   return "Device";
    }
}

const char *pci_vendor_name(uint16_t v) {
    switch (v) {
        case 0x8086: return "Intel";
        case 0x1022: return "AMD";
        case 0x1002: return "AMD/ATI";
        case 0x10DE: return "NVIDIA";
        case 0x10EC: return "Realtek";
        case 0x168C: return "Qualcomm Atheros";
        case 0x17CB: return "Qualcomm";
        case 0x1969: return "Qualcomm Atheros";
        case 0x14C3: return "MediaTek";
        case 0x14E4: return "Broadcom";
        case 0x144D: return "Samsung";
        case 0x1C5C: return "SK hynix";
        case 0x15B7: return "Western Digital";
        case 0x1E0F: return "KIOXIA";
        case 0x126F: return "Silicon Motion";
        case 0x1987: return "Phison";
        case 0x1179: return "Toshiba";
        case 0x1217: return "O2 Micro";
        case 0x1180: return "Ricoh";
        case 0x10EE: return "Xilinx";
        case 0x1234: return "QEMU";
        case 0x1AF4: return "Red Hat (virtio)";
        case 0x1B36: return "Red Hat (QEMU)";
        case 0x15AD: return "VMware";
        case 0x80EE: return "VirtualBox";
        default:     return 0;
    }
}
