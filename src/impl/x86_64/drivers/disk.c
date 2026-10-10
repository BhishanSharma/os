// disk.c - block devices: the ATA disk, NVMe partitions and the RAM disk
#include "drivers/disk.h"
#include "drivers/ata.h"
#include "drivers/nvme.h"
#include "drivers/heap.h"
#include "lib/string.h"

#define SECTOR_SIZE 512

static uint8_t *ram_base;
static uint64_t ram_size;

void disk_set_ramdisk(uint8_t *base, uint64_t size) {
    ram_base = base;
    ram_size = size;
}

uint64_t disk_ramdisk_size(void) { return ram_base ? ram_size : 0; }

static int ram_read(void *ctx, uint64_t lba, uint32_t count, void *buf) {
    (void)ctx;
    if ((lba + count) * SECTOR_SIZE > ram_size) return -1;
    memcpy(buf, ram_base + lba * SECTOR_SIZE, (size_t)count * SECTOR_SIZE);
    return 0;
}

static int ram_write(void *ctx, uint64_t lba, uint32_t count, const void *buf) {
    (void)ctx;
    if ((lba + count) * SECTOR_SIZE > ram_size) return -1;
    memcpy(ram_base + lba * SECTOR_SIZE, buf, (size_t)count * SECTOR_SIZE);
    return 0;
}

static int ata_rd(void *ctx, uint64_t lba, uint32_t count, void *buf) {
    (void)ctx;
    return lba + count > 0xFFFFFFFFull ? -1 : ata_read_sectors((uint32_t)lba, count, buf);
}

static int ata_wr(void *ctx, uint64_t lba, uint32_t count, const void *buf) {
    (void)ctx;
    return lba + count > 0xFFFFFFFFull ? -1 : ata_write_sectors((uint32_t)lba, count, (uint8_t *)buf);
}

typedef struct { uint64_t start, sectors; int writable; } nvme_part_t;

static int nvme_part_read(void *ctx, uint64_t lba, uint32_t count, void *buf) {
    nvme_part_t *p = ctx;
    if (lba + count > p->sectors) return -1;
    return nvme_read(p->start + lba, count, buf);
}

static int nvme_part_write(void *ctx, uint64_t lba, uint32_t count, const void *buf) {
    nvme_part_t *p = ctx;
    if (!p->writable || lba + count > p->sectors) return -1;    /* only inside our own partition */
    return nvme_write(p->start + lba, count, buf);
}

int disk_open(disk_kind_t kind, blockdev_t *out) {
    memset(out, 0, sizeof(*out));
    out->kind = kind;
    out->writable = 1;
    if (kind == DISK_RAM) {
        if (!ram_base) return -1;
        out->read = ram_read;
        out->write = ram_write;
        out->sectors = ram_size / SECTOR_SIZE;
        kstrncpy(out->name, "RAM disk", sizeof(out->name));
        return 0;
    }
    if (kind == DISK_ATA) {
        out->read = ata_rd;
        out->write = ata_wr;
        out->sectors = 0xFFFFFFFFull;
        kstrncpy(out->name, "ATA disk", sizeof(out->name));
        return 0;
    }
    return -1;
}

int disk_open_nvme_partition(uint64_t start, uint64_t sectors, int writable, int number, blockdev_t *out) {
    nvme_part_t *p = kmalloc(sizeof(*p));
    if (!p) return -1;
    p->start = start;
    p->sectors = sectors;
    p->writable = writable;
    memset(out, 0, sizeof(*out));
    out->kind = DISK_NVME;
    out->read = nvme_part_read;
    out->write = nvme_part_write;
    out->ctx = p;
    out->sectors = sectors;
    out->writable = writable;
    if (writable) kstrncpy(out->name, "NVMe disk", sizeof(out->name));
    else k_snprintf(out->name, sizeof(out->name), "NVMe partition %d", number);
    return 0;
}
