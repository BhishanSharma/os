// i2c_hid.c - I2C touchpad: Intel LPSS I2C controllers + HID over I2C
//
// The I2C controller is a Synopsys DesignWare core: commands go into a FIFO
// (IC_DATA_CMD: a byte to write, or "read one byte", with optional RESTART
// and STOP), read bytes come out of another. HID over I2C (Microsoft's
// spec, version 1.0) then works through registers on the device: a HID
// descriptor saying where everything is, a report descriptor describing
// the report layout, a command register (power, reset), and the input
// register, read with a plain read, whose first two bytes are the length.
#include "drivers/touchpad.h"
#include "drivers/pci.h"
#include "drivers/paging.h"
#include "drivers/heap.h"
#include "drivers/timer.h"
#include "drivers/mouse.h"
#include "drivers/acpi.h"
#include "lib/print.h"
#include "lib/string.h"
#include <stdint.h>

#define PAGE_PCD 0x10
#define PAGE_PWT 0x08

/* ---- DesignWare I2C registers -------------------------------------------- */

#define IC_CON           0x00
#define IC_TAR           0x04
#define IC_DATA_CMD      0x10
#define IC_SS_SCL_HCNT   0x14
#define IC_SS_SCL_LCNT   0x18
#define IC_INTR_MASK     0x30
#define IC_RAW_INTR_STAT 0x34
#define IC_RX_TL         0x38
#define IC_TX_TL         0x3C
#define IC_CLR_INTR      0x40
#define IC_CLR_TX_ABRT   0x54
#define IC_CLR_STOP_DET  0x60
#define IC_ENABLE        0x6C
#define IC_STATUS        0x70
#define IC_SDA_HOLD      0x7C
#define IC_TX_ABRT_SOURCE 0x80
#define IC_ENABLE_STATUS 0x9C
#define IC_COMP_PARAM_1  0xF4
#define IC_COMP_TYPE     0xFC
#define DW_COMP_TYPE     0x44570140u      /* "DW" */

#define CMD_READ    (1u << 8)
#define CMD_STOP    (1u << 9)
#define CMD_RESTART (1u << 10)

#define STATUS_TFNF (1u << 1)            /* transmit FIFO not full */
#define STATUS_RFNE (1u << 3)            /* receive FIFO not empty */
#define INTR_TX_ABRT  (1u << 6)
#define INTR_STOP_DET (1u << 9)

/* Intel LPSS private registers, after the 0x200 bytes of the I2C core */
#define LPSS_RESETS  0x204
#define LPSS_RESETS_OUT 0x7               /* function and DMA out of reset */

/* LPSS I2C controllers by PCI device ID (Intel), from Skylake to Meteor Lake. */
static const uint16_t lpss_i2c_ids[] = {
    0x9D60, 0x9D61, 0x9D62, 0x9D63, 0x9D64, 0x9D65,            /* Sunrise Point-LP (SKL/KBL) */
    0xA160, 0xA161, 0xA162, 0xA163,                            /* Sunrise Point-H */
    0x9DE8, 0x9DE9, 0x9DEA, 0x9DEB, 0x9DC5, 0x9DC6,            /* Cannon Point-LP (WHL) */
    0x02E8, 0x02E9, 0x02EA, 0x02EB, 0x02C5, 0x02C6,            /* Comet Lake-LP */
    0x34E8, 0x34E9, 0x34EA, 0x34EB, 0x34C5, 0x34C6,            /* Ice Lake-LP */
    0xA0E8, 0xA0E9, 0xA0EA, 0xA0EB, 0xA0C5, 0xA0C6,            /* Tiger Lake-LP */
    0x4DE8, 0x4DE9, 0x4DEA, 0x4DEB, 0x4DC5, 0x4DC6,            /* Jasper Lake */
    0x43E8, 0x43E9, 0x43EA, 0x43EB, 0x43AD, 0x43AE,            /* Tiger Lake-H */
    0x51E8, 0x51E9, 0x51EA, 0x51EB, 0x51C5, 0x51C6,            /* Alder Lake-P, Raptor Lake-P */
    0x51D8, 0x51D9,
    0x54E8, 0x54E9, 0x54EA, 0x54EB, 0x54C5, 0x54C6,            /* Alder Lake-N */
    0x7A4C, 0x7A4D, 0x7A4E, 0x7A4F, 0x7A7C, 0x7A7D,            /* Alder Lake-S */
    0x7E50, 0x7E51, 0x7E78, 0x7E79, 0x7E7A, 0x7E7B,            /* Meteor Lake */
    0x31AC, 0x31AE, 0x31B0, 0x31B2, 0x31B4, 0x31B6, 0x31B8, 0x31BA,   /* Gemini Lake */
    0x5AAC, 0x5AAE, 0x5AB0, 0x5AB2, 0x5AB4, 0x5AB6, 0x5AB8, 0x5ABA,   /* Apollo Lake */
};

#define MAX_CTRL 8

typedef struct {
    uint8_t bus, slot, func;
    uint16_t device;
    volatile uint8_t *regs;
    int ok;                              /* the I2C core answered */
    int rx_depth;
    uint8_t found[16];                   /* addresses that answered */
    int found_count;
    int aborts, timeouts;                /* while scanning: no answer / bus stuck */
    uint32_t abort_source;               /* IC_TX_ABRT_SOURCE of the last abort */
    uint32_t fw_resets, fw_clock, fw_con, fw_ss_h, fw_ss_l, fw_fs_h, fw_fs_l;   /* as the firmware left it */
    int clock_forced;                    /* LPSS clock gate turned on by us */
    uint32_t pm_before, pm_after;        /* PCI power state register (bits 0-1: D0-D3) */
    uint32_t comp_type;                  /* what IC_COMP_TYPE read */
    uint64_t base;
    int assigned;                        /* we gave it its address */
} i2c_ctrl_t;

