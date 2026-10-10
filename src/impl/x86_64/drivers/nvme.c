// nvme.c - NVMe 1.x controller, polled (NVM Express base spec 1.4)
//
//   1. PCI: memory + bus master; BAR 0 (64-bit) holds the registers.
//   2. Disable (CC.EN = 0, wait CSTS.RDY = 0), give it the admin queues
//      (AQA, ASQ, ACQ), enable (CC: 4 KiB pages, 64-byte submission and
//      16-byte completion entries), wait CSTS.RDY = 1.
//   3. Admin commands: identify the controller (model, serial) and namespace
//      1 (size, block size); create one I/O completion and submission queue.
//   4. Reads and writes go through a 64 KiB bounce buffer described by a PRP
//      list, so callers may pass any buffer.
#include "drivers/nvme.h"
#include "sys/smp.h"
#include "drivers/part.h"
#include "drivers/pci.h"
#include "drivers/paging.h"
#include "drivers/heap.h"
#include "drivers/timer.h"
#include "drivers/msi.h"
#include "lib/print.h"
#include "lib/string.h"
#include <stdint.h>

#define REG_CAP    0x00
#define REG_VS     0x08
#define REG_CC     0x14
#define REG_CSTS   0x1C
#define REG_AQA    0x24
#define REG_ASQ    0x28
#define REG_ACQ    0x30

#define QDEPTH       64
#define BOUNCE_PAGES 16                  /* 64 KiB per command */

typedef struct {
    uint8_t opcode, flags;
    uint16_t cid;
    uint32_t nsid;
    uint64_t rsvd;
    uint64_t mptr;
    uint64_t prp1, prp2;
    uint32_t cdw10, cdw11, cdw12, cdw13, cdw14, cdw15;
} __attribute__((packed)) sqe_t;

typedef struct {
    uint32_t dw0, dw1;
    uint16_t sq_head, sq_id;
    uint16_t cid, status;                /* status bit 0: phase */
} __attribute__((packed)) cqe_t;

typedef struct {
    sqe_t *sq;
    cqe_t *cq;
    uint16_t tail, head, phase, id;
} queue_t;

static volatile uint8_t *regs;
static uint32_t dstrd;
static queue_t admin, io;
static uint8_t *bounce;
static uint64_t *prp_list;
static uint64_t ns_blocks;
static uint32_t block_size = 512;
static char model[41], serial[21];
static int ready;
static char desc[120];
static uint16_t next_cid;
static int irq_mode;                     /* completions interrupt: wait with hlt, not spinning */

static void nvme_interrupt(void) { }     /* waking the waiting CPU is all it takes */

static int interrupts_on(void) {
    uint64_t f;
    __asm__ volatile("pushfq; pop %0" : "=r"(f));
    return (f & 0x200) != 0;
}

static uint32_t rd32(uint32_t o) { return *(volatile uint32_t *)(regs + o); }
static uint64_t rd64(uint32_t o) { return rd32(o) | (uint64_t)rd32(o + 4) << 32; }
static void wr32(uint32_t o, uint32_t v) { *(volatile uint32_t *)(regs + o) = v; }
static void wr64(uint32_t o, uint64_t v) { wr32(o, (uint32_t)v); wr32(o + 4, (uint32_t)(v >> 32)); }

static void *dma_alloc(uint64_t size) {
    uint8_t *raw = kmalloc(size + 4096);
    if (!raw) return 0;
    uint8_t *p = (uint8_t *)(((uint64_t)raw + 4095) & ~4095ull);
    memset(p, 0, size);
    return p;
}

static void doorbell(uint16_t qid, int completion, uint16_t value) {
    wr32(0x1000 + (2u * qid + (completion ? 1 : 0)) * (4u << dstrd), value);
}

