#include "drivers/e1000.h"
#include "drivers/pci.h"
#include "drivers/pic.h"
#include "drivers/paging.h"
#include "lib/print.h"
#include "drivers/heap.h"
#include "lib/string.h"
#include <stdint.h>

/* Registers (offsets in the BAR0 window), from Intel's 8254x developer manual. */
#define REG_CTRL    0x0000
#define REG_STATUS  0x0008
#define REG_EERD    0x0014   /* EEPROM read */
#define REG_ICR     0x00C0   /* interrupt cause, cleared by reading */
#define REG_IMS     0x00D0   /* interrupt mask set */
#define REG_IMC     0x00D8   /* interrupt mask clear */
#define REG_RCTL    0x0100
#define REG_TCTL    0x0400
#define REG_TIPG    0x0410
#define REG_RDBAL   0x2800
#define REG_RDBAH   0x2804
#define REG_RDLEN   0x2808
#define REG_RDH     0x2810
#define REG_RDT     0x2818
#define REG_TDBAL   0x3800
#define REG_TDBAH   0x3804
#define REG_TDLEN   0x3808
#define REG_TDH     0x3810
#define REG_TDT     0x3818
#define REG_MTA     0x5200   /* multicast table, 128 dwords */
#define REG_RAL0    0x5400   /* receive address (our MAC) */
#define REG_RAH0    0x5404

#define CTRL_ASDE   (1u << 5)    /* auto speed detection */
#define CTRL_SLU    (1u << 6)    /* set link up */
#define CTRL_RST    (1u << 26)

#define STATUS_LU   (1u << 1)    /* link up */

#define RCTL_EN     (1u << 1)
#define RCTL_MPE    (1u << 4)    /* all multicast */
#define RCTL_BAM    (1u << 15)   /* broadcast */
#define RCTL_SECRC  (1u << 26)   /* strip the CRC */
                                 /* BSIZE 00: 2048-byte buffers */
#define TCTL_EN     (1u << 1)
#define TCTL_PSP    (1u << 3)    /* pad short packets */
#define TCTL_CT     (0x10u << 4)
#define TCTL_COLD   (0x40u << 12)

#define INT_TXDW    (1u << 0)
#define INT_LSC     (1u << 2)    /* link status change */
#define INT_RXDMT0  (1u << 4)
#define INT_RXO     (1u << 6)
#define INT_RXT0    (1u << 7)

#define RAH_AV      (1u << 31)   /* address valid */

#define TX_CMD_EOP  0x01
#define TX_CMD_IFCS 0x02         /* insert the CRC */
#define TX_CMD_RS   0x08         /* report status (sets DD) */
#define DESC_DD     0x01         /* descriptor done */
#define RX_EOP      0x02

typedef struct {
    volatile uint64_t addr;
    volatile uint16_t length;
    volatile uint16_t checksum;
    volatile uint8_t status;
    volatile uint8_t errors;
    volatile uint16_t special;
} __attribute__((packed)) rx_desc_t;

typedef struct {
    volatile uint64_t addr;
    volatile uint16_t length;
    volatile uint8_t cso;
    volatile uint8_t cmd;
    volatile uint8_t status;
    volatile uint8_t css;
    volatile uint16_t special;
} __attribute__((packed)) tx_desc_t;

#define RX_COUNT    32           /* ring sizes: multiples of 8 (128-byte ring length) */
#define TX_COUNT    8
#define BUF_SIZE    2048
#define MMIO_SIZE   0x20000      /* 128 KiB register window */
#define PAGE_PCD    0x10         /* uncached: these are device registers */
#define PAGE_PWT    0x08

static const struct { uint16_t id; const char *model; } models[] = {
    {0x100E, "82540EM"},         /* VirtualBox default, QEMU -device e1000 */
    {0x100F, "82545EM"},         /* VirtualBox "PRO/1000 MT Server" */
    {0x1004, "82543GC"},         /* VirtualBox "PRO/1000 T Server" */
    {0x10D3, "82574L"},          /* QEMU -device e1000e */
};

static volatile uint8_t *regs;
static const char *model_name = "?";
static int newer_eerd;                   /* 82574: other EERD bit layout */
static uint8_t irq_line = NIC_IRQ_NONE;
static int ready;
static uint8_t mac_addr[6];
static nic_rx_cb_t rx_callback;
static nic_stats_t stats;

static rx_desc_t *rx_ring;
static tx_desc_t *tx_ring;
static uint8_t *rx_bufs, *tx_bufs;
static uint32_t rx_cur, tx_cur;

static inline void w32(uint32_t r, uint32_t v) { *(volatile uint32_t *)(regs + r) = v; }
static inline uint32_t r32(uint32_t r) { return *(volatile uint32_t *)(regs + r); }

static inline uint64_t irq_save(void) {
    uint64_t f;
    __asm__ volatile("pushfq; pop %0; cli" : "=r"(f) :: "memory");
    return f;
}
static inline void irq_restore(uint64_t f) {
    __asm__ volatile("push %0; popfq" :: "r"(f) : "memory", "cc");
}

