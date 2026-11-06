// drive.h - the computer's own drives (NVMe, SATA): where partitions are
//
// Each driver adds the drives it finds; the partition table of every drive
// is read the same way (part_scan), and Terminal OS writes only inside a
// partition that is its own (part.h). USB sticks are separate (usb.h).
#ifndef DRIVE_H
#define DRIVE_H

#include <stdint.h>
#include "drivers/part.h"

#define MAX_DRIVES 8

typedef struct {
    const char *kind;                    /* "NVMe", "SATA" */
    char model[48];
    uint64_t sectors;                    /* in 512-byte sectors */
    int (*read)(void *ctx, uint64_t lba, uint32_t count, void *buf);
    int (*write)(void *ctx, uint64_t lba, uint32_t count, const void *buf);
    int (*flush)(void *ctx);             /* optional: the drive's write cache onto the medium */
    void *ctx;
} drive_t;

/* A driver found a drive: its number (from 0), -1 if there are too many. */
int drive_add(const drive_t *d);
int drive_count(void);
const drive_t *drive_get(int n);

int drive_read(int n, uint64_t lba, uint32_t count, void *buf);
int drive_write(int n, uint64_t lba, uint32_t count, const void *buf);

/* Drive n's partitions (part_scan). How many were found. */
int drive_scan(int n, partition_t *out, int max);

/* "NVMe drive" or "SATA drive 2" (several of a kind are numbered from 1). */
void drive_label(int n, char *out, int size);

/* `disks`: every drive, its model and size, and its partitions. */
void drive_print_info(void);

#endif
