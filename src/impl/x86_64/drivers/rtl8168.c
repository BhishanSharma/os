#include "drivers/rtl8168.h"
#include "drivers/pci.h"
#include "drivers/pic.h"
#include "../lib/ports.h"
#include "lib/print.h"
#include "drivers/heap.h"
#include "lib/string.h"
#include <stdint.h>

/* Register offsets (I/O base + offset), from the RTL8168 datasheet / Linux r8169. */
#define R_IDR0        0x00   /* MAC address */
#define R_MAR0        0x08   /* multicast filter, 8 bytes */
#define R_TNPDS_LO    0x20   /* TX normal-priority descriptor ring, 64-bit */
#define R_TNPDS_HI    0x24
#define R_CMD         0x37
#define R_TPPOLL      0x38
#define R_IMR         0x3C
#define R_ISR         0x3E
#define R_TCR         0x40
#define R_RCR         0x44
#define R_9346CR      0x50
#define R_PHYSTATUS   0x6C
#define R_RMS         0xDA   /* RX max packet size */
#define R_CPCR        0xE0   /* C+ command */
#define R_RDSAR_LO    0xE4   /* RX descriptor ring, 64-bit */
#define R_RDSAR_HI    0xE8
#define R_MTPS        0xEC   /* max TX packet size, 128-byte units */

#define CMD_RST  0x10
#define CMD_RE   0x08
#define CMD_TE   0x04

#define TPPOLL_NPQ 0x40      /* "normal priority queue has new descriptors" */

#define CFG_UNLOCK 0xC0
#define CFG_LOCK   0x00

/* IMR/ISR bits */
#define INT_ROK   0x0001
#define INT_RER   0x0002
#define INT_TOK   0x0004
#define INT_TER   0x0008
#define INT_RDU   0x0010   /* RX descriptor unavailable (ring full) */
#define INT_LINK  0x0020
#define INT_FOVW  0x0040
#define INT_MASK  (INT_ROK | INT_RER | INT_TOK | INT_TER | INT_RDU | INT_LINK | INT_FOVW)

/* RCR: accept physical match, multicast and broadcast; no RX FIFO threshold,
 * unlimited DMA burst. Bits 14-15 are the 8168's multi-descriptor/128-int
 * enables, which Linux sets too. */
#define RCR_APM  (1u << 1)
#define RCR_AM   (1u << 2)
#define RCR_AB   (1u << 3)
#define RCR_VALUE (RCR_APM | RCR_AM | RCR_AB | (7u << 8) | (7u << 13) | (1u << 14) | (1u << 15))

/* TCR: standard inter-frame gap, unlimited DMA burst. */
#define TCR_VALUE ((3u << 24) | (7u << 8))

/* Descriptor opts1 bits */
#define DESC_OWN (1u << 31)   /* owned by the NIC */
#define DESC_EOR (1u << 30)   /* last descriptor in the ring */
#define DESC_FS  (1u << 29)   /* first segment of a frame */
#define DESC_LS  (1u << 28)   /* last segment of a frame */
#define RX_RES   (1u << 21)   /* receive error summary */

#define PHY_LINK 0x02

typedef struct {
    volatile uint32_t opts1;
    volatile uint32_t opts2;
    volatile uint64_t addr;
} __attribute__((packed)) rtl_desc_t;

/* Kept small: the kernel heap is 1 MiB and downloads need most of it. */
#define RX_COUNT    32
#define TX_COUNT    8
#define BUF_SIZE    1536       /* multiple of 8; holds a full frame plus CRC */
#define ETH_MIN_FRAME 60

static uint16_t io_base;
static uint8_t irq_line = NIC_IRQ_NONE;
static int ready;
static uint8_t mac_addr[6];
static nic_rx_cb_t rx_callback;
static nic_stats_t stats;

static rtl_desc_t *rx_ring, *tx_ring;
static uint8_t *rx_bufs, *tx_bufs;
static uint32_t rx_cur, tx_cur;

static inline void w8(uint16_t r, uint8_t v)   { outb(io_base + r, v); }
static inline void w16(uint16_t r, uint16_t v) { outw(io_base + r, v); }
static inline void w32(uint16_t r, uint32_t v) { outl(io_base + r, v); }
static inline uint8_t  r8(uint16_t r)  { return inb(io_base + r); }
static inline uint16_t r16(uint16_t r) { return inw(io_base + r); }

