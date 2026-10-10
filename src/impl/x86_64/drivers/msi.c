// msi.c - PCI MSI / MSI-X (see drivers/msi.h)
#include "drivers/msi.h"
#include "sys/smp.h"
#include "drivers/apic.h"
#include "drivers/pci.h"
#include "drivers/paging.h"
#include "core/idt.h"
#include "lib/print.h"
#include "lib/string.h"

#define MSI_BASE_VECTOR 0x50
#define MSI_SLOTS       32
#define CAP_MSI         0x05
#define CAP_MSIX        0x11

extern void *msi_stub_table[MSI_SLOTS];

typedef struct {
    void (*handler)(void);
    char name[16];
    const char *kind;
    volatile uint32_t count;
} msi_slot_t;

static msi_slot_t slots[MSI_SLOTS];
static int ready, used;

int msi_init(void) {
    if (ready) return 0;
    if (apic_msi_ready() != 0) return -1;
    ready = 1;
    return 0;
}

/* From the stubs (interrupts are off). */
void msi_dispatch(uint64_t slot) {
    bkl_enter();
    if (slot < MSI_SLOTS && slots[slot].handler) {
        slots[slot].count++;
        slots[slot].handler();
    }
    bkl_leave();
    apic_eoi();
}

static uint8_t find_cap(uint8_t bus, uint8_t slot, uint8_t func, uint8_t id) {
    if (!(pci_config_read_dword(bus, slot, func, 0x04) >> 16 & 0x10)) return 0;   /* no capability list */
    uint8_t p = pci_config_read_byte(bus, slot, func, 0x34) & 0xFC;
    for (int guard = 0; p && guard < 48; guard++) {
        uint32_t h = pci_config_read_dword(bus, slot, func, p);
        if ((h & 0xFF) == id) return p;
        p = (uint8_t)((h >> 8) & 0xFC);
    }
    return 0;
}

static uint64_t bar_address(uint8_t bus, uint8_t slot, uint8_t func, int bir) {
    uint8_t off = (uint8_t)(0x10 + 4 * bir);
    uint32_t lo = pci_config_read_dword(bus, slot, func, off);
    if (lo & 1) return 0;                                   /* I/O space */
    uint64_t a = lo & ~0xFull;
    if (((lo >> 1) & 3) == 2) a |= (uint64_t)pci_config_read_dword(bus, slot, func, (uint8_t)(off + 4)) << 32;
    return a;
}

static int setup_msi(uint8_t bus, uint8_t slot, uint8_t func, uint8_t cap, uint32_t addr, uint32_t data) {
    uint32_t h = pci_config_read_dword(bus, slot, func, cap);
    uint16_t ctrl = (uint16_t)(h >> 16);
    int is64 = (ctrl >> 7) & 1;
    ctrl &= (uint16_t)~(0x70 | 1);                          /* one message, off while we write */
    pci_config_write_dword(bus, slot, func, cap, (h & 0xFFFF) | (uint32_t)ctrl << 16);
    pci_config_write_dword(bus, slot, func, (uint8_t)(cap + 4), addr);
    if (is64) {
        pci_config_write_dword(bus, slot, func, (uint8_t)(cap + 8), 0);
        pci_config_write_dword(bus, slot, func, (uint8_t)(cap + 12), data);
    } else {
        pci_config_write_dword(bus, slot, func, (uint8_t)(cap + 8), data);
    }
    if (ctrl & 0x100) {                                     /* per-vector masks: unmask vector 0 */
        uint8_t mask = (uint8_t)(cap + (is64 ? 16 : 12));
        pci_config_write_dword(bus, slot, func, mask, pci_config_read_dword(bus, slot, func, mask) & ~1u);
    }
    pci_config_write_dword(bus, slot, func, cap, (h & 0xFFFF) | (uint32_t)(ctrl | 1) << 16);
    return 0;
}