/* I2C devices the firmware's ACPI tables describe. */
static acpi_i2c_device_t acpi_devs[16];
static int acpi_count = -1;

static i2c_ctrl_t ctrls[MAX_CTRL];
static int ctrl_count;

static uint32_t rd(i2c_ctrl_t *c, uint32_t off) { return *(volatile uint32_t *)(c->regs + off); }
static void wr(i2c_ctrl_t *c, uint32_t off, uint32_t v) { *(volatile uint32_t *)(c->regs + off) = v; }

static void delay_ms(uint32_t ms) {
    uint32_t start = get_tick(), ticks = (ms * TIMER_FREQ + 999) / 1000;
    while ((uint32_t)(get_tick() - start) < ticks) __asm__ volatile("hlt");
}

/* ---- Controller ------------------------------------------------------------ */

/* PCI power management: put the device in D0 (the firmware leaves these
 * controllers in D3hot). Leaving D3hot can reset the device, including its
 * command register and BARs, so the caller sets those up afterwards. */
static void pci_power_on(i2c_ctrl_t *c) {
    if (!(pci_config_read_dword(c->bus, c->slot, c->func, 0x04) & (1u << 20))) return;   /* no cap list */
    uint8_t cap = pci_config_read_byte(c->bus, c->slot, c->func, 0x34) & 0xFC;
    for (int guard = 0; cap && guard < 32; guard++) {
        if (pci_config_read_byte(c->bus, c->slot, c->func, cap) == 0x01) {   /* power management */
            uint32_t pmcsr = pci_config_read_dword(c->bus, c->slot, c->func, cap + 4);
            c->pm_before = pmcsr;
            if (pmcsr & 3) {
                pci_config_write_dword(c->bus, c->slot, c->func, cap + 4, pmcsr & ~3u);
                delay_ms(20);
            }
            c->pm_after = pci_config_read_dword(c->bus, c->slot, c->func, cap + 4);
            return;
        }
        cap = pci_config_read_byte(c->bus, c->slot, c->func, cap + 1) & 0xFC;
    }
}

static int i2c_disable(i2c_ctrl_t *c) {
    wr(c, IC_ENABLE, 0);
    for (int i = 0; i < 1000; i++) {
        if (!(rd(c, IC_ENABLE_STATUS) & 1)) return 0;
        for (volatile int s = 0; s < 1000; s++) {}
    }
    return -1;
}

/* ---- Giving a controller an address ---------------------------------------- */

/* Some firmware leaves these controllers without a memory address (BAR 0),
 * as Windows and Linux assign one themselves. Do the same: find what every
 * other device uses, and put the controller in the first free, aligned spot
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

static uint64_t assign_bar(i2c_ctrl_t *c) {
    uint32_t v = pci_config_read_dword(c->bus, c->slot, c->func, 0x10);
    int is64 = ((v >> 1) & 3) == 2;
    uint64_t size = bar_size(c->bus, c->slot, c->func, 0x10, is64);
    if (!size || size > (16u << 20)) return 0;
    if (size < 4096) size = 4096;
    if (!used_count) collect_used();
    uint64_t a = find_free(size);
    if (!a) return 0;
    pci_config_write_dword(c->bus, c->slot, c->func, 0x10, (uint32_t)a | (v & 0xF));
    if (is64) pci_config_write_dword(c->bus, c->slot, c->func, 0x14, 0);
    add_used(a, size);
    c->assigned = 1;
    return a;
}

static int ctrl_start(i2c_ctrl_t *c) {
    /* Wake it first, then put back the BAR (if the wake-up cleared it) and
     * turn memory decoding on. */
    uint32_t bar0 = pci_config_read_dword(c->bus, c->slot, c->func, 0x10);
    uint32_t bar1 = pci_config_read_dword(c->bus, c->slot, c->func, 0x14);
    pci_power_on(c);
    if ((pci_config_read_dword(c->bus, c->slot, c->func, 0x10) & ~0xFu) != (bar0 & ~0xFu)) {
        pci_config_write_dword(c->bus, c->slot, c->func, 0x10, bar0);
        pci_config_write_dword(c->bus, c->slot, c->func, 0x14, bar1);
    }
    uint32_t cmd = pci_config_read_dword(c->bus, c->slot, c->func, 0x04);
    pci_config_write_dword(c->bus, c->slot, c->func, 0x04, (cmd & 0xFFFF) | 0x2);   /* memory space */
    uint64_t base = bar0 & ~0xFu;
    if (((bar0 >> 1) & 3) == 2) base |= (uint64_t)bar1 << 32;
    if (!base) base = assign_bar(c);
    c->base = base;
    if (!base) return -1;
    map_page(base, base, PAGE_PRESENT | PAGE_RW | PAGE_PCD | PAGE_PWT);
    c->regs = (volatile uint8_t *)base;

    c->fw_resets = rd(c, LPSS_RESETS);
    c->fw_clock = rd(c, 0x200);
    c->fw_con = rd(c, IC_CON);
    c->fw_ss_h = rd(c, IC_SS_SCL_HCNT);
    c->fw_ss_l = rd(c, IC_SS_SCL_LCNT);
    c->fw_fs_h = rd(c, 0x1C);
    c->fw_fs_l = rd(c, 0x20);
    if (rd(c, LPSS_RESETS) != LPSS_RESETS_OUT) {
        wr(c, LPSS_RESETS, 0);
        delay_ms(1);
        wr(c, LPSS_RESETS, LPSS_RESETS_OUT);
        delay_ms(5);
    }
    c->comp_type = rd(c, IC_COMP_TYPE);
    if (c->comp_type != DW_COMP_TYPE) return -1;

    i2c_disable(c);
    /* Master, standard mode (100 kHz), restarts allowed, slave off. The
     * input clock is 100-216 MHz depending on the chipset: these counts give
     * at most 100 kHz (slower on slower clocks, which I2C allows). */
    wr(c, IC_CON, 0x01 | (1u << 1) | (1u << 5) | (1u << 6));
    wr(c, IC_SS_SCL_HCNT, 1000);
    wr(c, IC_SS_SCL_LCNT, 1160);
    wr(c, IC_SDA_HOLD, 64);
    wr(c, IC_INTR_MASK, 0);
    wr(c, IC_RX_TL, 0);
    wr(c, IC_TX_TL, 0);
    c->rx_depth = (int)((rd(c, IC_COMP_PARAM_1) >> 8) & 0xFF) + 1;
    if (c->rx_depth < 2 || c->rx_depth > 256) c->rx_depth = 16;
    return 0;
}

