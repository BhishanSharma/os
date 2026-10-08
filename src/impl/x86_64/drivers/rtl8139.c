#include "drivers/rtl8139.h"
#include "drivers/pci.h"
#include "drivers/pic.h"
#include "../lib/ports.h"
#include "lib/print.h"
#include "drivers/heap.h"     // kmalloc/kfree
#include "lib/string.h"       // memcpy, memset
#include <stdint.h>

/* RTL8139 register offsets (I/O base + offset) */
#define RTL_REG_MAC        0x00
#define RTL_REG_TSD0       0x10   /* TX status, 4 x 32 bit (0x10,0x14,0x18,0x1C) */
#define RTL_REG_TSAD0      0x20   /* TX start address, 4 x 32 bit */
#define RTL_REG_RBSTART    0x30
#define RTL_REG_CR         0x37
#define RTL_REG_CAPR       0x38
#define RTL_REG_IMR        0x3C
#define RTL_REG_ISR        0x3E
#define RTL_REG_RCR        0x44
#define RTL_REG_CONFIG1    0x52

/* CR bits */
#define RL_CR_BUFE 0x01   /* receive buffer empty */
#define RL_CR_TE   0x04
#define RL_CR_RE   0x08
#define RL_CR_RST  0x10

/* IMR/ISR bits */
#define RL_ISR_ROK    (1 << 0)
#define RL_ISR_RER    (1 << 1)
#define RL_ISR_TOK    (1 << 2)
#define RL_ISR_TER    (1 << 3)
#define RL_ISR_RXOVW  (1 << 4)

/* TX status bits */
#define RL_TSD_OWN   (1u << 13)   /* set by the chip when it has DMA'd the frame */
#define RL_TSD_TUN   (1u << 14)   /* FIFO underrun */
#define RL_TSD_TOK   (1u << 15)   /* transmit OK */
#define RL_TSD_OWC   (1u << 29)   /* out of window collision */
#define RL_TSD_TABT  (1u << 30)   /* aborted */

/* RCR bits: accept our unicast + multicast + broadcast, WRAP mode,
 * unlimited DMA burst (7<<8), no RX FIFO threshold (7<<13). */
#define RL_RCR_APM   (1u << 1)
#define RL_RCR_AM    (1u << 2)
#define RL_RCR_AB    (1u << 3)
#define RL_RCR_WRAP  (1u << 7)
#define RL_RCR_VALUE (RL_RCR_APM | RL_RCR_AM | RL_RCR_AB | RL_RCR_WRAP | (7u << 8) | (7u << 13))

/* RX packet header status */
#define RL_RX_ROK    0x0001

/* Receive ring is 8 KiB. In WRAP mode the chip keeps writing a packet
 * contiguously past the end of the ring instead of splitting it, so the buffer
 * needs 16 bytes + one max-size frame of slack behind the ring. */
#define RTL_RX_RING_SIZE   8192
#define RTL_RX_BUF_ALLOC   (RTL_RX_RING_SIZE + 16 + 1536)

#define RTL_TX_DESC_COUNT  4
#define RTL_TX_BUF_SIZE    1536
#define ETH_MIN_FRAME      60

static uint16_t io_base = 0;
static uint8_t  irq_line = 0xFF;
static int      nic_ready = 0;
static uint8_t  mac_addr[6];

static uint8_t *rx_buf_virt = 0;
static uint32_t rx_buf_phys = 0;
static uint32_t rx_offset = 0;
static rtl8139_rx_cb_t rx_callback = 0;

static uint8_t *tx_buf_virt[RTL_TX_DESC_COUNT];
static uint32_t tx_buf_phys[RTL_TX_DESC_COUNT];
static uint8_t  tx_cur = 0;       /* next descriptor to use */
static uint8_t  tx_used = 0;      /* bit i set: descriptor i has been used at least once */

static rtl8139_stats_t stats;

static inline void outb_io(uint16_t reg, uint8_t val) { outb(io_base + reg, val); }
static inline uint8_t inb_io(uint16_t reg) { return inb(io_base + reg); }
static inline void outw_io(uint16_t reg, uint16_t val) { outw(io_base + reg, val); }
static inline uint16_t inw_io(uint16_t reg) { return inw(io_base + reg); }
static inline void outl_io(uint16_t reg, uint32_t val) { outl(io_base + reg, val); }
static inline uint32_t inl_io(uint16_t reg) { return inl(io_base + reg); }

/* Disable interrupts and return the old RFLAGS, so rtl8139_send() can be
 * called from both the shell and the NIC interrupt without racing. */