static int setup_msix(uint8_t bus, uint8_t slot, uint8_t func, uint8_t cap, uint32_t addr, uint32_t data) {
    uint32_t h = pci_config_read_dword(bus, slot, func, cap);
    uint16_t ctrl = (uint16_t)(h >> 16);
    uint32_t entries = (ctrl & 0x7FF) + 1u;
    uint32_t t = pci_config_read_dword(bus, slot, func, (uint8_t)(cap + 4));
    uint64_t bar = bar_address(bus, slot, func, (int)(t & 7));
    if (!bar) return -1;
    uint64_t table_phys = bar + (t & ~7u);
    volatile uint32_t *table = mmio_map(table_phys & ~0xFFFull, ((table_phys & 0xFFF) + entries * 16 + 0xFFF) & ~0xFFFull);
    if (!table) return -1;
    table = (volatile uint32_t *)((volatile uint8_t *)table + (table_phys & 0xFFF));
    /* Enabled with the whole function masked while the table is written. */
    pci_config_write_dword(bus, slot, func, cap, (h & 0xFFFF) | (uint32_t)(ctrl | 0x8000 | 0x4000) << 16);
    for (uint32_t i = 0; i < entries; i++) {                /* every entry to our vector; only 0 unmasked */
        table[i * 4] = addr;
        table[i * 4 + 1] = 0;
        table[i * 4 + 2] = data;
        table[i * 4 + 3] = i == 0 ? 0 : 1;
    }
    pci_config_write_dword(bus, slot, func, cap, (h & 0xFFFF) | (uint32_t)((ctrl | 0x8000) & ~0x4000) << 16);
    return 0;
}

int msi_enable(uint8_t bus, uint8_t slot, uint8_t func, void (*handler)(void), const char *name, int flags) {
    if (!ready || used >= MSI_SLOTS) return -1;
    uint8_t msi = find_cap(bus, slot, func, CAP_MSI), msix = find_cap(bus, slot, func, CAP_MSIX);
    if (!msi && !msix) return -1;
    int n = used;
    msi_slot_t *s = &slots[n];
    s->handler = handler;
    s->count = 0;
    kstrncpy(s->name, name, sizeof(s->name));
    idt_set_entry(MSI_BASE_VECTOR + n, msi_stub_table[n], 0x8E);
    uint32_t addr = 0xFEE00000u | (apic_id() & 0xFF) << 12;
    uint32_t data = (uint32_t)(MSI_BASE_VECTOR + n);        /* fixed delivery, edge */
    int use_msix = msix && (!msi || (flags & MSI_PREFER_MSIX));
    int r = use_msix ? setup_msix(bus, slot, func, msix, addr, data) : setup_msi(bus, slot, func, msi, addr, data);
    if (r != 0 && use_msix && msi) {
        use_msix = 0;
        r = setup_msi(bus, slot, func, msi, addr, data);
    }
    if (r != 0) {
        s->handler = 0;
        return -1;
    }
    s->kind = use_msix ? "MSI-X" : "MSI";
    /* No legacy INTx line any more. */
    uint32_t cmd = pci_config_read_dword(bus, slot, func, 0x04);
    pci_config_write_dword(bus, slot, func, 0x04, (cmd & 0xFFFF) | 0x400);
    used++;
    return 0;
}

uint32_t msi_count(const char *name) {
    for (int i = 0; i < used; i++)
        if (!strcmp(slots[i].name, name)) return slots[i].count;
    return 0;
}

void msi_print(void) {
    if (!ready) {
        kprintf("MSI is not set up (no local APIC?).\n");
        return;
    }
    kprintf("  vector  device           kind    count\n");
    for (int i = 0; i < used; i++)
        kprintf("  0x%02x    %-16s %-6s  %u\n", MSI_BASE_VECTOR + i, slots[i].name, slots[i].kind, slots[i].count);
    if (!used) kprintf("  (no device uses MSI)\n");
    kprintf("Other devices are polled from the timer (100 Hz) or use the 8259 PIC.\n");
}
