// ahci.c - SATA disks through AHCI (Serial ATA AHCI 1.3.1; ATA8-ACS commands)
//
//   1. PCI: memory + bus master; BAR 5 (ABAR) holds the registers. Take the
//      controller from the firmware (BIOS/OS handoff), AHCI mode on (GHC.AE).
//   2. Each implemented port with a device on its link (SSTS.DET = 3): stop
//      it, give it a command list, a received-FIS area and one command table
//      (all in one page), start it, and IDENTIFY the disk (model, LBA48 size).
//   3. Reads and writes: READ/WRITE DMA EXT through a 64 KiB bounce buffer
//      (one PRD entry per page), one command at a time per controller.
//      Completions raise MSI where the controller has it (the wait sleeps,
//      the kernel lock free); otherwise the wait looks again at each tick.
//   4. A command that fails or times out restarts the port (and resets the
//      link if the disk stays busy), and is tried once more.
#include "drivers/ahci.h"
#include "drivers/drive.h"
#include "sys/smp.h"
#include "sys/task.h"
#include "drivers/pci.h"
#include "drivers/paging.h"
#include "drivers/heap.h"
#include "drivers/timer.h"
#include "drivers/msi.h"
#include "lib/print.h"
#include "lib/string.h"
#include <stdint.h>

#define CAP      0x00
#define GHC      0x04
#define IS       0x08
#define PI       0x0C
#define VS       0x10
#define CAP2     0x24
#define BOHC     0x28

#define GHC_AE   (1u << 31)
#define GHC_IE   (1u << 1)
#define CAP_S64A (1u << 31)
#define CAP_SSS  (1u << 27)

#define P_CLB    0x00
#define P_CLBU   0x04
#define P_FB     0x08
#define P_FBU    0x0C
#define P_IS     0x10
#define P_IE     0x14
#define P_CMD    0x18
#define P_TFD    0x20
#define P_SIG    0x24
#define P_SSTS   0x28
#define P_SCTL   0x2C
#define P_SERR   0x30
#define P_CI     0x38

#define CMD_ST   (1u << 0)
#define CMD_SUD  (1u << 1)
#define CMD_POD  (1u << 2)
#define CMD_FRE  (1u << 4)
#define CMD_FR   (1u << 14)
#define CMD_CR   (1u << 15)
#define CMD_ICC_ACTIVE (1u << 28)

#define TFD_ERR  0x01
#define TFD_DRQ  0x08
#define TFD_BSY  0x80

#define PIS_DONE   0x0000000Fu           /* D2H register, PIO setup, DMA setup, set device bits FIS */
#define PIS_ERRORS 0x78000000u           /* task file, host bus fatal, host bus data, interface fatal */

#define SIG_ATA    0x00000101u
#define SIG_ATAPI  0xEB140101u
#define SIG_PM     0x96690101u

#define ATA_IDENTIFY       0xEC
#define ATA_READ_DMA_EXT   0x25
#define ATA_WRITE_DMA_EXT  0x35
#define ATA_FLUSH_EXT      0xEA

#define BOUNCE_PAGES 16                  /* 64 KiB per command: 128 sectors */
#define TABLE_OFF    0x800               /* in the port's page: list 0x000, FIS 0x400, table 0x800 */
#define MAX_HBAS     2
#define MAX_DISKS    8

enum { PORT_NONE, PORT_DISK, PORT_ATAPI, PORT_PM, PORT_SILENT, PORT_FAILED, PORT_OTHER };

typedef struct hba hba_t;

typedef struct {
    hba_t *hba;
    int port;
    volatile uint8_t *regs;              /* the port's registers */
    uint8_t *page;                       /* command list, received FISes, command table 0 */
    uint64_t sectors;
    char model[41], serial[21];
    volatile uint32_t err;               /* error bits the interrupt took off PxIS */
    uint32_t errors;                     /* commands that failed (`sata`) */
} disk_t;

