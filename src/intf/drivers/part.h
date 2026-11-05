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
    int number;                       /* its number in the table (GPT entry or MBR slot), from 1 */
    int gpt;                          /* from a GPT (else MBR) */
    uint8_t guid[16];                 /* GPT: the partition's own GUID, as stored */
    const char *fs;                   /* "FAT32", "NTFS", "exFAT", "BitLocker", "empty", "unknown" */
    char name[40];                    /* GPT name ("Basic data partition"), or "" */
    char label[17];                   /* FAT32 or ext2 volume label, or "" */
    int fat32;                        /* holds a FAT32 file system */
    int ext2;                         /* holds ext2/ext3 (label from its superblock) */
    int ours;                         /* labelled TERMINALOS (FAT32, ext2) or TERMOS DATA (ext2): may be written */
    const char *kind;                 /* "EFI system", "Microsoft data", ... */
} partition_t;

typedef int (*sector_reader_t)(uint64_t lba, uint32_t count, void *buf);
typedef int (*sector_writer_t)(uint64_t lba, uint32_t count, const void *buf);

/* Read the partition table through `rd`. Returns how many were found. */
int part_scan(sector_reader_t rd, partition_t *out, int max);

#define OUR_LABEL "TERMINALOS"
#define DATA_LABEL "TERMOS DATA"      /* the boot stick's data partition (ext3) */

/* GPT type "Linux file system data", as stored. */
extern const uint8_t part_type_linux[16];

/* Add partition [start, end] (sectors, inclusive) to a GPT drive of
 * `sectors`, and move the backup table to the drive's end (a stick written
 * with a smaller image has it in the middle). Refuses if it overlaps a
 * partition. 0 on success. */
int part_gpt_add(sector_reader_t rd, sector_writer_t wr, uint64_t sectors, uint64_t start, uint64_t end,
                 const uint8_t type[16], const char *name);

/* Is this a partition Windows (or the firmware) needs: the EFI system
 * partition, Microsoft reserved, Windows recovery, or one holding NTFS,
 * exFAT or BitLocker? Terminal OS never writes to those. */
int part_is_protected(const partition_t *p);

#endif