static inline uint64_t irq_save(void) {
    uint64_t flags;
    __asm__ volatile ("pushfq; pop %0; cli" : "=r"(flags) : : "memory");
    return flags;
}
static inline void irq_restore(uint64_t flags) {
    __asm__ volatile ("push %0; popfq" : : "r"(flags) : "memory", "cc");
}

/* helper: read PCI BAR0 (I/O base) */
static uint32_t pci_get_bar0(uint8_t bus, uint8_t slot, uint8_t func) {
    uint32_t bar = pci_config_read_dword(bus, slot, func, 0x10);
    /* For I/O BAR, lowest bit set => I/O port. Bits [31:2] is base */
    if (bar & 1) {
        return bar & ~0x3u;
    }
    return 0; // we expect IO BAR for many RTL8139 in QEMU
}

/* Identity mapped: physical == virtual. */
static uint32_t virt_to_phys(void* v) {
    return (uint32_t)(uintptr_t)v;
}

/* (Re)start the receiver from an empty ring. */
static void rtl8139_rx_restart(void) {
    outb_io(RTL_REG_CR, RL_CR_TE);                 /* receiver off */
    outb_io(RTL_REG_CR, RL_CR_RE | RL_CR_TE);      /* and on again: resets the ring pointers */
    outl_io(RTL_REG_RBSTART, rx_buf_phys);
    outl_io(RTL_REG_RCR, RL_RCR_VALUE);
    rx_offset = 0;
    outw_io(RTL_REG_CAPR, (uint16_t)(rx_offset - 16));
}

int rtl8139_probe_init(void) {
    uint8_t bus=0, slot=0, func=0;
    if (pci_find_device(0x10EC, 0x8139, &bus, &slot, &func) != 0) {
        kprintf("[NET] RTL8139 not found\n");
        return -1;
    }
    kprintf("[NET] RTL8139 found at bus %u slot %u func %u\n", bus, slot, func);

    /* enable bus mastering in PCI command register (offset 0x04, bit 2) */
    uint32_t cmd = pci_config_read_dword(bus, slot, func, 0x04);
    cmd |= (1 << 2); // bus mastering bit
    pci_config_write_dword(bus, slot, func, 0x04, cmd);

    /* get I/O base from BAR0 */
    uint32_t bar0 = pci_get_bar0(bus, slot, func);
    if (!bar0) {
        kprintf("[NET] RTL8139: BAR0 not an IO BAR or missing\n");
        return -1;
    }
    io_base = (uint16_t)bar0;

    /* read interrupt line from PCI config space (offset 0x3C) */
    irq_line = pci_config_read_byte(bus, slot, func, 0x3C);
    kprintf("[NET] IO base=0x%x IRQ=%u\n", io_base, irq_line);

    /* Power on, then soft reset */
    outb_io(RTL_REG_CONFIG1, 0x00);
    outb_io(RTL_REG_CR, RL_CR_RST);
    {
        uint32_t spin = 1000000;
        while ((inb_io(RTL_REG_CR) & RL_CR_RST) && spin--) { /* wait for reset */ }
        if (inb_io(RTL_REG_CR) & RL_CR_RST) {
            kprintf("[NET] RTL8139: reset timed out\n");
            return -1;
        }
    }

    /* MAC address lives in the first 6 registers */
    for (int i = 0; i < 6; i++) mac_addr[i] = inb_io(RTL_REG_MAC + i);

    /* Receive ring */
    rx_buf_virt = kmalloc(RTL_RX_BUF_ALLOC);
    if (!rx_buf_virt) {
        kprintf("[NET] Failed to allocate RX buffer\n");
        return -1;
    }
    rx_buf_phys = virt_to_phys(rx_buf_virt);
    memset(rx_buf_virt, 0, RTL_RX_BUF_ALLOC);

    /* Four transmit buffers, one per TX descriptor */
    for (int i = 0; i < RTL_TX_DESC_COUNT; i++) {
        tx_buf_virt[i] = kmalloc(RTL_TX_BUF_SIZE);
        if (!tx_buf_virt[i]) {
            kprintf("[NET] Failed to allocate TX buffer\n");
            return -1;
        }
        tx_buf_phys[i] = virt_to_phys(tx_buf_virt[i]);
    }
    tx_cur = 0;
    tx_used = 0;

    /* Clear ISR, enable the interrupts we care about */
    outw_io(RTL_REG_ISR, 0xFFFF);
    outw_io(RTL_REG_IMR, RL_ISR_ROK | RL_ISR_RER | RL_ISR_TOK | RL_ISR_TER | RL_ISR_RXOVW);

    /* Enable receiver + transmitter, point the chip at the ring */
    outb_io(RTL_REG_CR, RL_CR_RE | RL_CR_TE);
    outl_io(RTL_REG_RBSTART, rx_buf_phys);
    outl_io(RTL_REG_RCR, RL_RCR_VALUE);
    rx_offset = 0;

    nic_ready = 1;
    kprintf("[NET] RTL8139 init complete\n");

    /* Unmask PIC for this IRQ. The IDT entry (0x20 + irq_line) is installed
     * by kernel_main(). */
    enable_irq(irq_line);
    return 0;
}