static inline uint64_t irq_save(void) {
    uint64_t f;
    __asm__ volatile("pushfq; pop %0; cli" : "=r"(f) :: "memory");
    return f;
}
static inline void irq_restore(uint64_t f) {
    __asm__ volatile("push %0; popfq" :: "r"(f) : "memory", "cc");
}

/* Identity mapped: physical == virtual. */
static uint64_t phys(const void *v) { return (uint64_t)(uintptr_t)v; }

/* Descriptor rings must be 256-byte aligned; kmalloc gives no such promise. */
static void *alloc_aligned(uint32_t size, uint32_t align) {
    uint8_t *p = kmalloc(size + align);
    if (!p) return 0;
    return (void *)(((uintptr_t)p + align - 1) & ~(uintptr_t)(align - 1));
}

static void rx_give_back(uint32_t i) {
    uint32_t opts = DESC_OWN | BUF_SIZE;
    if (i == RX_COUNT - 1) opts |= DESC_EOR;
    rx_ring[i].opts2 = 0;
    rx_ring[i].opts1 = opts;
}

int rtl8168_probe_init(void) {
    static const uint16_t ids[] = {0x8168, 0x8161, 0x8169};
    uint8_t bus = 0, slot = 0, func = 0;
    uint16_t dev = 0;
    for (unsigned i = 0; i < sizeof(ids) / sizeof(ids[0]); i++) {
        if (pci_find_device(0x10EC, ids[i], &bus, &slot, &func) == 0) { dev = ids[i]; break; }
    }
    if (!dev) return -1;
    kprintf("[NET] %s found at bus %u slot %u func %u\n",
            dev == 0x8169 ? "RTL8169" : "RTL8168/8111", bus, slot, func);

    /* I/O space + bus mastering. */
    uint32_t cmd = pci_config_read_dword(bus, slot, func, 0x04);
    pci_config_write_dword(bus, slot, func, 0x04, cmd | 0x1 | 0x4);

    /* BAR0 is the I/O-port window on every 8168/8169. */
    uint32_t bar0 = pci_config_read_dword(bus, slot, func, 0x10);
    if (!(bar0 & 1)) { print_str("[NET] RTL8168: BAR0 is not an I/O BAR\n"); return -1; }
    io_base = (uint16_t)(bar0 & ~0x3u);

    uint8_t line = pci_config_read_byte(bus, slot, func, 0x3C);
    irq_line = (line >= 1 && line < 16) ? line : NIC_IRQ_NONE;

    w8(R_CMD, CMD_RST);
    uint32_t spin = 1000000;
    while ((r8(R_CMD) & CMD_RST) && spin--) {}
    if (r8(R_CMD) & CMD_RST) { print_str("[NET] RTL8168: reset timed out\n"); return -1; }

    for (int i = 0; i < 6; i++) mac_addr[i] = r8(R_IDR0 + i);

    rx_ring = alloc_aligned(RX_COUNT * sizeof(rtl_desc_t), 256);
    tx_ring = alloc_aligned(TX_COUNT * sizeof(rtl_desc_t), 256);
    rx_bufs = alloc_aligned(RX_COUNT * BUF_SIZE, 8);
    tx_bufs = alloc_aligned(TX_COUNT * BUF_SIZE, 8);
    if (!rx_ring || !tx_ring || !rx_bufs || !tx_bufs) {
        print_str("[NET] RTL8168: out of memory\n");
        return -1;
    }
    for (uint32_t i = 0; i < RX_COUNT; i++) {
        rx_ring[i].addr = phys(rx_bufs + i * BUF_SIZE);
        rx_give_back(i);
    }
    for (uint32_t i = 0; i < TX_COUNT; i++) {
        tx_ring[i].addr = phys(tx_bufs + i * BUF_SIZE);
        tx_ring[i].opts2 = 0;
        tx_ring[i].opts1 = (i == TX_COUNT - 1) ? DESC_EOR : 0;
    }
    rx_cur = tx_cur = 0;

    w8(R_9346CR, CFG_UNLOCK);
    w16(R_CPCR, r16(R_CPCR));                 /* keep firmware's C+ settings */
    w16(R_RMS, BUF_SIZE);
    w8(R_MTPS, 0x3B);
    w32(R_TNPDS_LO, (uint32_t)phys(tx_ring));
    w32(R_TNPDS_HI, (uint32_t)(phys(tx_ring) >> 32));
    w32(R_RDSAR_LO, (uint32_t)phys(rx_ring));
    w32(R_RDSAR_HI, (uint32_t)(phys(rx_ring) >> 32));
    for (int i = 0; i < 8; i++) w8(R_MAR0 + i, 0xFF);   /* accept all multicast */
    w8(R_CMD, CMD_RE | CMD_TE);
    /* Newer 8168 revisions only latch RCR/TCR once RX/TX are enabled. */
    w32(R_TCR, TCR_VALUE);
    w32(R_RCR, RCR_VALUE);
    w8(R_9346CR, CFG_LOCK);

    w16(R_ISR, 0xFFFF);
    w16(R_IMR, INT_MASK);

    ready = 1;
    kprintf("[NET] RTL8168 IO base=%x IRQ=%s, link %s\n", io_base,
            irq_line == NIC_IRQ_NONE ? "none (polling)" : "PIC", rtl8168_link_up() ? "up" : "down");
    if (irq_line != NIC_IRQ_NONE) enable_irq(irq_line);
    return 0;
}