/* One transfer: write `wlen` bytes, then (after a repeated start) read
 * `rlen`. Returns 0, -1 if it was aborted (no answer: the reason goes in
 * c->abort_source), or -2 if the bus did not move (timeout). */
static int i2c_xfer(i2c_ctrl_t *c, uint8_t addr, const uint8_t *w, int wlen, uint8_t *r, int rlen) {
    if (i2c_disable(c) != 0) return -2;
    wr(c, IC_TAR, addr);
    (void)rd(c, IC_CLR_INTR);
    wr(c, IC_ENABLE, 1);

    int total = wlen + rlen, pushed = 0, got = 0;
    uint32_t start = get_tick();
    uint32_t limit = 3 + (uint32_t)total * TIMER_FREQ / 1000;     /* ~1 ms a byte, plus 20-30 ms */
    while (got < rlen || pushed < total) {
        if (rd(c, IC_RAW_INTR_STAT) & INTR_TX_ABRT) {
            c->abort_source = rd(c, IC_TX_ABRT_SOURCE);
            (void)rd(c, IC_CLR_TX_ABRT);
            i2c_disable(c);
            return -1;
        }
        while (pushed < total && (rd(c, IC_STATUS) & STATUS_TFNF) &&
               (pushed < wlen || pushed - wlen - got < c->rx_depth - 1)) {
            uint32_t cmd = pushed < wlen ? w[pushed] : CMD_READ;
            if (pushed == wlen && wlen > 0 && rlen > 0) cmd |= CMD_RESTART;
            if (pushed == total - 1) cmd |= CMD_STOP;
            wr(c, IC_DATA_CMD, cmd);
            pushed++;
        }
        while (got < rlen && (rd(c, IC_STATUS) & STATUS_RFNE))
            r[got++] = (uint8_t)rd(c, IC_DATA_CMD);
        if ((uint32_t)(get_tick() - start) > limit) {
            i2c_disable(c);
            return -2;
        }
    }
    /* Wait for the stop condition (and a late abort: a NACK on the last write). */
    while (!(rd(c, IC_RAW_INTR_STAT) & (INTR_STOP_DET | INTR_TX_ABRT)) &&
           (uint32_t)(get_tick() - start) <= limit) {}
    int aborted = (rd(c, IC_RAW_INTR_STAT) & INTR_TX_ABRT) != 0;
    if (aborted) c->abort_source = rd(c, IC_TX_ABRT_SOURCE);
    (void)rd(c, IC_CLR_TX_ABRT);
    (void)rd(c, IC_CLR_STOP_DET);
    return aborted ? -1 : 0;
}

/* A plain read whose first two bytes give its length (the HID input
 * register): read those, then exactly the rest, then stop. Returns the
 * bytes read, or -1. */
static int i2c_read_sized(i2c_ctrl_t *c, uint8_t addr, uint8_t *r, int max) {
    if (i2c_disable(c) != 0) return -1;
    wr(c, IC_TAR, addr);
    (void)rd(c, IC_CLR_INTR);
    wr(c, IC_ENABLE, 1);
    wr(c, IC_DATA_CMD, CMD_READ);
    wr(c, IC_DATA_CMD, CMD_READ);
    int total = 2, pushed = 2, got = 0;
    uint32_t start = get_tick();
    while (got < total) {
        if (rd(c, IC_RAW_INTR_STAT) & INTR_TX_ABRT) {
            (void)rd(c, IC_CLR_TX_ABRT);
            i2c_disable(c);
            return -1;
        }
        while (got < total && (rd(c, IC_STATUS) & STATUS_RFNE)) {
            r[got++] = (uint8_t)rd(c, IC_DATA_CMD);
            if (got == 2) {                                 /* now we know the length */
                int n = r[0] | r[1] << 8;
                total = n < 3 ? 3 : n > max ? max : n;      /* at least one more byte, to send the stop */
            }
        }
        if (got >= 2)
            while (pushed < total && (rd(c, IC_STATUS) & STATUS_TFNF) && pushed - got < c->rx_depth - 1) {
                wr(c, IC_DATA_CMD, CMD_READ | (pushed == total - 1 ? CMD_STOP : 0));
                pushed++;
            }
        if ((uint32_t)(get_tick() - start) > TIMER_FREQ / 5) {
            i2c_disable(c);
            return -1;
        }
    }
    while (!(rd(c, IC_RAW_INTR_STAT) & INTR_STOP_DET) && (uint32_t)(get_tick() - start) <= TIMER_FREQ / 5) {}
    (void)rd(c, IC_CLR_STOP_DET);
    return got;
}