/* Drain the receive ring. Called from the interrupt handler. */
static void rtl8139_handle_rx(void) {
    while (!(inb_io(RTL_REG_CR) & RL_CR_BUFE)) {
        /* Each packet is preceded by a 4-byte header: status(2), length(2).
         * `length` includes the 4-byte CRC. */
        uint8_t *hdr = rx_buf_virt + rx_offset;
        uint16_t status = *(uint16_t *)hdr;
        uint16_t length = *(uint16_t *)(hdr + 2);

        if (!(status & RL_RX_ROK) || length < 8 || length > RTL8139_MAX_FRAME + 4) {
            /* Ring is out of sync or the chip flagged an error: start over. */
            stats.rx_errors++;
            rtl8139_rx_restart();
            return;
        }

        uint16_t frame_len = (uint16_t)(length - 4);   /* drop the CRC */
        stats.rx_packets++;
        stats.rx_bytes += frame_len;
        if (rx_callback) rx_callback(hdr + 4, frame_len);

        /* Advance to the next packet (header + data, dword aligned) and tell
         * the chip how far we have read. CAPR is "read pointer - 16". */
        rx_offset = (rx_offset + length + 4 + 3) & ~3u;
        rx_offset %= RTL_RX_RING_SIZE;
        outw_io(RTL_REG_CAPR, (uint16_t)(rx_offset - 16));
    }
}

/* IRQ line the NIC reported over PCI (0xFF until rtl8139_probe_init succeeds). */
uint8_t rtl8139_get_irq(void) {
    return irq_line;
}

/* This should be called by your IRQ stub for the NIC (which you assign to PCI IRQ) */
void rtl8139_handle_irq(void) {
    uint16_t isr = inw_io(RTL_REG_ISR);
    outw_io(RTL_REG_ISR, isr);          /* write back to clear */

    if (isr & (RL_ISR_ROK | RL_ISR_RER | RL_ISR_RXOVW)) {
        rtl8139_handle_rx();
    }
    if (isr & RL_ISR_TER) {
        stats.tx_errors++;
    }
}

int rtl8139_is_up(void) {
    return nic_ready;
}

void rtl8139_get_mac(uint8_t mac[6]) {
    for (int i = 0; i < 6; i++) mac[i] = mac_addr[i];
}

void rtl8139_set_rx_handler(rtl8139_rx_cb_t cb) {
    rx_callback = cb;
}

void rtl8139_get_stats(rtl8139_stats_t *out) {
    *out = stats;
}

int rtl8139_send(const void *frame, uint16_t len) {
    if (!nic_ready || !frame || len == 0 || len > RTL8139_MAX_FRAME) return -1;

    uint64_t flags = irq_save();
    uint8_t i = tx_cur;

    /* If this descriptor was used before, wait until the chip is done with it. */
    if (tx_used & (1u << i)) {
        uint32_t st = 0;
        uint32_t spin = 1000000;
        while (spin--) {
            st = inl_io(RTL_REG_TSD0 + 4 * i);
            if (st & (RL_TSD_OWN | RL_TSD_TOK)) break;
        }
        if (!(st & (RL_TSD_OWN | RL_TSD_TOK))) {
            stats.tx_errors++;
            irq_restore(flags);
            return -1;
        }
        if (st & (RL_TSD_TABT | RL_TSD_OWC | RL_TSD_TUN)) stats.tx_errors++;
    }

    uint16_t send_len = len < ETH_MIN_FRAME ? ETH_MIN_FRAME : len;
    memcpy(tx_buf_virt[i], frame, len);
    if (send_len > len) memset(tx_buf_virt[i] + len, 0, send_len - len);

    outl_io(RTL_REG_TSAD0 + 4 * i, tx_buf_phys[i]);
    outl_io(RTL_REG_TSD0 + 4 * i, send_len);   /* size in bits 0-12, OWN=0 starts the DMA */

    tx_used |= (uint8_t)(1u << i);
    tx_cur = (uint8_t)((i + 1) % RTL_TX_DESC_COUNT);
    stats.tx_packets++;
    stats.tx_bytes += send_len;

    irq_restore(flags);
    return 0;
}