/* Submit and wait (up to `ms`). Returns the status field (0 = success), -1 on timeout. */
static int submit(queue_t *q, sqe_t *cmd, uint32_t ms) {
    cmd->cid = ++next_cid;
    q->sq[q->tail] = *cmd;
    q->tail = (uint16_t)((q->tail + 1) % QDEPTH);
    __asm__ volatile("mfence" ::: "memory");
    doorbell(q->id, 0, q->tail);
    uint32_t start = get_tick(), ticks = ms * TIMER_FREQ / 1000 + 2;
    while ((uint32_t)(get_tick() - start) < ticks) {
        volatile cqe_t *e = &q->cq[q->head];
        if ((e->status & 1) == q->phase) {
            uint16_t status = e->status >> 1, cid = e->cid;
            q->head = (uint16_t)((q->head + 1) % QDEPTH);
            if (!q->head) q->phase ^= 1;
            doorbell(q->id, 1, q->head);
            if (cid != cmd->cid) continue;                  /* (an old completion) */
            return status & 0x7FFF;
        }
        if (irq_mode && interrupts_on()) cpu_wait();   /* the completion (or a tick) wakes us */
        else __asm__ volatile("pause");
    }
    return -1;
}

static int wait_ready(int want, uint32_t ms) {
    uint32_t start = get_tick(), ticks = ms * TIMER_FREQ / 1000 + 2;
    while ((uint32_t)(get_tick() - start) < ticks) {
        uint32_t s = rd32(REG_CSTS);
        if (s == 0xFFFFFFFF) return -1;
        if (s & 2) return -2;                               /* fatal status */
        if ((s & 1) == (uint32_t)want) return 0;
    }
    return -1;
}

static void copy_ascii(char *out, const uint8_t *in, int n) {
    memcpy(out, in, (size_t)n);
    out[n] = 0;
    for (int i = n - 1; i >= 0 && (out[i] == ' ' || !out[i]); i--) out[i] = 0;
}

static int find_controller(pci_device_t *out, int *vmd) {
    static pci_device_t devs[64];
    int n = pci_scan(devs, 64);
    *vmd = 0;
    for (int i = 0; i < n; i++) {
        if (devs[i].class_code == 0x01 && devs[i].subclass == 0x08 && devs[i].prog_if == 0x02) {
            *out = devs[i];
            return 0;
        }
        /* Intel VMD ("RST" mode): the drives hide behind it. */
        if (devs[i].vendor == 0x8086 && (devs[i].device == 0x467F || devs[i].device == 0x9A0B ||
                                         devs[i].device == 0xA77F || devs[i].device == 0x7D0B))
            *vmd = 1;
    }
    return -1;
}