struct hba {
    volatile uint8_t *abar;
    pci_device_t pci;
    uint32_t cap, pi;
    int irq_mode;
    uint8_t *bounce;
    kmutex_t lock;                       /* one command at a time (the waits let go of the kernel lock) */
    uint8_t port_kind[32];
};

static hba_t hbas[MAX_HBAS];
static int nhba;
static disk_t disks[MAX_DISKS];
static int ndisks;
static char raid_note[120];

static uint32_t hr(hba_t *c, uint32_t o) { return *(volatile uint32_t *)(c->abar + o); }
static void hw(hba_t *c, uint32_t o, uint32_t v) { *(volatile uint32_t *)(c->abar + o) = v; }
static uint32_t pr(volatile uint8_t *regs, uint32_t o) { return *(volatile uint32_t *)(regs + o); }
static void pw(volatile uint8_t *regs, uint32_t o, uint32_t v) { *(volatile uint32_t *)(regs + o) = v; }

static int interrupts_on(void) {
    uint64_t f;
    __asm__ volatile("pushfq; pop %0" : "=r"(f));
    return (f & 0x200) != 0;
}

static void *dma_alloc(uint64_t size) {
    uint8_t *raw = kmalloc(size + 4096);
    if (!raw) return 0;
    uint8_t *p = (uint8_t *)(((uint64_t)raw + 4095) & ~4095ull);
    memset(p, 0, size);
    return p;
}

static void delay_ms(uint32_t ms) {
    uint32_t start = timer_ms();
    while (timer_ms() - start < ms) __asm__ volatile("pause");
}

/* Until (reg & mask) == 0, at most `ms`. */
static int wait_clear(volatile uint8_t *regs, uint32_t o, uint32_t mask, uint32_t ms) {
    uint32_t start = timer_ms();
    while (pr(regs, o) & mask) {
        if (timer_ms() - start > ms) return -1;
        __asm__ volatile("pause");
    }
    return 0;
}

static void ahci_interrupt(void) {
    for (int h = 0; h < nhba; h++) {
        hba_t *c = &hbas[h];
        if (!c->irq_mode) continue;
        uint32_t is = hr(c, IS);
        if (!is) continue;
        for (int p = 0; p < 32; p++) {
            if (!(is >> p & 1)) continue;
            volatile uint8_t *regs = c->abar + 0x100 + p * 0x80;
            uint32_t pis = pr(regs, P_IS);
            for (int i = 0; i < ndisks; i++)
                if (disks[i].hba == c && disks[i].port == p) disks[i].err |= pis & PIS_ERRORS;
            pw(regs, P_IS, pis);
        }
        hw(c, IS, is);
    }
}

static int port_stop(volatile uint8_t *regs) {
    uint32_t cmd = pr(regs, P_CMD);
    if (cmd & CMD_ST) pw(regs, P_CMD, cmd & ~CMD_ST);
    if (wait_clear(regs, P_CMD, CMD_CR, 500)) return -1;
    cmd = pr(regs, P_CMD);
    if (cmd & CMD_FRE) pw(regs, P_CMD, cmd & ~CMD_FRE);
    return wait_clear(regs, P_CMD, CMD_FR, 500);
}

/* COMRESET: the link starts over (a disk that stays busy). */
static int link_reset(volatile uint8_t *regs) {
    uint32_t sctl = pr(regs, P_SCTL) & ~0xFu;
    pw(regs, P_SCTL, sctl | 1);
    delay_ms(2);
    pw(regs, P_SCTL, sctl);
    uint32_t start = timer_ms();
    while ((pr(regs, P_SSTS) & 0xF) != 3) {
        if (timer_ms() - start > 1000) return -1;
        __asm__ volatile("pause");
    }
    pw(regs, P_SERR, 0xFFFFFFFF);
    return 0;
}