static uint64_t phys(const void *v) { return (uint64_t)(uintptr_t)v; }   /* identity mapped */

static void *alloc_aligned(uint32_t size, uint32_t align) {
    uint8_t *p = kmalloc(size + align);
    if (!p) return 0;
    return (void *)(((uintptr_t)p + align - 1) & ~(uintptr_t)(align - 1));
}

/* One EEPROM word. The 82540/82545 put the address at bit 8 and "done" at bit
 * 4; the 82574 at bit 2 and bit 1. */
static int eeprom_read(uint8_t addr, uint16_t *out) {
    uint32_t start = newer_eerd ? ((uint32_t)addr << 2) | 1 : ((uint32_t)addr << 8) | 1;
    uint32_t done = newer_eerd ? (1u << 1) : (1u << 4);
    w32(REG_EERD, start);
    for (int spin = 0; spin < 1000000; spin++) {
        uint32_t v = r32(REG_EERD);
        if (v & done) {
            *out = (uint16_t)(v >> 16);
            return 0;
        }
    }
    return -1;
}

static int read_mac(void) {
    uint32_t lo = r32(REG_RAL0), hi = r32(REG_RAH0);
    if (lo || (hi & 0xFFFF)) {        // loaded from the EEPROM at reset
        for (int i = 0; i < 4; i++) mac_addr[i] = (uint8_t)(lo >> (i * 8));
        mac_addr[4] = (uint8_t)hi;
        mac_addr[5] = (uint8_t)(hi >> 8);
        return 0;
    }
    for (int i = 0; i < 3; i++) {
        uint16_t w;
        if (eeprom_read((uint8_t)i, &w) != 0) return -1;
        mac_addr[i * 2] = (uint8_t)w;
        mac_addr[i * 2 + 1] = (uint8_t)(w >> 8);
    }
    w32(REG_RAL0, mac_addr[0] | mac_addr[1] << 8 | mac_addr[2] << 16 | (uint32_t)mac_addr[3] << 24);
    w32(REG_RAH0, mac_addr[4] | mac_addr[5] << 8 | RAH_AV);
    return 0;
}

int e1000_probe_init(void) {
    uint8_t bus = 0, slot = 0, func = 0;
    int found = -1;
    for (unsigned i = 0; i < sizeof(models) / sizeof(models[0]); i++)
        if (pci_find_device(0x8086, models[i].id, &bus, &slot, &func) == 0) { found = (int)i; break; }
    if (found < 0) return -1;
    model_name = models[found].model;
    newer_eerd = models[found].id == 0x10D3;
    kprintf("[NET] Intel %s (e1000) found at bus %u slot %u func %u\n", model_name, bus, slot, func);

    /* Memory space + bus mastering. */
    uint32_t cmd = pci_config_read_dword(bus, slot, func, 0x04);
    pci_config_write_dword(bus, slot, func, 0x04, cmd | 0x2 | 0x4);

    uint32_t bar0 = pci_config_read_dword(bus, slot, func, 0x10);
    if (bar0 & 1) { print_str("[NET] e1000: BAR0 is not a memory BAR\n"); return -1; }
    uint64_t base = bar0 & ~0xFu;
    if (((bar0 >> 1) & 3) == 2) base |= (uint64_t)pci_config_read_dword(bus, slot, func, 0x14) << 32;
    if (!base) { print_str("[NET] e1000: BAR0 not assigned\n"); return -1; }
    for (uint64_t a = base; a < base + MMIO_SIZE; a += PAGE_SIZE)
        map_page(a, a, PAGE_PRESENT | PAGE_RW | PAGE_PCD | PAGE_PWT);
    regs = (volatile uint8_t *)base;

    uint8_t line = pci_config_read_byte(bus, slot, func, 0x3C);
    irq_line = (line >= 1 && line < 16) ? line : NIC_IRQ_NONE;

    /* Reset, then bring the link up. */
    w32(REG_IMC, 0xFFFFFFFF);
    w32(REG_CTRL, r32(REG_CTRL) | CTRL_RST);
    for (volatile int spin = 0; spin < 100000; spin++) {}
    for (int spin = 0; spin < 1000000 && (r32(REG_CTRL) & CTRL_RST); spin++) {}
    w32(REG_IMC, 0xFFFFFFFF);
    r32(REG_ICR);
    w32(REG_CTRL, (r32(REG_CTRL) | CTRL_SLU | CTRL_ASDE) & ~CTRL_RST);

    if (read_mac() != 0) { print_str("[NET] e1000: cannot read the MAC address\n"); return -1; }
    for (int i = 0; i < 128; i++) w32(REG_MTA + i * 4, 0);

    rx_ring = alloc_aligned(RX_COUNT * sizeof(rx_desc_t), 128);
    tx_ring = alloc_aligned(TX_COUNT * sizeof(tx_desc_t), 128);
    rx_bufs = alloc_aligned(RX_COUNT * BUF_SIZE, 16);
    tx_bufs = alloc_aligned(TX_COUNT * BUF_SIZE, 16);
    if (!rx_ring || !tx_ring || !rx_bufs || !tx_bufs) {
        print_str("[NET] e1000: out of memory\n");
        return -1;
    }
    for (uint32_t i = 0; i < RX_COUNT; i++) {
        memset((void *)&rx_ring[i], 0, sizeof(rx_desc_t));
        rx_ring[i].addr = phys(rx_bufs + i * BUF_SIZE);
    }
    for (uint32_t i = 0; i < TX_COUNT; i++) {
        memset((void *)&tx_ring[i], 0, sizeof(tx_desc_t));
        tx_ring[i].addr = phys(tx_bufs + i * BUF_SIZE);
        tx_ring[i].status = DESC_DD;          // free
    }
    rx_cur = tx_cur = 0;

    w32(REG_RDBAL, (uint32_t)phys(rx_ring));
    w32(REG_RDBAH, (uint32_t)(phys(rx_ring) >> 32));
    w32(REG_RDLEN, RX_COUNT * sizeof(rx_desc_t));
    w32(REG_RDH, 0);
    w32(REG_RDT, RX_COUNT - 1);
    w32(REG_RCTL, RCTL_EN | RCTL_MPE | RCTL_BAM | RCTL_SECRC);

    w32(REG_TDBAL, (uint32_t)phys(tx_ring));
    w32(REG_TDBAH, (uint32_t)(phys(tx_ring) >> 32));
    w32(REG_TDLEN, TX_COUNT * sizeof(tx_desc_t));
    w32(REG_TDH, 0);
    w32(REG_TDT, 0);
    w32(REG_TIPG, 10 | (8 << 10) | (6 << 20));
    w32(REG_TCTL, TCTL_EN | TCTL_PSP | TCTL_CT | TCTL_COLD);

    r32(REG_ICR);
    w32(REG_IMS, INT_RXT0 | INT_RXO | INT_RXDMT0 | INT_LSC | INT_TXDW);

    ready = 1;
    kprintf("[NET] e1000 registers at 0x%lx, IRQ %s, link %s\n", base,
            irq_line == NIC_IRQ_NONE ? "none (polling)" : "PIC", e1000_link_up() ? "up" : "down");
    if (irq_line != NIC_IRQ_NONE) enable_irq(irq_line);
    return 0;
}

