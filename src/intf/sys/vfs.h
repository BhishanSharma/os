// vfs.h - mounts and paths
//
// Every file name the system uses is a path: absolute ("/HOME/BOSS/A.TXT")
// or relative to the current folder ("../A.TXT"), with "." and "..". The
// mount table maps the leading folders to volumes: "/" is the main volume
// and others hang below it ("/SYS"). The file calls in drivers/fat32.h
// (fat32_read_file and friends) take such paths and are implemented here.
//
// Layout: the RAM disk (system files, programs, firmware) is "/", unless a
// TERMINALOS partition is found on the NVMe drive: then that is "/" (so
// homes, accounts and settings survive a reboot) and the RAM disk is "/SYS".
#ifndef VFS_H
#define VFS_H

#include <stdint.h>
#include "drivers/disk.h"

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
int vfs_normalize(const char *path, char *out, int size);

/* Raw sectors of the main volume (for `readsector`, `fat32info`). */
int vfs_read_sectors(uint64_t lba, uint32_t count, void *buf);

/* "no such file", "names are 8.3", ... for a negative result. */
const char *vfs_error(int code);

#endif
