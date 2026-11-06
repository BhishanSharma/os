// vfs.h - mounts and paths
//
// Every file name the system uses is a path: absolute ("/HOME/BOSS/A.TXT")
// or relative to the current folder ("../A.TXT"), with "." and "..". The
// mount table maps the leading folders to volumes: "/" is the main volume
// and others hang below it ("/SYS"). The file calls in drivers/fat32.h
// (fat32_read_file and friends) take such paths and are implemented here.
//
// Layout: the RAM disk (system files, programs, firmware) is "/", unless a
// volume that keeps files is found: the boot stick's data partition (ext3,
// "TERMOS DATA") or a TERMINALOS partition on the NVMe drive. Then that is
// "/" (homes, accounts and settings survive a reboot) and the RAM disk is
// "/SYS".
//
// Volumes are FAT32 or ext2/ext3. On ext2 each file has an owner, a group
// and mode bits, checked the Unix way: search (x) on every folder of a path,
// read or write on the file, write on the folder to create or remove a name,
// and in a sticky folder (/TMP) only owners remove. Root may do anything.
// FAT32 has no owners: there the guards below decide by path.
#ifndef VFS_H
#define VFS_H

#include "sys/task.h"
#include <stdint.h>
#include "drivers/disk.h"
#include "drivers/fatvol.h"
#include "drivers/ext2.h"

#define VFS_R 4
#define VFS_W 2
#define VFS_X 1

#define VFS_ERR_NO_OWNERS  (-95)          /* chmod, chown, rename on FAT32 */
#define VFS_ERR_CROSS      (-18)          /* rename to another volume */

/* Who is asking (sys/users.c): fills the current user's uid and gid, and
 * returns 1 when no permission applies (the kernel itself, root, or the
 * kernel acting for a user, as when it saves the account file). */
void vfs_set_creds(int (*creds)(uint32_t *uid, uint32_t *gid));
/* And the sandbox (sys/process.c): 0 if the current program may not touch `abs`. */
void vfs_set_path_check(int (*check)(const char *abs));

typedef struct {
    uint32_t size;
    int is_dir;
    uint16_t mode;                        /* permission bits (FAT32: 0755 folders, 0644 files) */
    uint32_t uid, gid, mtime;             /* mtime: seconds since 1970 (0 on FAT32) */
    int owners;                           /* 1 on ext2; on FAT32 these are made up */
} vfs_stat_t;

int vfs_stat(const char *path, vfs_stat_t *out);
int vfs_chmod(const char *path, uint32_t mode);           /* the owner or root */
int vfs_chown(const char *path, int64_t uid, int64_t gid); /* root; -1 keeps */
int vfs_rename(const char *from, const char *to);         /* replaces a file at `to` */
/* May the current user `want` (VFS_R | VFS_W | VFS_X) `path`? For a file
 * not there yet, VFS_W asks whether it may be created. 1 or 0. */
int vfs_access(const char *path, int want);
/* Size and free space of the volume holding `path`. 0 on success. */
int vfs_space(const char *path, uint64_t *total, uint64_t *free_bytes);
/* Before power off or a reboot: ext2 volumes are marked clean. */
void vfs_shutdown(void);
/* `crash journal`: the next change on an ext2 volume stops after its journal
 * commit, as if the power went there (for the system tests). */
void vfs_crash_after_commit(void);

#define VFS_MAX_MOUNTS 6

/* Mount the FAT32 volume on `dev` at `path` ("/" replaces the main volume).
 * 0 on success, negative (FATV_*) otherwise. */
int vfs_mount(const char *path, const blockdev_t *dev);
int vfs_unmount(const char *path);

/* `mount` with no arguments: the table. */
void vfs_print_mounts(void);

/* Where the RAM disk's files are: "/SYS", or "/" when it is the main volume. */
const char *vfs_system_dir(void);

/* The main volume's device ("RAM disk", "NVMe partition 4"), and whether
 * files there survive a reboot. */
const char *vfs_root_name(void);
int vfs_root_persistent(void);

/* The absolute, upper-case form of `path` (relative to the current folder).
 * 0 on success, -1 if it is too long. */
/* Held by every call that reaches the disk (recursive; see vfs.c). */
extern kmutex_t vfs_lock;

int vfs_normalize(const char *path, char *out, int size);
/* The same against `base` instead of the current directory (which belongs to
 * the kernel's shell: programs keep their own and pass it here). */
int vfs_absolute(const char *base, const char *path, char *out, int size);
/* If `path` is a folder: its absolute path as the disk spells it, 0. */
int vfs_folder(const char *path, char *out, int size);

/* Raw sectors of the main volume (for `readsector`, `fat32info`). */
int vfs_read_sectors(uint64_t lba, uint32_t count, void *buf);

/* "no such file", "names are 8.3", ... for a negative result. */
const char *vfs_error(int code);

/* A file opened for reading in parts (big files: programs read them as they
 * go instead of all at once). */
typedef struct {
    void *mount;
    fatent_t ent;                         /* FAT32 */
    fatpos_t pos;
    ext2ent_t xent;                       /* ext2 */
} vfs_file_t;

int vfs_open_file(const char *path, vfs_file_t *out);     /* 0, or < 0 (FAT32_ERR_PERMISSION, ...) */
int vfs_read_at(vfs_file_t *f, uint32_t offset, void *buf, uint32_t len);
uint32_t vfs_file_size(const vfs_file_t *f);

/* Where partition `start` of drive `drive` is mounted, or 0. */
const char *vfs_partition_mount(int drive, uint64_t start);

#endif
