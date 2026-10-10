#include "drivers/pci.h"
#include "lib/string.h"
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

/* ---- Giving a device an address ----------------------------------------------- */

/* Some firmware leaves devices it has no driver for without a memory
 * address (BAR 0), as Windows and Linux assign one themselves. Do the same:
 * find what every other device uses, and put the device in the first free,
 * aligned spot
 * after the firmware's own assignments, below the top 64 MiB (chipset,
 * APIC, HPET, flash) and outside the PCI configuration window. */
#define MAX_RANGES 160
static uint64_t used_start[MAX_RANGES], used_end[MAX_RANGES];
static int used_count;
static uint32_t assign_low;                  /* lowest firmware-assigned 32-bit BAR */
static char assign_note[80];

static void add_used(uint64_t start, uint64_t size) {
    if (!size || used_count >= MAX_RANGES) return;
    used_start[used_count] = start;
    used_end[used_count++] = start + size;
}

/* A BAR's size, probed the standard way (decoding off meanwhile). */
static uint64_t bar_size(uint8_t b, uint8_t d, uint8_t f, uint8_t off, int is64) {
    uint32_t cmd = pci_config_read_dword(b, d, f, 0x04);
    pci_config_write_dword(b, d, f, 0x04, cmd & ~0x3u & 0xFFFF);
    uint32_t lo = pci_config_read_dword(b, d, f, off), hi = is64 ? pci_config_read_dword(b, d, f, off + 4) : 0;
    pci_config_write_dword(b, d, f, off, 0xFFFFFFFF);
    uint32_t mlo = pci_config_read_dword(b, d, f, off);
    uint32_t mhi = 0xFFFFFFFF;
    if (is64) {
        pci_config_write_dword(b, d, f, off + 4, 0xFFFFFFFF);
        mhi = pci_config_read_dword(b, d, f, off + 4);
        pci_config_write_dword(b, d, f, off + 4, hi);
    }
    pci_config_write_dword(b, d, f, off, lo);
    pci_config_write_dword(b, d, f, 0x04, cmd & 0xFFFF);
    uint64_t mask = ((uint64_t)mhi << 32) | (mlo & ~0xFu);
    if (!(mlo & ~0xFu)) return 0;
    return ~mask + 1;
}

static void collect_used(void) {
    used_count = 0;
    assign_low = 0xFFFFFFFF;
    __asm__ volatile("cli");
    for (int bus = 0; bus < 256; bus++)
        for (uint8_t dev = 0; dev < 32; dev++)
            for (uint8_t fn = 0; fn < 8; fn++) {
                uint32_t id = pci_config_read_dword((uint8_t)bus, dev, fn, 0);
                if ((id & 0xFFFF) == 0xFFFF) { if (!fn) break; continue; }
                uint8_t type = (pci_config_read_dword((uint8_t)bus, dev, fn, 0x0C) >> 16) & 0xFF;
                int bars = (type & 0x7F) == 0 ? 6 : (type & 0x7F) == 1 ? 2 : 0;
                for (int i = 0; i < bars; i++) {
                    uint8_t off = (uint8_t)(0x10 + 4 * i);
                    uint32_t v = pci_config_read_dword((uint8_t)bus, dev, fn, off);
                    if (v & 1) continue;                            /* I/O ports */
                    int is64 = ((v >> 1) & 3) == 2;
                    uint64_t base = v & ~0xFu;
                    if (is64) base |= (uint64_t)pci_config_read_dword((uint8_t)bus, dev, fn, off + 4) << 32;
                    if (base) {
                        add_used(base, bar_size((uint8_t)bus, dev, fn, off, is64));
                        if (base < 0x100000000ull && base >= 0x10000000 && base < assign_low) assign_low = (uint32_t)base;
                    }
                    if (is64) i++;
                }
                if ((type & 0x7F) == 1) {                           /* bridge windows */
                    uint32_t m = pci_config_read_dword((uint8_t)bus, dev, fn, 0x20);
                    uint64_t mb = (uint64_t)(m & 0xFFF0) << 16, ml = ((uint64_t)(m >> 16 & 0xFFF0) << 16) | 0xFFFFF;
                    if (ml > mb) add_used(mb, ml - mb + 1);
                    uint32_t pm = pci_config_read_dword((uint8_t)bus, dev, fn, 0x24);
                    uint64_t pb = ((uint64_t)(pm & 0xFFF0) << 16) | ((uint64_t)pci_config_read_dword((uint8_t)bus, dev, fn, 0x28) << 32);
                    uint64_t pl = ((uint64_t)(pm >> 16 & 0xFFF0) << 16) | 0xFFFFF |
                                  ((uint64_t)pci_config_read_dword((uint8_t)bus, dev, fn, 0x2C) << 32);
                    if (pl > pb) add_used(pb, pl - pb + 1);
                }
                if (!fn && !(type & 0x80)) break;                    /* single-function device */
            }
    __asm__ volatile("sti");

    /* The PCI configuration window (Intel host bridge PCIEXBAR) and the top 64 MiB. */
    uint64_t pciex = pci_config_read_dword(0, 0, 0, 0x60) | (uint64_t)pci_config_read_dword(0, 0, 0, 0x64) << 32;
    if (pciex & 1) {
        uint64_t len = (pciex >> 1 & 3) == 1 ? (128ull << 20) : (pciex >> 1 & 3) == 2 ? (64ull << 20) : (256ull << 20);
        add_used(pciex & 0x7FFC000000ull, len);
    }
    add_used(0xFC000000ull, 0x4000000ull);
}