/* The port receives FISes, and (once the disk is not busy) runs commands. */
static int port_start(volatile uint8_t *regs, uint32_t busy_ms) {
    pw(regs, P_SERR, 0xFFFFFFFF);
    pw(regs, P_IS, 0xFFFFFFFF);
    pw(regs, P_CMD, pr(regs, P_CMD) | CMD_FRE);
    if (wait_clear(regs, P_TFD, TFD_BSY | TFD_DRQ, busy_ms) != 0) {
        if (link_reset(regs) != 0 || wait_clear(regs, P_TFD, TFD_BSY | TFD_DRQ, busy_ms) != 0) return -1;
    }
    pw(regs, P_CMD, pr(regs, P_CMD) | CMD_ST);
    return 0;
}

static void recover(disk_t *d) {
    port_stop(d->regs);
    port_start(d->regs, 1000);
    if (d->hba->irq_mode) pw(d->regs, P_IE, PIS_DONE | PIS_ERRORS);
}

/* One command in slot 0; `bytes` through the bounce buffer. 0 on success. */
static int issue(disk_t *d, uint8_t command, uint64_t lba, uint32_t count, uint32_t bytes, int write, uint32_t ms) {
    hba_t *c = d->hba;
    if (wait_clear(d->regs, P_TFD, TFD_BSY | TFD_DRQ, 1000) != 0) return -1;
    uint8_t *t = d->page + TABLE_OFF;
    memset(t, 0, 0x80 + BOUNCE_PAGES * 16);
    t[0] = 0x27;                                        /* host to device register FIS */
    t[1] = 0x80;                                        /* (a command) */
    t[2] = command;
    t[4] = (uint8_t)lba;
    t[5] = (uint8_t)(lba >> 8);
    t[6] = (uint8_t)(lba >> 16);
    t[7] = 0x40;                                        /* LBA mode */
    t[8] = (uint8_t)(lba >> 24);
    t[9] = (uint8_t)(lba >> 32);
    t[10] = (uint8_t)(lba >> 40);
    t[12] = (uint8_t)count;
    t[13] = (uint8_t)(count >> 8);
    uint32_t prds = 0;
    for (uint32_t off = 0; off < bytes; off += 4096, prds++) {
        uint32_t *e = (uint32_t *)(t + 0x80 + prds * 16);
        uint64_t pa = virt_to_phys(c->bounce + off);
        uint32_t n = bytes - off > 4096 ? 4096 : bytes - off;
        e[0] = (uint32_t)pa;
        e[1] = (uint32_t)(pa >> 32);
        e[2] = 0;
        e[3] = n - 1;
    }
    uint32_t *h = (uint32_t *)d->page;                  /* command header 0 */
    h[0] = 5u | (write ? 1u << 6 : 0) | prds << 16;     /* 5-dword FIS */
    h[1] = 0;
    d->err = 0;
    pw(d->regs, P_IS, 0xFFFFFFFF);
    __asm__ volatile("mfence" ::: "memory");
    pw(d->regs, P_CI, 1);
    uint32_t start = timer_ms();
    for (;;) {
        if ((pr(d->regs, P_IS) | d->err) & PIS_ERRORS) break;
        if (!(pr(d->regs, P_CI) & 1)) {
            if (pr(d->regs, P_TFD) & TFD_ERR) break;
            return 0;
        }
        if (timer_ms() - start > ms) break;
        /* Other cores may use the kernel meanwhile (the controller's lock
         * keeps its commands one at a time). */
        if (interrupts_on()) cpu_wait();                /* MSI (or the next tick) wakes us */
        else __asm__ volatile("pause");
    }
    d->errors++;
    recover(d);
    return -1;
}

static int rw(disk_t *d, int write, uint64_t lba, uint32_t count, uint8_t *buf) {
    hba_t *c = d->hba;
    kmutex_lock(&c->lock);
    int r = 0;
    while (count && r == 0) {
        uint32_t n = count > BOUNCE_PAGES * 8 ? BOUNCE_PAGES * 8 : count;
        uint32_t bytes = n * 512;
        if (write) memcpy(c->bounce, buf, bytes);
        uint8_t cmd = write ? ATA_WRITE_DMA_EXT : ATA_READ_DMA_EXT;
        r = issue(d, cmd, lba, n, bytes, write, 10000);
        if (r != 0) r = issue(d, cmd, lba, n, bytes, write, 10000);   /* once more, after the port restarted */
        if (r == 0 && !write) memcpy(buf, c->bounce, bytes);
        buf += bytes;
        lba += n;
        count -= n;
    }
    kmutex_unlock(&c->lock);
    return r;
}