/* ---- HID over I2C ---------------------------------------------------------- */

typedef struct {
    uint16_t desc_len, version, report_desc_len, report_desc_reg, input_reg, max_input;
    uint16_t output_reg, max_output, command_reg, data_reg, vendor, product, product_version;
} __attribute__((packed)) hid_desc_t;

/* Where the report fields are (bit offsets after the report ID): the mouse
 * collection (relative movement), and the Precision Touchpad collection
 * (absolute finger positions; the first finger's fields are used). */
typedef struct {
    int report_id;                       /* -1: reports have no ID byte */
    int buttons_off, buttons;
    int x_off, x_size, y_off, y_size, w_off, w_size;
    int relative;
    int tp_id;                           /* touchpad report ID, -1: none */
    int tp_tip_off, tp_x_off, tp_x_size, tp_y_off, tp_y_size, tp_cc_off, tp_cc_size, tp_btn_off;
    uint32_t tp_x_max, tp_y_max;
    int collections;                     /* application collections seen */
    uint32_t apps[6];                    /* their usages (page << 16 | usage) */
    int app_ids[6];
} layout_t;

static i2c_ctrl_t *tp_ctrl;
static uint8_t tp_addr;
static uint16_t tp_desc_reg;
static hid_desc_t tp_desc;
static layout_t tp_layout;
static uint8_t *tp_report_desc;
static uint8_t tp_buf[256], tp_last[256];
static int tp_last_len;
static int tp_ready;
static uint32_t tp_last_tick;
static char tp_text[96] = "not set up";

static int read_reg(uint16_t reg, uint8_t *buf, int len) {
    uint8_t w[2] = { (uint8_t)reg, (uint8_t)(reg >> 8) };
    return i2c_xfer(tp_ctrl, tp_addr, w, 2, buf, len);
}

static int hid_command(uint8_t opcode, uint8_t arg) {
    uint8_t w[4] = { (uint8_t)tp_desc.command_reg, (uint8_t)(tp_desc.command_reg >> 8), arg, opcode };
    return i2c_xfer(tp_ctrl, tp_addr, w, 4, 0, 0);
}

static int try_hid_descriptor(i2c_ctrl_t *c, uint8_t addr, uint16_t reg, hid_desc_t *d) {
    uint8_t w[2] = { (uint8_t)reg, (uint8_t)(reg >> 8) };
    if (i2c_xfer(c, addr, w, 2, (uint8_t *)d, sizeof(*d)) != 0) return -1;
    return d->desc_len == 30 && d->version == 0x0100 && d->report_desc_len && d->max_input >= 3 ? 0 : -1;
}