int nvme_init(char *how, int size) {
    pci_device_t d;
    int vmd;
    if (find_controller(&d, &vmd) != 0) {
        k_snprintf(how, (size_t)size, vmd ? "the SSD is behind Intel VMD (\"RST\" mode in the firmware setup): "
                                            "switch the storage mode to AHCI/NVMe to use it"
                                          : "no NVMe drive");
        return -1;
    }
    uint32_t after;
    pci_power_on(d.bus, d.slot, d.func, &after);
    uint32_t bar0 = pci_config_read_dword(d.bus, d.slot, d.func, 0x10);
    uint64_t base = bar0 & ~0xFull;
    if (((bar0 >> 1) & 3) == 2) base |= (uint64_t)pci_config_read_dword(d.bus, d.slot, d.func, 0x14) << 32;
    if (!base) base = pci_assign_bar0(d.bus, d.slot, d.func);
    if (!base) {
        k_snprintf(how, (size_t)size, "the NVMe controller has no address");
        return -1;
    }
    uint32_t c = pci_config_read_dword(d.bus, d.slot, d.func, 0x04);
    pci_config_write_dword(d.bus, d.slot, d.func, 0x04, (c & 0xFFFF) | 0x2 | 0x4 | 0x400);
    if (!regs) regs = mmio_map(base, 0x4000);

    uint64_t cap = rd64(REG_CAP);
    dstrd = (uint32_t)((cap >> 32) & 0xF);
    uint32_t timeout = (uint32_t)((cap >> 24) & 0xFF) * 500 + 500;   /* ms */
    if (((cap >> 48) & 0xF) > 0) {                          /* minimum page size above 4 KiB */
        k_snprintf(how, (size_t)size, "the controller does not support 4 KiB pages");
        return -1;
    }

    /* Disable, set up the admin queues, enable. */
    wr32(REG_CC, rd32(REG_CC) & ~1u);
    if (wait_ready(0, timeout) != 0) {
        k_snprintf(how, (size_t)size, "the controller did not stop (CSTS %08x)", rd32(REG_CSTS));
        return -1;
    }
    if (!admin.sq) {
        admin.sq = dma_alloc(4096);
        admin.cq = dma_alloc(4096);
        io.sq = dma_alloc(4096);
        io.cq = dma_alloc(4096);
        bounce = dma_alloc(BOUNCE_PAGES * 4096);
        prp_list = dma_alloc(4096);
        if (!admin.sq || !admin.cq || !io.sq || !io.cq || !bounce || !prp_list) return -1;
        for (int i = 0; i < BOUNCE_PAGES; i++) prp_list[i] = (uint64_t)bounce + (uint64_t)i * 4096;
    }
    memset(admin.sq, 0, 4096);
    memset(admin.cq, 0, 4096);
    admin.tail = admin.head = 0;
    admin.phase = 1;
    admin.id = 0;
    wr32(REG_AQA, (QDEPTH - 1) << 16 | (QDEPTH - 1));
    wr64(REG_ASQ, (uint64_t)admin.sq);
    wr64(REG_ACQ, (uint64_t)admin.cq);
    wr32(REG_CC, (4u << 20) | (6u << 16) | 1u);             /* CQ entry 16 B, SQ entry 64 B, NVM, 4 KiB, enable */
    if (wait_ready(1, timeout) != 0) {
        k_snprintf(how, (size_t)size, "the controller did not start (CSTS %08x)", rd32(REG_CSTS));
        return -1;
    }

    /* Identify the controller and namespace 1. */
    sqe_t cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.opcode = 0x06;
    cmd.prp1 = (uint64_t)bounce;
    cmd.cdw10 = 1;
    if (submit(&admin, &cmd, 2000) != 0) {
        k_snprintf(how, (size_t)size, "identify failed");
        return -1;
    }
    copy_ascii(serial, bounce + 4, 20);
    copy_ascii(model, bounce + 24, 40);
    memset(&cmd, 0, sizeof(cmd));
    cmd.opcode = 0x06;
    cmd.nsid = 1;
    cmd.prp1 = (uint64_t)bounce;
    cmd.cdw10 = 0;
    if (submit(&admin, &cmd, 2000) != 0) {
        k_snprintf(how, (size_t)size, "namespace 1 not found");
        return -1;
    }
    ns_blocks = *(uint64_t *)bounce;
    uint8_t flbas = bounce[26] & 0xF;
    block_size = 1u << ((*(uint32_t *)(bounce + 128 + 4 * flbas) >> 16) & 0xFF);
    if (block_size != 512 && block_size != 4096) {
        k_snprintf(how, (size_t)size, "unsupported block size %u", block_size);
        return -1;
    }

    /* One I/O queue pair. Completions raise MSI/MSI-X vector 0 when that is
     * available (the waits sleep then); otherwise they are polled. */
    static int msi_tried;
    if (!msi_tried) {
        msi_tried = 1;
        irq_mode = msi_enable(d.bus, d.slot, d.func, nvme_interrupt, "NVMe", MSI_PREFER_MSIX) == 0;
    }
    memset(io.sq, 0, 4096);
    memset(io.cq, 0, 4096);
    io.tail = io.head = 0;
    io.phase = 1;
    io.id = 1;
    memset(&cmd, 0, sizeof(cmd));
    cmd.opcode = 0x05;                                      /* create I/O completion queue */
    cmd.prp1 = (uint64_t)io.cq;
    cmd.cdw10 = (QDEPTH - 1) << 16 | 1;
    cmd.cdw11 = 1 | (irq_mode ? 2u : 0);                    /* contiguous; interrupts on vector 0 */
    int s1 = submit(&admin, &cmd, 2000);
    memset(&cmd, 0, sizeof(cmd));
    cmd.opcode = 0x01;                                      /* create I/O submission queue */
    cmd.prp1 = (uint64_t)io.sq;
    cmd.cdw10 = (QDEPTH - 1) << 16 | 1;
    cmd.cdw11 = 1u << 16 | 1;                               /* completion queue 1, contiguous */
    int s2 = s1 == 0 ? submit(&admin, &cmd, 2000) : -1;
    if (s1 != 0 || s2 != 0) {
        k_snprintf(how, (size_t)size, "could not create the I/O queues (%d, %d)", s1, s2);
        return -1;
    }
    ready = 1;
    uint64_t mb = ns_blocks * block_size / (1024 * 1024);
    k_snprintf(desc, sizeof(desc), "%s, %u.%u GB", model[0] ? model : "NVMe drive", (uint32_t)(mb / 1000),
               (uint32_t)(mb % 1000 / 100));
    k_snprintf(how, (size_t)size, "%s", desc);
    return 0;
}