static int disk_read(void *ctx, uint64_t lba, uint32_t count, void *buf) { return rw(ctx, 0, lba, count, buf); }
static int disk_write(void *ctx, uint64_t lba, uint32_t count, const void *buf) {
    return rw(ctx, 1, lba, count, (uint8_t *)buf);
}

static int disk_flush(void *ctx) {
    disk_t *d = ctx;
    kmutex_lock(&d->hba->lock);
    int r = issue(d, ATA_FLUSH_EXT, 0, 0, 0, 0, 30000);
    kmutex_unlock(&d->hba->lock);
    return r;
}

/* IDENTIFY's strings: big-endian words, padded with spaces. */
static void ata_string(char *out, const uint16_t *words, int nwords) {
    for (int i = 0; i < nwords; i++) {
        out[2 * i] = (char)(words[i] >> 8);
        out[2 * i + 1] = (char)words[i];
    }
    out[2 * nwords] = 0;
    for (int i = 2 * nwords - 1; i >= 0 && (out[i] == ' ' || !out[i]); i--) out[i] = 0;
    char *s = out;
    while (*s == ' ') s++;
    if (s != out) memmove(out, s, strlen(s) + 1);
}

/* Is every page of [p, p + size) below 4 GiB (a controller without 64-bit addresses)? */
static int low_enough(const void *p, uint64_t size) {
    for (uint64_t off = 0; off < size; off += 4096)
        if (virt_to_phys((const uint8_t *)p + off) >> 32) return 0;
    return 1;
}

static void add_port(hba_t *c, int p) {
    volatile uint8_t *regs = c->abar + 0x100 + p * 0x80;
    uint32_t ssts = pr(regs, P_SSTS);
    if ((ssts & 0xF) != 3 && (c->cap & CAP_SSS)) {      /* staggered spin-up: wake the device */
        pw(regs, P_CMD, pr(regs, P_CMD) | CMD_SUD | CMD_POD);
        delay_ms(20);
        ssts = pr(regs, P_SSTS);
    }
    if ((ssts & 0xF) != 3) return;                      /* nothing on the link */
    if (((ssts >> 8) & 0xF) != 1) pw(regs, P_CMD, pr(regs, P_CMD) | CMD_ICC_ACTIVE);   /* out of a power-saving state */
    if (ndisks >= MAX_DISKS) return;
    disk_t *d = &disks[ndisks];
    memset(d, 0, sizeof(*d));
    d->hba = c;
    d->port = p;
    d->regs = regs;
    d->page = dma_alloc(4096);
    if (!d->page || (!(c->cap & CAP_S64A) && !low_enough(d->page, 4096))) {
        c->port_kind[p] = PORT_FAILED;
        return;
    }
    if (port_stop(regs) != 0) {
        c->port_kind[p] = PORT_FAILED;
        return;
    }
    uint64_t pa = virt_to_phys(d->page);
    pw(regs, P_CLB, (uint32_t)pa);
    pw(regs, P_CLBU, (uint32_t)(pa >> 32));
    pw(regs, P_FB, (uint32_t)(pa + 0x400));
    pw(regs, P_FBU, (uint32_t)((pa + 0x400) >> 32));
    uint32_t *h = (uint32_t *)d->page;
    h[2] = (uint32_t)(pa + TABLE_OFF);
    h[3] = (uint32_t)((pa + TABLE_OFF) >> 32);
    if (port_start(regs, 3000) != 0) {                  /* (a hard disk spins up meanwhile) */
        c->port_kind[p] = PORT_SILENT;
        return;
    }
    uint32_t sig = pr(regs, P_SIG);
    if (sig == SIG_ATAPI || sig == SIG_PM || sig != SIG_ATA) {
        c->port_kind[p] = sig == SIG_ATAPI ? PORT_ATAPI : sig == SIG_PM ? PORT_PM : PORT_OTHER;
        port_stop(regs);
        return;
    }
    if (issue(d, ATA_IDENTIFY, 0, 0, 512, 0, 3000) != 0) {
        c->port_kind[p] = PORT_FAILED;
        return;
    }
    const uint16_t *id = (const uint16_t *)c->bounce;
    ata_string(d->model, id + 27, 20);
    ata_string(d->serial, id + 10, 10);
    int lba48 = (id[83] >> 10) & 1;
    d->sectors = lba48 ? (uint64_t)id[100] | (uint64_t)id[101] << 16 | (uint64_t)id[102] << 32 | (uint64_t)id[103] << 48
                       : 0;
    uint32_t logical = 512;
    if ((id[106] & 0xC000) == 0x4000 && (id[106] & (1u << 12))) logical = ((uint32_t)id[117] | (uint32_t)id[118] << 16) * 2;
    if (!lba48 || !d->sectors || logical != 512) {     /* (pre-2003 disks; 4Kn disks) */
        c->port_kind[p] = PORT_OTHER;
        return;
    }
    c->port_kind[p] = PORT_DISK;
    ndisks++;
}