/* Walk the report descriptor for the mouse collection's input fields. */
static void parse_report_desc(const uint8_t *p, int len, layout_t *L) {
    memset(L, 0, sizeof(*L));
    L->report_id = -1;
    L->buttons_off = L->x_off = L->y_off = L->w_off = -1;
    L->tp_id = L->tp_tip_off = L->tp_x_off = L->tp_y_off = L->tp_cc_off = L->tp_btn_off = -1;
    static int offsets[256];
    memset(offsets, 0, sizeof(offsets));
    uint32_t page = 0, rsize = 0, rcount = 0, rid = 0, any_id = 0, lmax = 0;
    uint32_t usages[16], umin = 0, umax = 0;
    int nusages = 0, have_range = 0;
    int depth = 0, mouse_depth = -1, pad_depth = -1;
    uint32_t stack_page[8], stack_rsize[8], stack_rcount[8], stack_rid[8];
    int sp = 0;

    for (int i = 0; i < len;) {
        uint8_t b = p[i];
        if (b == 0xFE) { i += 3 + (i + 1 < len ? p[i + 1] : 0); continue; }   /* long item */
        int size = (b & 3) == 3 ? 4 : (b & 3);
        int type = (b >> 2) & 3, tag = b >> 4;
        uint32_t v = 0;
        for (int k = 0; k < size && i + 1 + k < len; k++) v |= (uint32_t)p[i + 1 + k] << (8 * k);
        i += 1 + size;

        if (type == 1) {                                     /* global */
            switch (tag) {
            case 0: page = v; break;
            case 2: lmax = v; break;
            case 7: rsize = v; break;
            case 8: rid = v & 0xFF; any_id = 1; break;
            case 9: rcount = v; break;
            case 10: if (sp < 8) { stack_page[sp] = page; stack_rsize[sp] = rsize; stack_rcount[sp] = rcount; stack_rid[sp] = rid; sp++; } break;
            case 11: if (sp > 0) { sp--; page = stack_page[sp]; rsize = stack_rsize[sp]; rcount = stack_rcount[sp]; rid = stack_rid[sp]; } break;
            }
        } else if (type == 2) {                              /* local */
            uint32_t full = size == 4 ? v : (page << 16 | v);
            if (tag == 0 && nusages < 16) usages[nusages++] = full;
            if (tag == 1) { umin = full; have_range = 1; }
            if (tag == 2) umax = full;
        } else if (type == 0) {                              /* main */
            if (tag == 10) {                                 /* collection */
                if (v == 1 && depth == 0 && L->collections < 6) {
                    uint32_t u = nusages ? usages[0] : 0;
                    L->apps[L->collections] = u;
                    L->app_ids[L->collections++] = -1;
                    if (u == 0x00010002 && mouse_depth < 0) mouse_depth = depth;
                    if (u == 0x000D0005 && pad_depth < 0) pad_depth = depth;
                }
                depth++;
            } else if (tag == 12) {                          /* end collection */
                if (depth > 0) depth--;
                if (depth == mouse_depth) mouse_depth = -2;     /* done with the mouse */
                if (depth == pad_depth) pad_depth = -2;
            } else if (tag == 8) {                           /* input */
                int in_mouse = mouse_depth >= 0 && depth > mouse_depth;
                int in_pad = pad_depth >= 0 && depth > pad_depth;
                if (L->collections && L->app_ids[L->collections - 1] < 0 && any_id)
                    L->app_ids[L->collections - 1] = (int)rid;
                for (uint32_t f = 0; f < rcount; f++) {
                    uint32_t u = have_range ? umin + f : nusages ? usages[f < (uint32_t)nusages ? f : (uint32_t)nusages - 1] : 0;
                    if (have_range && umax && u > umax) u = umax;
                    int off = offsets[rid];
                    if (in_mouse && !(v & 1)) {              /* data, not padding */
                        if (L->report_id < 0 && any_id) L->report_id = (int)rid;
                        if ((u >> 16) == 9) {
                            if (L->buttons_off < 0) L->buttons_off = off;
                            L->buttons++;
                        } else if (u == 0x00010030) { L->x_off = off; L->x_size = (int)rsize; L->relative = (v >> 2) & 1; }
                        else if (u == 0x00010031) { L->y_off = off; L->y_size = (int)rsize; }
                        else if (u == 0x00010038) { L->w_off = off; L->w_size = (int)rsize; }
                    }
                    if (in_pad && !(v & 1)) {                /* first finger, contact count, click */
                        if (L->tp_id < 0) L->tp_id = any_id ? (int)rid : 0;
                        if (u == 0x000D0042 && L->tp_tip_off < 0) L->tp_tip_off = off;
                        else if (u == 0x00010030 && L->tp_x_off < 0) { L->tp_x_off = off; L->tp_x_size = (int)rsize; L->tp_x_max = lmax; }
                        else if (u == 0x00010031 && L->tp_y_off < 0) { L->tp_y_off = off; L->tp_y_size = (int)rsize; L->tp_y_max = lmax; }
                        else if (u == 0x000D0054 && L->tp_cc_off < 0) { L->tp_cc_off = off; L->tp_cc_size = (int)rsize; }
                        else if (u == 0x00090001 && L->tp_btn_off < 0) L->tp_btn_off = off;
                    }
                    offsets[rid] = off + (int)rsize;
                }
            }
            if (tag == 8 || tag == 9 || tag == 11 || tag == 10) { nusages = 0; have_range = 0; umin = umax = 0; }
        }
    }
    if (!any_id) L->report_id = -1;
}

static int32_t get_bits(const uint8_t *data, int datalen, int off, int size, int sign) {
    if (off < 0 || size <= 0 || size > 32 || (off + size + 7) / 8 > datalen) return 0;
    uint32_t v = 0;
    for (int b = 0; b < size; b++)
        if (data[(off + b) / 8] >> ((off + b) % 8) & 1) v |= 1u << b;
    if (sign && size < 32 && (v >> (size - 1)) & 1) v |= ~0u << size;
    return (int32_t)v;
}

/* Read the input register: [length lo, length hi, report...]. Returns the
 * number of bytes, 0 when nothing is waiting, -1 on a bus error. */
static int read_input(uint8_t *buf, int max) {
    int len = tp_desc.max_input;
    if (len > max) len = max;
    int got = i2c_read_sized(tp_ctrl, tp_addr, buf, len);
    if (got < 0) return -1;
    int n = buf[0] | buf[1] << 8;
    if (n <= 2) return 0;
    return n > got ? got : n;
}

/* Precision Touchpad reports: absolute finger positions. Turned into
 * pointer movement (one finger), scrolling (two fingers moving), a click
 * (the pad pressed down; with two fingers on it, a right click), and tap
 * to click (a short touch that hardly moved; two fingers: right click). */
static struct {
    int touching, last_x, last_y, fingers, moved, clicked;
    uint32_t since;
    int rem_x, rem_y, scroll;
    int held;                                               /* buttons the pad press holds */
    uint32_t last_report;
} pad;

static int pad_scale(int delta, int *rem) {
    /* The width of the pad moves the pointer 1600 "mouse pixels" (two
     * screen widths of 100 columns), keeping the remainder for precision. */
    int range = (int)(tp_layout.tp_x_max ? tp_layout.tp_x_max : 4096);
    int v = delta * 1600 + *rem;
    *rem = v % range;
    return v / range;
}

