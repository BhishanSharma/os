// disk.h - the block device the filesystem uses: the ATA disk or a RAM disk
#ifndef DISK_H
#define DISK_H

#include <stdint.h>

typedef enum { DISK_NONE, DISK_ATA, DISK_RAM } disk_kind_t;

/* A RAM disk is a FAT32 image in memory (loaded by GRUB as a boot module). */
void disk_set_ramdisk(uint8_t *base, uint64_t size);
uint64_t disk_ramdisk_size(void);        // 0 if there is none

/* Route disk_read_sectors/disk_write_sectors to `kind`. */
void disk_select(disk_kind_t kind);
disk_kind_t disk_selected(void);
const char *disk_name(void);             // "ATA disk", "RAM disk" or "none"

/* 512-byte sectors. Return 0 on success, negative on error. */
int disk_read_sectors(uint32_t lba, uint32_t count, uint8_t *buffer);
int disk_write_sectors(uint32_t lba, uint32_t count, uint8_t *buffer);

#endif