static int overlaps(uint64_t a, uint64_t size) {
    for (int i = 0; i < used_count; i++)
        if (a < used_end[i] && used_start[i] < a + size) return 1;
    return 0;
}

static uint64_t find_free(uint64_t size) {
    uint32_t tolud = pci_config_read_dword(0, 0, 0, 0xBC) & 0xFFF00000u;   /* top of low RAM */
    uint64_t floor = assign_low != 0xFFFFFFFF ? assign_low : tolud;
    if (floor < tolud) floor = tolud;
    uint64_t best = 0;
    /* Candidates: just after each used range, aligned. Take the lowest one. */
    for (int i = -1; i < used_count; i++) {
        uint64_t a = i < 0 ? floor : used_end[i];
        a = (a + size - 1) & ~(size - 1);
        if (a < floor || a + size > 0xFC000000ull || overlaps(a, size)) continue;
        if (!best || a < best) best = a;
    }
    k_snprintf(assign_note, sizeof(assign_note), "TOLUD %x, firmware BARs from %x, %d ranges in use",
               tolud, assign_low, used_count);
    return best;
}

uint64_t pci_assign_bar0(uint8_t bus, uint8_t slot, uint8_t func) {
    uint32_t v = pci_config_read_dword(bus, slot, func, 0x10);
    int is64 = ((v >> 1) & 3) == 2;
    uint64_t size = bar_size(bus, slot, func, 0x10, is64);
    if (!size || size > (16u << 20)) return 0;
    if (size < 4096) size = 4096;
    if (!used_count) collect_used();
    uint64_t a = find_free(size);
    if (!a) return 0;
    pci_config_write_dword(bus, slot, func, 0x10, (uint32_t)a | (v & 0xF));
    if (is64) pci_config_write_dword(bus, slot, func, 0x14, 0);
    add_used(a, size);
    return a;
}

uint64_t pci_find_free_window(uint64_t size) {
    if (!used_count) collect_used();
    return find_free(size);
}

const char *pci_assign_note(void) { return assign_note; }

/* PCI power management: put the device in D0 (from D3hot). Leaving D3hot
 * can reset the device, command register and BARs included. Returns the
 * power register before and after (bits 0-1: D0-D3), or 0 without one. */
uint32_t pci_power_on(uint8_t bus, uint8_t slot, uint8_t func, uint32_t *after) {
    if (after) *after = 0;
    if (!(pci_config_read_dword(bus, slot, func, 0x04) & (1u << 20))) return 0;   /* no capability list */
    uint8_t cap = pci_config_read_byte(bus, slot, func, 0x34) & 0xFC;
    for (int guard = 0; cap && guard < 32; guard++) {
        if (pci_config_read_byte(bus, slot, func, cap) == 0x01) {
            uint32_t pmcsr = pci_config_read_dword(bus, slot, func, cap + 4);
            if (pmcsr & 3) {
                pci_config_write_dword(bus, slot, func, cap + 4, pmcsr & ~3u);
                for (volatile int i = 0; i < 20000000; i++) {}         /* ~10 ms settle */
            }
            if (after) *after = pci_config_read_dword(bus, slot, func, cap + 4);
            return pmcsr;
        }
        cap = pci_config_read_byte(bus, slot, func, cap + 1) & 0xFC;
    }
    return 0;
}