static void handle_touchpad(const uint8_t *data, int datalen) {
    const layout_t *L = &tp_layout;
    int tip = L->tp_tip_off >= 0 ? get_bits(data, datalen, L->tp_tip_off, 1, 0) : 1;
    int x = get_bits(data, datalen, L->tp_x_off, L->tp_x_size, 0);
    int y = get_bits(data, datalen, L->tp_y_off, L->tp_y_size, 0);
    int fingers = L->tp_cc_off >= 0 ? get_bits(data, datalen, L->tp_cc_off, L->tp_cc_size, 0) : tip;
    int press = L->tp_btn_off >= 0 ? get_bits(data, datalen, L->tp_btn_off, 1, 0) : 0;
    int dx = 0, dy = 0, wheel = 0;
    pad.last_report = get_tick();

    if (tip) {
        if (!pad.touching) {
            pad.touching = 1;
            pad.since = get_tick();
            pad.moved = pad.clicked = pad.scroll = 0;
            pad.fingers = fingers;
            pad.rem_x = pad.rem_y = 0;
        } else {
            int mx = x - pad.last_x, my = y - pad.last_y;
            pad.moved += (mx < 0 ? -mx : mx) + (my < 0 ? -my : my);
            if (fingers >= 2) {                             /* two fingers: scroll */
                pad.scroll += my;
                int step = (int)(tp_layout.tp_y_max ? tp_layout.tp_y_max : 4096) / 25;
                while (pad.scroll >= step) { wheel--; pad.scroll -= step; }    /* fingers down: scroll down */
                while (pad.scroll <= -step) { wheel++; pad.scroll += step; }
            } else {
                dx = pad_scale(mx, &pad.rem_x);
                dy = pad_scale(my, &pad.rem_y);
            }
        }
        if (fingers > pad.fingers) pad.fingers = fingers;
        pad.last_x = x;
        pad.last_y = y;
    }

    /* The pad pressed down: left click, or right click with two fingers. */
    if (press && !pad.held) pad.held = fingers >= 2 ? MOUSE_RIGHT : MOUSE_LEFT;
    if (!press) pad.held = 0;
    if (press) pad.clicked = 1;
    mouse_report("I2C touchpad", pad.held, dx, dy, wheel);

    if (!tip && pad.touching) {                             /* lifted: was it a tap? */
        pad.touching = 0;
        int range = (int)(tp_layout.tp_x_max ? tp_layout.tp_x_max : 4096);
        if (!pad.clicked && (uint32_t)(get_tick() - pad.since) < TIMER_FREQ / 4 && pad.moved < range / 40) {
            int b = pad.fingers >= 2 ? MOUSE_RIGHT : MOUSE_LEFT;
            mouse_report("I2C touchpad", b, 0, 0, 0);
            mouse_report("I2C touchpad", 0, 0, 0, 0);
        }
    }
}

static void handle_report(const uint8_t *buf, int n) {
    const layout_t *L = &tp_layout;
    const uint8_t *data = buf + 2;
    int datalen = n - 2;
    if (L->tp_id > 0 && datalen >= 1 && data[0] == L->tp_id && L->tp_x_off >= 0) {
        handle_touchpad(data + 1, datalen - 1);
        return;
    }
    if (L->report_id >= 0) {
        if (datalen < 1 || data[0] != L->report_id) return;
        data++;
        datalen--;
    }
    int buttons = 0;
    for (int i = 0; i < L->buttons && i < 3; i++)
        if (get_bits(data, datalen, L->buttons_off + i, 1, 0)) buttons |= 1 << i;
    int dx = get_bits(data, datalen, L->x_off, L->x_size, 1);
    int dy = get_bits(data, datalen, L->y_off, L->y_size, 1);
    int wheel = get_bits(data, datalen, L->w_off, L->w_size, 1);
    mouse_report("I2C touchpad", buttons, dx, dy, wheel);
}

void touchpad_poll(void) {
    if (!tp_ready || get_tick() == tp_last_tick) return;    /* at most once per tick */
    tp_last_tick = get_tick();
    for (int i = 0; i < 4; i++) {                           /* drain a few */
        int n = read_input(tp_buf, sizeof(tp_buf));
        if (n == 0 && pad.touching && (uint32_t)(get_tick() - pad.last_report) > TIMER_FREQ / 10) {
            /* Quiet for 100 ms while "touching": the finger was lifted without
             * a last report saying so (or it is resting): end the touch. */
            static uint8_t lifted[64];
            handle_touchpad(lifted, (int)sizeof(lifted));   /* all zero: no tip */
            return;
        }
        if (n <= 0) return;
        /* Without the interrupt line, some touchpads answer every read with
         * their last report: only a changed report is new. */
        if (n == tp_last_len && !memcmp(tp_buf, tp_last, (size_t)n)) return;
        memcpy(tp_last, tp_buf, (size_t)n);
        tp_last_len = n;
        handle_report(tp_buf, n);
    }
}

/* ---- Finding it ------------------------------------------------------------ */

static const uint16_t desc_regs[] = { 0x0001, 0x0020, 0x0000, 0x0010, 0x0030, 0x0002 };

/* Which addresses answer a one-byte read (like i2cdetect -r). */
static void scan_bus(i2c_ctrl_t *c) {
    c->found_count = c->aborts = c->timeouts = 0;
    uint8_t dummy;
    for (int a = 0x08; a <= 0x77; a++) {
        int r = i2c_xfer(c, (uint8_t)a, 0, 0, &dummy, 1);
        if (r == 0 && c->found_count < 16) c->found[c->found_count++] = (uint8_t)a;
        else if (r == -1) c->aborts++;
        else if (r == -2) c->timeouts++;
        if (c->timeouts >= 4 && !c->aborts && !c->found_count) break;   /* the bus is not moving */
    }
}

/* The controller ACPI calls "...I2Cn": on Intel chipsets I2C0-3 are PCI
 * device 0x15 functions 0-3, I2C4-5 device 0x19 functions 0-1, I2C6-7
 * device 0x10 functions 0-1. */