static void add_hba(const pci_device_t *pd) {
    if (nhba >= MAX_HBAS) return;
    hba_t *c = &hbas[nhba];
    memset(c, 0, sizeof(*c));
    c->pci = *pd;
    uint32_t after;
    pci_power_on(pd->bus, pd->slot, pd->func, &after);
    uint32_t bar5 = pci_config_read_dword(pd->bus, pd->slot, pd->func, 0x24);
    uint64_t base = bar5 & ~0xFull;
    if (!base || (bar5 & 1)) return;
    uint32_t cmdreg = pci_config_read_dword(pd->bus, pd->slot, pd->func, 0x04);
    pci_config_write_dword(pd->bus, pd->slot, pd->func, 0x04, (cmdreg & 0xFFFF) | 0x2 | 0x4 | 0x400);
    c->abar = mmio_map(base, 0x1100);
    if (!c->abar) return;
    if (hr(c, CAP2) & 1) {                              /* BIOS/OS handoff */
        hw(c, BOHC, hr(c, BOHC) | 2);
        uint32_t start = timer_ms();
        while ((hr(c, BOHC) & 1) && timer_ms() - start < 50) __asm__ volatile("pause");
        start = timer_ms();
        while ((hr(c, BOHC) & 0x10) && timer_ms() - start < 2000) __asm__ volatile("pause");
    }
    hw(c, GHC, hr(c, GHC) | GHC_AE);
    hw(c, GHC, hr(c, GHC) & ~GHC_IE);                   /* (polled until MSI is on) */
    c->cap = hr(c, CAP);
    c->pi = hr(c, PI);
    c->bounce = dma_alloc(BOUNCE_PAGES * 4096);
    if (!c->bounce || (!(c->cap & CAP_S64A) && !low_enough(c->bounce, BOUNCE_PAGES * 4096))) return;
    nhba++;
    int first = ndisks;
    for (int p = 0; p < 32; p++)
        if (c->pi >> p & 1) add_port(c, p);
    if (ndisks > first && msi_enable(pd->bus, pd->slot, pd->func, ahci_interrupt, "AHCI", 0) == 0) {
        c->irq_mode = 1;
        hw(c, IS, 0xFFFFFFFF);
        for (int i = first; i < ndisks; i++) {
            pw(disks[i].regs, P_IS, 0xFFFFFFFF);
            pw(disks[i].regs, P_IE, PIS_DONE | PIS_ERRORS);
        }
        hw(c, GHC, hr(c, GHC) | GHC_IE);
    }
}