static void handle_rx(void) {
    for (uint32_t n = 0; n < RX_COUNT; n++) {
        rtl_desc_t *d = &rx_ring[rx_cur];
        uint32_t opts = d->opts1;
        if (opts & DESC_OWN) break;

        uint32_t len = opts & 0x3FFF;               /* includes the 4-byte CRC */
        int whole = (opts & (DESC_FS | DESC_LS)) == (DESC_FS | DESC_LS);
        if ((opts & RX_RES) || !whole || len < 18 || len > NIC_MAX_FRAME + 4) {
            stats.rx_errors++;
        } else {
            uint16_t frame_len = (uint16_t)(len - 4);
            stats.rx_packets++;
            stats.rx_bytes += frame_len;
            if (rx_callback) rx_callback(rx_bufs + rx_cur * BUF_SIZE, frame_len);
        }
        rx_give_back(rx_cur);
        rx_cur = (rx_cur + 1) % RX_COUNT;
    }
}

uint8_t rtl8168_get_irq(void) { return irq_line; }

void rtl8168_handle_irq(void) {
    if (!ready) return;
    uint16_t isr = r16(R_ISR);
    if (isr) w16(R_ISR, isr);
    /* Always scan the ring: when polling, ISR may already have been cleared. */
    handle_rx();
    if (isr & INT_TER) stats.tx_errors++;
}

int rtl8168_link_up(void) { return io_base && (r8(R_PHYSTATUS) & PHY_LINK) != 0; }

void rtl8168_get_mac(uint8_t mac[6]) { for (int i = 0; i < 6; i++) mac[i] = mac_addr[i]; }

void rtl8168_set_rx_handler(nic_rx_cb_t cb) { rx_callback = cb; }

void rtl8168_get_stats(nic_stats_t *out) { *out = stats; }

int rtl8168_send(const void *frame, uint16_t len) {
    if (!ready || !frame || len == 0 || len > NIC_MAX_FRAME) return -1;

    uint64_t flags = irq_save();
    rtl_desc_t *d = &tx_ring[tx_cur];
    uint32_t spin = 1000000;
    while ((d->opts1 & DESC_OWN) && spin--) {}
    if (d->opts1 & DESC_OWN) {
        stats.tx_errors++;
        irq_restore(flags);
        return -1;
    }

    uint8_t *buf = tx_bufs + tx_cur * BUF_SIZE;
    uint16_t send_len = len < ETH_MIN_FRAME ? ETH_MIN_FRAME : len;
    memcpy(buf, frame, len);
    if (send_len > len) memset(buf + len, 0, send_len - len);

    uint32_t opts = DESC_OWN | DESC_FS | DESC_LS | send_len;
    if (tx_cur == TX_COUNT - 1) opts |= DESC_EOR;
    d->opts2 = 0;
    __asm__ volatile("" ::: "memory");          /* buffer and opts2 before OWN */
    d->opts1 = opts;
    w8(R_TPPOLL, TPPOLL_NPQ);

    tx_cur = (tx_cur + 1) % TX_COUNT;
    stats.tx_packets++;
    stats.tx_bytes += send_len;
    irq_restore(flags);
    return 0;
}