static void handle_rx(void) {
    for (uint32_t n = 0; n < RX_COUNT; n++) {
        rx_desc_t *d = &rx_ring[rx_cur];
        if (!(d->status & DESC_DD)) break;
        uint16_t len = d->length;
        if (!(d->status & RX_EOP) || d->errors || len < 14 || len > NIC_MAX_FRAME) {
            stats.rx_errors++;
        } else {
            stats.rx_packets++;
            stats.rx_bytes += len;
            if (rx_callback) rx_callback(rx_bufs + rx_cur * BUF_SIZE, len);
        }
        d->status = 0;
        w32(REG_RDT, rx_cur);                 // hand the descriptor back
        rx_cur = (rx_cur + 1) % RX_COUNT;
    }
}

uint8_t e1000_get_irq(void) { return irq_line; }

void e1000_handle_irq(void) {
    if (!ready) return;
    r32(REG_ICR);                             // reading clears it
    handle_rx();                              // always scan: polling may come first
}

int e1000_link_up(void) { return regs && (r32(REG_STATUS) & STATUS_LU) != 0; }

const char *e1000_model(void) { return model_name; }

void e1000_get_mac(uint8_t mac[6]) { for (int i = 0; i < 6; i++) mac[i] = mac_addr[i]; }

void e1000_set_rx_handler(nic_rx_cb_t cb) { rx_callback = cb; }

void e1000_get_stats(nic_stats_t *out) { *out = stats; }

int e1000_send(const void *frame, uint16_t len) {
    if (!ready || !frame || len == 0 || len > NIC_MAX_FRAME) return -1;

    uint64_t flags = irq_save();
    tx_desc_t *d = &tx_ring[tx_cur];
    for (int spin = 0; spin < 1000000 && !(d->status & DESC_DD); spin++) {}
    if (!(d->status & DESC_DD)) {
        stats.tx_errors++;
        irq_restore(flags);
        return -1;
    }
    memcpy(tx_bufs + tx_cur * BUF_SIZE, frame, len);
    d->length = len;
    d->cso = 0;
    d->css = 0;
    d->special = 0;
    d->cmd = TX_CMD_EOP | TX_CMD_IFCS | TX_CMD_RS;
    d->status = 0;
    __asm__ volatile("" ::: "memory");        // descriptor before the tail moves
    tx_cur = (tx_cur + 1) % TX_COUNT;
    w32(REG_TDT, tx_cur);

    stats.tx_packets++;
    stats.tx_bytes += len;
    irq_restore(flags);
    return 0;
}
