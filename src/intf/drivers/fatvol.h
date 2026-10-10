// fatvol.h - FAT32 volumes: the on-disk format, one volume per instance
//
// Works on (volume, folder cluster, name): no paths, no current folder. The
// VFS (sys/vfs.c) turns paths into those and handles mounts and permissions.
// Names are 8.3 (no long names), compared without case.
#ifndef FATVOL_H
#define FATVOL_H

#include <stdint.h>
#include "drivers/disk.h"
#include "drivers/fat32.h"

typedef struct {
    blockdev_t dev;
    uint32_t spc, bpc;                    /* sectors / bytes per cluster */
    uint32_t fat_start, fat_size, num_fats, data_start;
    uint32_t root, clusters;              /* root folder cluster, number of data clusters */
    uint32_t alloc_hint;
    uint8_t media;
    char label[12];
    uint32_t cache_sector;                /* one FAT sector, cached (write-through) */
    int cache_valid;
    uint8_t cache[512];
} fatvol_t;

/* A folder entry and where it lives (so it can be updated in place). */
typedef struct {
    fat32_dir_entry_t e;
    uint32_t slot_cluster, slot_index;    /* the folder cluster holding it, entry number */
} fatent_t;

#define FATV_NOT_FOUND   (-2)
#define FATV_EXISTS      (-17)
#define FATV_BAD_NAME    (-22)
#define FATV_NOT_EMPTY   (-39)
#define FATV_FULL        (-28)
#define FATV_IO          (-5)
#define FATV_READ_ONLY   (-30)

int fatvol_open(fatvol_t *v, const blockdev_t *dev);     /* 0 if `dev` holds FAT32 */

/* Is `name` a valid 8.3 name? Fills the 11-byte on-disk form. */
int fatvol_name(const char *name, uint8_t out[11]);
void fatvol_name_string(const uint8_t name[11], char *out);

uint32_t fatent_cluster(const fatent_t *f);

int fatvol_lookup(fatvol_t *v, uint32_t dir, const char *name, fatent_t *out);
int fatvol_list(fatvol_t *v, uint32_t dir, fat32_file_info_t *out, int max);
int fatvol_read(fatvol_t *v, const fatent_t *f, uint8_t *buf, uint32_t max);
/* Create a file (or folder) in `dir`; FATV_EXISTS if the name is taken. */
int fatvol_create(fatvol_t *v, uint32_t dir, const char *name, int is_dir, fatent_t *out);
/* Replace the file's contents. */
int fatvol_write(fatvol_t *v, fatent_t *f, const uint8_t *data, uint32_t size);
/* Remove a file, or an empty folder. */
int fatvol_delete(fatvol_t *v, fatent_t *f);

#endif