static i2c_ctrl_t *ctrl_for_path(const char *path) {
    int len = (int)strlen(path);
    if (len < 4 || path[len - 4] != 'I' || path[len - 3] != '2' || path[len - 2] != 'C') return 0;
    int n = path[len - 1] - '0';
    int slot = n < 4 ? 0x15 : n < 6 ? 0x19 : 0x10, func = n < 4 ? n : n < 6 ? n - 4 : n - 6;
    for (int i = 0; i < ctrl_count; i++)
        if (ctrls[i].slot == slot && ctrls[i].func == func) return &ctrls[i];
    return 0;
}

static void find_controllers(void) {
    if (ctrl_count) return;
    pci_device_t list[64];
    int n = pci_scan(list, 64);
    for (int i = 0; i < n && ctrl_count < MAX_CTRL; i++) {
        if (list[i].vendor != 0x8086) continue;
        int known = 0;
        for (unsigned k = 0; k < sizeof(lpss_i2c_ids) / sizeof(lpss_i2c_ids[0]); k++)
            if (lpss_i2c_ids[k] == list[i].device) known = 1;
        if (!known) continue;
        i2c_ctrl_t *c = &ctrls[ctrl_count++];
        c->bus = list[i].bus;
        c->slot = list[i].slot;
        c->func = list[i].func;
        c->device = list[i].device;
        c->ok = ctrl_start(c) == 0;
        if (!c->ok) continue;
        scan_bus(c);
        if (!c->aborts && c->timeouts && !(rd(c, 0x200) & 1)) {
            /* Nothing moved at all: the LPSS clock gate may be off. Turn it on
             * (bit 0) with an update (bit 31) and try again. */
            wr(c, 0x200, rd(c, 0x200) | 1u | (1u << 31));
            c->clock_forced = 1;
            delay_ms(1);
            scan_bus(c);
        }
    }
}

static const char *vendor_name(uint16_t v) {
    switch (v) {
    case 0x04F3: return "ELAN";
    case 0x06CB: return "Synaptics";
    case 0x044E: return "Alps";
    case 0x2808: return "FocalTech";
    case 0x0488: return "Cirque";
    case 0x093A: return "PixArt";
    case 0x27C6: return "Goodix";
    case 0x1A86: return "WCH";
    case 0x36B6: return "Sensel";
    default: return "HID";
    }
}

static int try_device(i2c_ctrl_t *c, uint8_t addr) {
    for (unsigned k = 0; k < sizeof(desc_regs) / sizeof(desc_regs[0]); k++)
        if (try_hid_descriptor(c, addr, desc_regs[k], &tp_desc) == 0) {
            tp_ctrl = c;
            tp_addr = addr;
            tp_desc_reg = desc_regs[k];
            return 0;
        }
    return -1;
}

int touchpad_init(void) {
    if (acpi_count < 0) acpi_count = acpi_find_i2c_devices(acpi_devs, 16);
    find_controllers();
    /* First where the firmware says the I2C devices are (whether or not
     * the address answered the scan), then every address that answered. */
    for (int i = 0; i < acpi_count && !tp_ctrl; i++) {
        i2c_ctrl_t *c = ctrl_for_path(acpi_devs[i].controller);
        if (c && c->ok && acpi_devs[i].address < 0x80) try_device(c, (uint8_t)acpi_devs[i].address);
    }
    for (int i = 0; i < ctrl_count && !tp_ctrl; i++) {
        i2c_ctrl_t *c = &ctrls[i];
        for (int j = 0; j < c->found_count && !tp_ctrl; j++)
            try_device(c, c->found[j]);
    }
    if (!tp_ctrl) {
        int answered = 0;
        for (int i = 0; i < ctrl_count; i++) answered += ctrls[i].found_count;
        k_snprintf(tp_text, sizeof(tp_text), ctrl_count ? "no HID device on %d I2C controller%s (%d address%s answered)"
                                                         : "no Intel I2C controller",
                   ctrl_count, ctrl_count == 1 ? "" : "s", answered, answered == 1 ? "" : "es");
        return -1;
    }

    tp_report_desc = kmalloc(tp_desc.report_desc_len);
    if (!tp_report_desc || read_reg(tp_desc.report_desc_reg, tp_report_desc, tp_desc.report_desc_len) != 0) {
        k_snprintf(tp_text, sizeof(tp_text), "%s %04x:%04x: report descriptor unreadable",
                   vendor_name(tp_desc.vendor), tp_desc.vendor, tp_desc.product);
        return -1;
    }
    parse_report_desc(tp_report_desc, tp_desc.report_desc_len, &tp_layout);

    hid_command(0x08, 0x00);                                /* SET_POWER: on */
    delay_ms(2);
    hid_command(0x01, 0x00);                                /* RESET */
    delay_ms(100);
    for (int i = 0; i < 20; i++) {                          /* the reset's empty report, and stale ones */
        if (read_input(tp_buf, sizeof(tp_buf)) <= 0) break;
        delay_ms(5);
    }

    if ((tp_layout.x_off < 0 || tp_layout.y_off < 0 || !tp_layout.relative) && tp_layout.tp_x_off < 0) {
        k_snprintf(tp_text, sizeof(tp_text), "%s %04x:%04x found, but it has no relative mouse report (`touchpad`)",
                   vendor_name(tp_desc.vendor), tp_desc.vendor, tp_desc.product);
        return -1;
    }
    tp_ready = 1;
    k_snprintf(tp_text, sizeof(tp_text), "%s %04x:%04x, I2C address 0x%02x (%s, polled)",
               vendor_name(tp_desc.vendor), tp_desc.vendor, tp_desc.product, tp_addr,
               tp_layout.tp_x_off >= 0 ? "precision touchpad" : "mouse mode");
    return 0;
}

