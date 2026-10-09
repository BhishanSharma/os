// disk.c - dispatch sector I/O to the ATA driver or the RAM disk
#include "drivers/disk.h"
#include "drivers/ata.h"
#include "lib/string.h"

#define SECTOR_SIZE 512

static disk_kind_t selected = DISK_NONE;
static uint8_t *ram_base;
static uint64_t ram_size;

void disk_set_ramdisk(uint8_t *base, uint64_t size) {
    ram_base = base;
    ram_size = size;
}

uint64_t disk_ramdisk_size(void) { return ram_base ? ram_size : 0; }

void disk_select(disk_kind_t kind) { selected = kind; }
disk_kind_t disk_selected(void) { return selected; }

const char *disk_name(void) {
    switch (selected) {
        case DISK_ATA: return "ATA disk";
        case DISK_RAM: return "RAM disk";
        default:       return "none";
    }
}

static uint8_t *ram_sector(uint32_t lba, uint32_t count) {
    if (!ram_base || count == 0) return 0;
    uint64_t end = ((uint64_t)lba + count) * SECTOR_SIZE;
    return end <= ram_size ? ram_base + (uint64_t)lba * SECTOR_SIZE : 0;
}

int disk_read_sectors(uint32_t lba, uint32_t count, uint8_t *buffer) {
    if (selected == DISK_ATA) return ata_read_sectors(lba, count, buffer);
    if (selected == DISK_RAM) {
        uint8_t *src = ram_sector(lba, count);
        if (!src) return -1;
        memcpy(buffer, src, (size_t)count * SECTOR_SIZE);
        return 0;
    }
    return -1;
}

int disk_write_sectors(uint32_t lba, uint32_t count, uint8_t *buffer) {
    if (selected == DISK_ATA) return ata_write_sectors(lba, count, buffer);
    if (selected == DISK_RAM) {
        uint8_t *dst = ram_sector(lba, count);
        if (!dst) return -1;
        memcpy(dst, buffer, (size_t)count * SECTOR_SIZE);
        return 0;
    }
    return -1;
}
