// part.h - partition tables (GPT and MBR)
//
// Terminal OS writes to a drive only inside a partition that is clearly its
// own: a FAT32 volume labelled TERMINALOS (made in Windows' Disk Management,
// for example). Everything else on the drive is read-only.
#ifndef PART_H
#define PART_H

#include <stdint.h>

typedef struct {
    uint64_t start, sectors;          /* in 512-byte sectors */
    char name[40];                    /* GPT name ("Basic data partition"), or "" */
    char label[12];                   /* FAT32 volume label, or "" */
    int fat32;                        /* holds a FAT32 file system */
    int ours;                         /* FAT32 labelled TERMINALOS: may be written */
    const char *kind;                 /* "EFI system", "Microsoft data", ... */
} partition_t;

typedef int (*sector_reader_t)(uint64_t lba, uint32_t count, void *buf);

/* Read the partition table through `rd`. Returns how many were found. */
int part_scan(sector_reader_t rd, partition_t *out, int max);

#define OUR_LABEL "TERMINALOS"

#endif