int ahci_init(char *how, int size) {
    static int done;
    static char desc[160];
    if (done) {
        k_snprintf(how, (size_t)size, "%s", desc);
        return ndisks;
    }
    done = 1;
    static pci_device_t devs[64];
    int n = pci_scan(devs, 64);
    for (int i = 0; i < n; i++) {
        const pci_device_t *pd = &devs[i];
        if (pd->class_code == 0x01 && pd->subclass == 0x06 && pd->prog_if == 0x01) add_hba(pd);
        else if (pd->class_code == 0x01 && pd->subclass == 0x04 && pd->vendor == 0x8086)
            k_snprintf(raid_note, sizeof(raid_note), "the SATA controller is in RAID (\"RST\") mode in the firmware "
                                                     "setup: switch it to AHCI to use its disks");
    }
    for (int i = 0; i < ndisks; i++) {
        drive_t dr;
        memset(&dr, 0, sizeof(dr));
        dr.kind = "SATA";
        kstrncpy(dr.model, disks[i].model, sizeof(dr.model));
        dr.sectors = disks[i].sectors;
        dr.read = disk_read;
        dr.write = disk_write;
        dr.flush = disk_flush;
        dr.ctx = &disks[i];
        drive_add(&dr);
    }
    if (!nhba) {
        k_snprintf(desc, sizeof(desc), "%s", raid_note[0] ? raid_note : "no AHCI controller");
    } else if (!ndisks) {
        k_snprintf(desc, sizeof(desc), "AHCI, no disk (`sata` lists the ports)");
    } else {
        k_snprintf(desc, sizeof(desc), "%d disk%s:", ndisks, ndisks == 1 ? "" : "s");
        for (int i = 0; i < ndisks; i++) {
            size_t at = strlen(desc);
            uint64_t mb = disks[i].sectors / 2048;
            k_snprintf(desc + at, sizeof(desc) - at, "%s %s, %u.%u GB", i ? ";" : "",
                       disks[i].model[0] ? disks[i].model : "disk", (uint32_t)(mb / 1000), (uint32_t)(mb % 1000 / 100));
        }
    }
    k_snprintf(how, (size_t)size, "%s", desc);
    return ndisks;
}

void ahci_print_info(void) {
    char how[160];
    ahci_init(how, sizeof(how));
    if (!nhba) {
        kprintf("SATA: %s\n", how);
        return;
    }
    static const char *const kinds[] = { "empty", "disk", "CD/DVD drive (not used)", "port multiplier (not supported)",
                                         "a device that does not answer", "a disk that failed to start",
                                         "a device that is not a disk Terminal OS can use (pre-LBA48, 4 KiB sectors)" };
    for (int h = 0; h < nhba; h++) {
        hba_t *c = &hbas[h];
        uint32_t vs = hr(c, VS);
        kprintf("AHCI %u.%u controller %04x:%04x at %02x:%02x.%u, %u ports, %s, %s\n", vs >> 16, (vs >> 8) & 0xFF,
                c->pci.vendor, c->pci.device, c->pci.bus, c->pci.slot, c->pci.func, (c->cap & 0x1F) + 1,
                c->cap & CAP_S64A ? "64-bit" : "32-bit", c->irq_mode ? "MSI" : "polled");
        for (int p = 0; p < 32; p++) {
            if (!(c->pi >> p & 1)) continue;
            kprintf("  port %d: %s", p, kinds[c->port_kind[p]]);
            for (int i = 0; i < ndisks; i++)
                if (disks[i].hba == c && disks[i].port == p) {
                    uint64_t mb = disks[i].sectors / 2048;
                    kprintf(": %s (serial %s), %u.%u GB", disks[i].model, disks[i].serial, (uint32_t)(mb / 1000),
                            (uint32_t)(mb % 1000 / 100));
                    if (disks[i].errors) kprintf(", %u failed commands", disks[i].errors);
                }
            kprintf("\n");
        }
    }
}
