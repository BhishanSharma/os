// disk.h - block devices for file systems: ATA disk, a drive's partition, RAM disk
#ifndef DISK_H
#define DISK_H

#include <stdint.h>

typedef enum { DISK_NONE, DISK_ATA, DISK_RAM, DISK_DRIVE, DISK_USB } disk_kind_t;

/* A device in 512-byte sectors; a volume reads and writes through it. */
typedef struct {
    int (*read)(void *ctx, uint64_t lba, uint32_t count, void *buf);
    int (*write)(void *ctx, uint64_t lba, uint32_t count, const void *buf);
    int (*flush)(void *ctx);             /* optional: the device's write cache onto the medium */
    void *ctx;
    uint64_t sectors;
    int writable;
    disk_kind_t kind;
    int drive;                           /* DISK_DRIVE: which (drivers/drive.h) */
    uint64_t part_start;                 /* DISK_DRIVE: the partition's first sector on the drive */
    char name[40];                       /* "RAM disk", "NVMe partition 3" */
} blockdev_t;

/* A RAM disk is a FAT32 image in memory (loaded by GRUB as a boot module). */
void disk_set_ramdisk(uint8_t *base, uint64_t size);
uint64_t disk_ramdisk_size(void);        // 0 if there is none

/* A block device for the ATA disk or the RAM disk. 0 on success. */
int disk_open(disk_kind_t kind, blockdev_t *out);
/* One partition of drive `drive` (NVMe, SATA: drivers/drive.h); writes only
 * if `writable` (our own TERMINALOS partition: never Windows' data). */
int disk_open_drive_partition(int drive, uint64_t start, uint64_t sectors, int writable, int number, blockdev_t *out);
/* One partition of USB stick `stick` (usb_storage_*); writes only if `writable`. */
int disk_open_usb_partition(int stick, uint64_t start, uint64_t sectors, int writable, blockdev_t *out);

#endif