const char *touchpad_description(void) { return tp_text; }

static const char *usage_name(uint32_t u) {
    switch (u) {
    case 0x00010002: return "mouse";
    case 0x00010006: return "keyboard";
    case 0x000D0005: return "touchpad";
    case 0x000D0004: return "touchscreen";
    case 0x000D000E: return "configuration";
    case 0x000C0001: return "consumer";
    default: return "other";
    }
}

void touchpad_diagnose(void) {
    find_controllers();
    kprintf("I2C devices in the ACPI tables: ");
    if (acpi_count < 0) kprintf("no ACPI tables\n");
    else {
        int real = 0;
        for (int i = 0; i < acpi_count; i++) real += acpi_devs[i].address != 0;
        kprintf("%d", real);
        if (acpi_count > real)
            kprintf(" (and %d templates whose address the firmware fills in at run time)", acpi_count - real);
        kprintf("\n");
        for (int i = 0; i < acpi_count; i++)
            if (acpi_devs[i].address)
                kprintf("  %-4s  address 0x%02x  %u kHz  on %s\n", acpi_devs[i].device[0] ? acpi_devs[i].device : "?",
                        acpi_devs[i].address, acpi_devs[i].speed / 1000, acpi_devs[i].controller);
    }
    kprintf("Intel I2C controllers: %d\n", ctrl_count);
    for (int i = 0; i < ctrl_count; i++) {
        i2c_ctrl_t *c = &ctrls[i];
        kprintf("  %02x:%02x.%u 8086:%04x  ", c->bus, c->slot, c->func, c->device);
        if (!c->ok) {
            kprintf("not responding: BAR %lx%s, power %x -> %x, reset %x, type %x\n", c->base,
                    c->assigned ? " (assigned by us)" : "", c->pm_before, c->pm_after, c->fw_resets, c->comp_type);
            continue;
        }
        kprintf("%d no-answer, %d timeout%s", c->aborts, c->timeouts, c->clock_forced ? " (clock forced on)" : "");
        if (c->aborts) kprintf(", abort %x", c->abort_source);
        if (c->found_count) {
            kprintf(", answered:");
            for (int j = 0; j < c->found_count; j++) kprintf(" %02x", c->found[j]);
        }
        kprintf("\n    BAR %lx%s, power %x -> %x, firmware left: reset %x clock %x con %x ss %u/%u fs %u/%u\n",
                c->base, c->assigned ? " (assigned by us)" : "", c->pm_before,
                c->pm_after, c->fw_resets, c->fw_clock, c->fw_con, c->fw_ss_h, c->fw_ss_l, c->fw_fs_h, c->fw_fs_l);
    }
    if (!assign_note[0]) {                                  /* dry run: where a 4 KiB BAR would go */
        if (!used_count) collect_used();
        uint64_t a = find_free(4096);
        kprintf("  a free 4 KiB address would be %lx\n", a);
    }
    kprintf("  address assignment: %s\n", assign_note);
    kprintf("Touchpad: %s\n", tp_text);
    if (!tp_ctrl) return;
    kprintf("  HID descriptor at register 0x%04x: report descriptor %u bytes, input max %u bytes\n",
            tp_desc_reg, tp_desc.report_desc_len, tp_desc.max_input);
    const layout_t *L = &tp_layout;
    kprintf("  collections:");
    for (int i = 0; i < L->collections; i++)
        kprintf(" %s(id %d)", usage_name(L->apps[i]), L->app_ids[i]);
    kprintf("\n  mouse report id %d: %d buttons at bit %d, X %d bits at %d, Y %d bits at %d, wheel %d bits, %s\n",
            L->report_id, L->buttons, L->buttons_off, L->x_size, L->x_off, L->y_size, L->y_off, L->w_size,
            L->relative ? "relative" : "ABSOLUTE");
    kprintf("  touchpad report id %d: tip at bit %d, X %d bits at %d (max %u), Y %d bits at %d (max %u),"
            " fingers at %d, click at %d\n", L->tp_id, L->tp_tip_off, L->tp_x_size, L->tp_x_off, L->tp_x_max,
            L->tp_y_size, L->tp_y_off, L->tp_y_max, L->tp_cc_off, L->tp_btn_off);

    kprintf("Move a finger and click for 8 seconds; reports (changed ones only):\n");
    int was_ready = tp_ready, shown = 0, reads = 0, same = 0, empty = 0;
    tp_ready = 0;                                           /* the idle loop must not read meanwhile */
    uint32_t start = get_tick();
    static uint8_t prev[256];
    int prev_len = 0;
    while ((uint32_t)(get_tick() - start) < 8 * TIMER_FREQ) {
        int n = read_input(tp_buf, sizeof(tp_buf));
        reads++;
        if (n <= 0) { empty++; delay_ms(10); continue; }
        if (n == prev_len && !memcmp(tp_buf, prev, (size_t)n)) { same++; delay_ms(10); continue; }
        memcpy(prev, tp_buf, (size_t)n);
        prev_len = n;
        if (shown < 14) {
            kprintf("  ");
            for (int i = 0; i < n && i < 20; i++) kprintf("%02x ", tp_buf[i]);
            kprintf("\n");
            shown++;
        }
        delay_ms(10);
    }
    kprintf("%d reads: %d empty, %d repeated, %d new\n", reads, empty, same, reads - empty - same);
    tp_ready = was_ready;
}
