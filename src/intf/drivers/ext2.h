// ext2.h - ext2/ext3 volumes: the Linux file system, with its journal
//
// Works like fatvol.h on (volume, folder inode, name); the VFS turns paths
// into those and checks permissions. Files have an owner, a group and Unix
// mode bits. Names keep their case but are found without it, as on macOS.
//
// Every change (a file written, a folder made, a name removed) is one
// transaction: the metadata blocks it touched go to the journal first, in
// ext3's format (Linux and e2fsck replay it too), then to their places. A
// crash or power cut leaves the volume as it was before the change or after
// it, never half way. A file's new contents go to free blocks before the
// transaction (ordered mode), so a file being replaced keeps its old
// contents until the new ones are complete.
//
// Volumes made by Linux mount if they use only ext2/ext3 features (no
// extents, flex_bg or 64-bit: ext4 is refused).
#ifndef EXT2_H
#define EXT2_H

#include <stdint.h>
#include "drivers/disk.h"
#include "drivers/fatvol.h"                /* the FATV_* error codes, fat32_file_info_t */

#define EXT2_ROOT_INO  2

/* Mode bits (as in Unix). */
#define EXT2_S_IFMT    0xF000
#define EXT2_S_IFDIR   0x4000
#define EXT2_S_IFREG   0x8000
#define EXT2_S_IFLNK   0xA000
#define EXT2_S_ISVTX   01000               /* sticky: only owners delete in this folder (/TMP) */

typedef struct {
    uint32_t ino;
    uint32_t dir;                         /* the folder it was found in */
    uint16_t mode, links;
    uint32_t uid, gid;
    uint64_t size;
    uint32_t mtime;                       /* seconds since 1970, UTC */
    char name[256];
} ext2ent_t;

typedef struct {
    uint32_t blk;
    uint8_t dirty;
    uint32_t used;                        /* for evicting the least recently used */
    uint8_t *data;
} ext2buf_t;

typedef struct {
    blockdev_t dev;
    uint32_t bs, spb;                     /* block size, sectors per block */
    uint32_t blocks, inodes, groups, bpg, ipg, first_data, isize, first_ino;
    uint32_t sb_blk, sb_off;              /* where the superblock is */
    int readonly;
    char label[17];
    uint32_t *jmap, jlen, jseq;           /* the journal's blocks, their count, the next transaction */
    uint8_t *jsb;                         /* the journal's superblock */
    ext2buf_t *cache;                     /* metadata blocks; the dirty ones are the transaction */
    int ncache, capcache, ndirty, maxdirty;
    uint32_t tick, goal_group;
    int clean;                            /* marked clean on disk (ext2_mark_clean) */
    int replayed;                         /* transactions a crash left, finished at mount */
    int crash_after_commit;               /* test hook (`crash journal`): stop with the journal full */
} ext2vol_t;

/* Mount: 0, FATV_BAD_NAME if `dev` does not hold ext2/ext3 we can use,
 * FATV_IO. A journal left by a crash is replayed first. */
int ext2_open(ext2vol_t *v, const blockdev_t *dev);
/* Unmount: marks the volume clean (no recovery needed) and frees its memory. */
void ext2_close(ext2vol_t *v);
/* Before power off: marked clean but still mounted (the next change marks it
 * in use again). */
void ext2_mark_clean(ext2vol_t *v);

/* Is this the superblock (the 1024 bytes at byte 1024 of the volume)?
 * Fills the label. For partition tables. */
int ext2_probe(const uint8_t *super, char label[17]);

/* A new ext3 file system on all of `dev` (at least 8 MiB), with an empty root
 * folder owned by root. Everything on it is lost. 0 on success. */
int ext2_format(const blockdev_t *dev, const char *label);

int ext2_stat(ext2vol_t *v, uint32_t ino, ext2ent_t *out);
int ext2_lookup(ext2vol_t *v, uint32_t dir, const char *name, ext2ent_t *out);
/* The folder's entries (not "." and ".."); first_cluster holds the inode. */
int ext2_list(ext2vol_t *v, uint32_t dir, fat32_file_info_t *out, int max);
/* Up to `len` bytes from `offset`: bytes read, 0 at the end, or < 0. */
int ext2_read_at(ext2vol_t *v, const ext2ent_t *f, uint32_t offset, uint8_t *buf, uint32_t len);

/* A file or folder (by `mode`'s type) in `dir`; FATV_EXISTS if the name is
 * taken (in any case). */
int ext2_create(ext2vol_t *v, uint32_t dir, const char *name, uint16_t mode, uint32_t uid, uint32_t gid,
                ext2ent_t *out);
/* Replace the file's contents. */
int ext2_write(ext2vol_t *v, ext2ent_t *f, const uint8_t *data, uint32_t size);
/* Remove a file, or an empty folder. */
int ext2_delete(ext2vol_t *v, ext2ent_t *f);
/* Change permissions (-1 keeps each). */
int ext2_setattr(ext2vol_t *v, ext2ent_t *f, int32_t mode, int64_t uid, int64_t gid);
/* Move to `dir` as `name`; a file there by that name is replaced. */
int ext2_rename(ext2vol_t *v, ext2ent_t *f, uint32_t dir, const char *name);

/* Size and free space, in bytes. */
void ext2_space(ext2vol_t *v, uint64_t *total, uint64_t *free_bytes);

#endif