int nvme_present(void) { return ready; }

uint64_t nvme_sectors(void) { return ready ? ns_blocks * (block_size / 512) : 0; }

/* One command of up to 64 KiB through the bounce buffer. */
static int rw(int write, uint64_t lba, uint32_t count, uint8_t *buf) {
    uint32_t per = block_size / 512;
    if (lba % per || count % per) return -1;                /* 4 KiB drives: whole blocks only */
    while (count) {
        uint32_t n = count > BOUNCE_PAGES * 8 ? BOUNCE_PAGES * 8 : count;
        uint32_t bytes = n * 512;
        if (write) memcpy(bounce, buf, bytes);
        sqe_t cmd;
        memset(&cmd, 0, sizeof(cmd));
        cmd.opcode = write ? 0x01 : 0x02;
        cmd.nsid = 1;
        cmd.prp1 = (uint64_t)bounce;
        if (bytes > 8192) cmd.prp2 = (uint64_t)&prp_list[1];   /* the rest of the pages */
        else if (bytes > 4096) cmd.prp2 = (uint64_t)bounce + 4096;
        uint64_t blk = lba / per;
        cmd.cdw10 = (uint32_t)blk;
        cmd.cdw11 = (uint32_t)(blk >> 32);
        cmd.cdw12 = n / per - 1;
        if (submit(&io, &cmd, 5000) != 0) return -1;
        if (!write) memcpy(buf, bounce, bytes);
        buf += bytes;
        lba += n;
        count -= n;
    }
    return 0;
}

int nvme_read(uint64_t lba, uint32_t count, void *buf) {
    if (!ready || lba + count > nvme_sectors()) return -1;
    return rw(0, lba, count, buf);
}

int nvme_write(uint64_t lba, uint32_t count, const void *buf) {
    if (!ready || lba + count > nvme_sectors()) return -1;
    return rw(1, lba, count, (uint8_t *)buf);
}

int nvme_partition_reader(uint64_t lba, uint32_t count, void *buf) { return nvme_read(lba, count, buf); }

void nvme_print_info(void) {
    if (!ready) {
        char how[160];
        if (nvme_init(how, sizeof(how)) != 0) {
            kprintf("NVMe: %s\n", how);
            return;
        }
    }
    kprintf("NVMe: %s (serial %s), %u-byte blocks, NVMe %u.%u\n", desc, serial, block_size, rd32(REG_VS) >> 16,
            (rd32(REG_VS) >> 8) & 0xFF);
    static partition_t parts[16];
    int n = part_scan(nvme_partition_reader, parts, 16);
    if (!n) {
        kprintf("  no partition table\n");
        return;
    }
    for (int i = 0; i < n; i++) {
        partition_t *p = &parts[i];
        uint64_t mb = p->sectors / 2048;
        kprintf("  %d: %-18s %6u MB  %s", i + 1, p->kind, (uint32_t)mb, p->name);
        if (p->fat32) kprintf("  FAT32");
        if (p->label[0]) kprintf(" \"%s\"", p->label);
        if (p->ours) kprintf("  <- Terminal OS files (read-write)");
        kprintf("\n");
    }
}
