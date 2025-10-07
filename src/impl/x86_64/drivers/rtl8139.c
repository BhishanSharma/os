#include "drivers/rtl8139.h"
#include "drivers/pci.h"
#include "../lib/ports.h"
#include "lib/print.h"
#include "drivers/heap.h"   // kmalloc/kfree
#include <stdint.h>
#include "lib/string.h" // if available

/* RTL8139 register offsets (I/O base + offset) */
#define RTL_REG_MAC        0x00
#define RTL_REG_MAR        0x08
#define RTL_REG_TXSTATUS0  0x10
#define RTL_REG_TSAD0      0x20
#define RTL_REG_RBSTART    0x30
#define RTL_REG_CR         0x37
#define RTL_REG_CAPR       0x38
#define RTL_REG_IMR        0x3C
#define RTL_REG_ISR        0x3E
#define RTL_REG_RCR        0x44
#define RTL_REG_CONFIG1    0x52

/* CR bits */
#define RL_CR_RST 0x10
#define RL_CR_RE  0x08
#define RL_CR_TE  0x04

/* IMR/ISR bits */
#define RL_ISR_ROK  (1<<0)
#define RL_ISR_TOK  (1<<2)

/* Receive buffer size recommended: 8192 + 16 */
#define RTL_RX_BUF_SIZE (8192 + 16)

static uint16_t io_base = 0;
static uint8_t irq_line = 0xFF;
static uint8_t *rx_buf_virt = 0;
static uint32_t rx_buf_phys = 0;
static uint32_t rx_offset = 0;

static inline void outb_io(uint16_t reg, uint8_t val) { outb(io_base + reg, val); }
static inline uint8_t inb_io(uint16_t reg) { return inb(io_base + reg); }
static inline void outw_io(uint16_t reg, uint16_t val) { outw(io_base + reg, val); }
static inline uint16_t inw_io(uint16_t reg) { return inw(io_base + reg); }
static inline void outl_io(uint16_t reg, uint32_t val) { outl(io_base + reg, val); }
static inline uint32_t inl_io(uint16_t reg) { return inl(io_base + reg); }

/* Simple memcopy if not available */
static void simple_memcpy(void *dst, const void *src, size_t n) {
    uint8_t *d = dst;
    const uint8_t *s = src;
    while (n--) *d++ = *s++;
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

/* Convert virtual to physical if you need; here assume identity mapping */
static uint32_t virt_to_phys(void* v) {
    return (uint32_t)(uintptr_t)v;
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

    /* Soft reset */
    outb_io(RTL_REG_CR, RL_CR_RST);
    /* wait until RST bit cleared */
    while (inb_io(RTL_REG_CR) & RL_CR_RST) { /* spin */ }

    /* Put config1 0x00 to power on */
    outb_io(RTL_REG_CONFIG1, 0x00);

    /* Allocate RX buffer (8192 + 16 recommended) */
    rx_buf_virt = kmalloc(RTL_RX_BUF_SIZE);
    if (!rx_buf_virt) {
        kprintf("[NET] Failed to allocate RX buffer\n");
        return -1;
    }
    rx_buf_phys = virt_to_phys(rx_buf_virt);
    /* zero it */
    for (uint32_t i = 0; i < RTL_RX_BUF_SIZE; i++) rx_buf_virt[i] = 0;

    /* Set RBSTART to physical address */
    outl_io(RTL_REG_RBSTART, rx_buf_phys);

    /* Set RCR: accept broadcast, multicast, physical match, maybe WRAP bit off (0) */
    /* Bits: AAP(0x20) APM(0x10) AM(0x08) AB(0x04) plus others. OSDev suggests: 0xf | (1<<7) if wrap */
    outl_io(RTL_REG_RCR, 0x0000000F);

    /* Clear ISR */
    outw_io(RTL_REG_ISR, 0xFFFF);

    /* Set IMR to ROK | TOK so we receive interrupts for RX/TX done */
    outw_io(RTL_REG_IMR, RL_ISR_ROK | RL_ISR_TOK);

    /* Enable receiver and transmitter */
    outb_io(RTL_REG_CR, RL_CR_RE | RL_CR_TE);

    rx_offset = 0;

    kprintf("[NET] RTL8139 init complete\n");

    /* Unmask PIC for this IRQ: call your enable_irq() helper with irq_line */
    enable_irq(irq_line);

    /* Register IDT entry: you must add idt_set_entry(0x20 + irq_line, ...) from kernel_main after idt_init */
    // Kernel: idt_set_entry(0x20 + irq_line, irqX_stub, 0x8E);

    return 0;
}

/* read a packet from ring -- called when ROK interrupt occurs */
static void rtl8139_handle_rx(void) {
    uint16_t capr = inw_io(RTL_REG_CAPR); // Current Address of Packet Read (pointer to last processed)
    /* CAPR points to the last read position (offset) + 16? See spec: CAPR = current offset + 16 */
    /* Implementation: parse packets starting at rx_offset and update CAPR when done */
    uint8_t *buf = rx_buf_virt;
    uint32_t read_offset = rx_offset;
    while (1) {
        /* Each packet is preceded by a 4-byte header: status(2), length(2) */
        uint16_t status = *(uint16_t*)(buf + read_offset);
        uint16_t length = *(uint16_t*)(buf + read_offset + 2);

        if ((status & 0x01) == 0) {
            // no more packets ready
            break;
        }
        uint32_t pkt_start = read_offset + 4;
        if (pkt_start + length > RTL_RX_BUF_SIZE) {
            // wrap around handling: copy in two parts
            uint32_t part = RTL_RX_BUF_SIZE - pkt_start;
            // allocate temp or handle in two steps; simple approach: linearize into a small buffer
            uint8_t tmp[1600];
            uint32_t p = 0;
            for (uint32_t i = 0; i < part; i++) tmp[p++] = buf[pkt_start + i];
            for (uint32_t i = 0; i < length - part; i++) tmp[p++] = buf[i];
            // hand off tmp, length
            // for demonstration, print frame length
            kprintf("[NET] RX pkt len=%u\n", length);
            // TODO: parse Ethernet frame (e.g., IPv4/UDP)
        } else {
            // linear case
            uint8_t *pkt = buf + pkt_start;
            kprintf("[NET] RX pkt len=%u\n", length);
            // For demo: print first few bytes hex
            // for (int i=0;i< (length<16?length:16); i++) kprintf("%02x ", pkt[i]);
            // kprintf("\n");
            // TODO: process pkt
        }

        /* Advance read_offset: packet header + rounded length to dword? spec: length field is packet length incl CRC */
        uint32_t adv = ((length + 4 + 3) & ~3); // 4 byte header + data, rounded to dword
        read_offset += adv;
        if (read_offset >= RTL_RX_BUF_SIZE) read_offset -= RTL_RX_BUF_SIZE;

        /* After handling packet, update CAPR so chip can free buffer */
        /* CAPR needs to be set to the last processed address - 16 (chip specific). OSDev suggests writing the current offset */
        outw_io(RTL_REG_CAPR, read_offset - 16);
    }
    rx_offset = read_offset;
}

/* This should be called by your IRQ stub for the NIC (which you assign to PCI IRQ) */
void rtl8139_handle_irq(void) {
    uint16_t isr = inw_io(RTL_REG_ISR);
    /* write back to clear */
    outw_io(RTL_REG_ISR, isr);
    if (isr & RL_ISR_ROK) {
        rtl8139_handle_rx();
    }
    if (isr & RL_ISR_TOK) {
        // handle transmit ok
    }
}
