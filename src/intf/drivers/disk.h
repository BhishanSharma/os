// disk.h - block devices for file systems: ATA disk, NVMe partition, RAM disk
#ifndef DISK_H
#define DISK_H

#include <stdint.h>

typedef enum { DISK_NONE, DISK_ATA, DISK_RAM, DISK_NVME } disk_kind_t;

/* A device in 512-byte sectors; a volume reads and writes through it. */
typedef struct {
    int (*read)(void *ctx, uint64_t lba, uint32_t count, void *buf);
    int (*write)(void *ctx, uint64_t lba, uint32_t count, const void *buf);
    void *ctx;
    uint64_t sectors;
    int writable;
    disk_kind_t kind;
    char name[40];                       /* "RAM disk", "NVMe partition 3" */
} blockdev_t;

/* A RAM disk is a FAT32 image in memory (loaded by GRUB as a boot module). */
void disk_set_ramdisk(uint8_t *base, uint64_t size);
uint64_t disk_ramdisk_size(void);        // 0 if there is none

/* A block device for the ATA disk or the RAM disk. 0 on success. */
int disk_open(disk_kind_t kind, blockdev_t *out);
/* One partition of the NVMe drive; writes only if `writable` (our own
 * TERMINALOS partition: never Windows' data). */
int disk_open_nvme_partition(uint64_t start, uint64_t sectors, int writable, int number, blockdev_t *out);

#endif
