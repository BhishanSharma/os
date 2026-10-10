// vfs.c - the mount table, path lookup, and the file calls of drivers/fat32.h
#include "sys/vfs.h"
#include "drivers/fatvol.h"
#include "drivers/fat32.h"
#include "lib/print.h"
#include "lib/string.h"

typedef struct {
    int used;
    char path[64];                        /* "/", "/SYS" */
    fatvol_t vol;
} mount_t;

static mount_t mounts[VFS_MAX_MOUNTS];
static char cwd[FAT32_MAX_PATH] = "/";
static int (*write_guard)(const char *path);
static int (*read_guard)(const char *path);

void fat32_set_write_guard(int (*guard)(const char *path)) { write_guard = guard; }
void fat32_set_read_guard(int (*guard)(const char *path)) { read_guard = guard; }

const char *vfs_error(int code) {
    switch (code) {
        case FATV_NOT_FOUND:      return "no such file or folder";
        case FATV_EXISTS:         return "already exists";
        case FATV_BAD_NAME:       return "names are 8.3: up to 8 letters, a dot, up to 3 more";
        case FATV_NOT_EMPTY:      return "the folder is not empty";
        case FATV_FULL:           return "the disk is full";
        case FATV_READ_ONLY:      return "read-only volume";
        case FAT32_ERR_PERMISSION: return "permission denied";
        default:                  return "disk error";
    }
}

/* ---- Paths ------------------------------------------------------------------------- */

int vfs_normalize(const char *path, char *out, int size) {
    char comps[MAX_PATH_DEPTH * 2][16];
    int n = 0;
    for (int pass = 0; pass < 2; pass++) {
        const char *p = pass == 0 ? (path[0] == '/' ? "" : cwd) : path;
        while (*p) {
            while (*p == '/') p++;
            if (!*p) break;
            char c[64];
            int len = 0;
            while (*p && *p != '/') {
                if (len < 63) c[len++] = *p;
                p++;
            }
            c[len] = 0;
            if (!strcmp(c, ".")) continue;
            if (!strcmp(c, "..")) {
                if (n) n--;
                continue;
            }
            if (len > 15 || n >= MAX_PATH_DEPTH * 2) return -1;
            for (int i = 0; i <= len; i++) comps[n][i] = (c[i] >= 'a' && c[i] <= 'z') ? (char)(c[i] - 32) : c[i];
            n++;
        }
    }
    int o = 0;
    if (size < 2) return -1;
    out[o++] = '/';
    for (int i = 0; i < n; i++) {
        int len = (int)strlen(comps[i]);
        if (o + len + 2 > size) return -1;
        if (i) out[o++] = '/';
        memcpy(out + o, comps[i], (size_t)len);
        o += len;
    }
    out[o] = 0;
    return 0;
}

/* The mount holding absolute path `abs`; *rest is the part below it. */
static mount_t *find_mount(const char *abs, const char **rest) {
    mount_t *best = 0;
    size_t best_len = 0;
    for (int i = 0; i < VFS_MAX_MOUNTS; i++) {
        mount_t *m = &mounts[i];
        if (!m->used) continue;
        size_t len = strlen(m->path);
        int match = len == 1 || (!strncmp(abs, m->path, len) && (abs[len] == '/' || abs[len] == 0));
        if (match && (!best || len > best_len)) {
            best = m;
            best_len = len;
        }
    }
    if (best) {
        const char *r = abs + (best_len == 1 ? 0 : best_len);
        while (*r == '/') r++;
        *rest = r;
    }
    return best;
}

typedef struct {
    char abs[FAT32_MAX_PATH];
    mount_t *m;
    uint32_t dir;                         /* the folder holding `base` (or the folder itself) */
    char base[16];                        /* last component; "" for a mount's root */
} target_t;

/* Resolve `path`. With `parent`, stop at the folder holding the last name. */
static int resolve(const char *path, int parent, target_t *t) {
    if (vfs_normalize(path, t->abs, sizeof(t->abs)) != 0) return FATV_BAD_NAME;
    const char *rest;
    t->m = find_mount(t->abs, &rest);
    if (!t->m) return FATV_NOT_FOUND;
    t->dir = t->m->vol.root;
    t->base[0] = 0;
    while (*rest) {
        char c[16];
        int len = 0;
        while (*rest && *rest != '/') c[len++] = *rest++;
        c[len] = 0;
        while (*rest == '/') rest++;
        if (parent && !*rest) {
            memcpy(t->base, c, (size_t)len + 1);
            break;
        }
        fatent_t f;
        int r = fatvol_lookup(&t->m->vol, t->dir, c, &f);
        if (r) return r;
        if (!(f.e.attributes & FAT_ATTR_DIRECTORY)) return FATV_NOT_FOUND;
        uint32_t cl = fatent_cluster(&f);
        t->dir = cl ? cl : t->m->vol.root;
    }
    return 0;
}

/* ---- Mounts ----------------------------------------------------------------------- */

int vfs_mount(const char *path, const blockdev_t *dev) {
    char abs[64];
    if (path[0] != '/' || vfs_normalize(path, abs, sizeof(abs)) != 0) return FATV_BAD_NAME;
    mount_t *slot = 0;
    for (int i = 0; i < VFS_MAX_MOUNTS; i++)
        if (mounts[i].used && !strcmp(mounts[i].path, abs)) slot = &mounts[i];
    if (!slot)
        for (int i = 0; i < VFS_MAX_MOUNTS && !slot; i++)
            if (!mounts[i].used) slot = &mounts[i];
    if (!slot) return FATV_FULL;
    static fatvol_t probe;
    int r = fatvol_open(&probe, dev);
    if (r) return r;
    slot->vol = probe;
    kstrncpy(slot->path, abs, sizeof(slot->path));
    slot->used = 1;
    target_t t;
    if (resolve(cwd, 0, &t) != 0) kstrncpy(cwd, "/", sizeof(cwd));
    return 0;
}

int vfs_unmount(const char *path) {
    char abs[64];
    if (vfs_normalize(path, abs, sizeof(abs)) != 0 || !strcmp(abs, "/")) return FATV_BAD_NAME;
    for (int i = 0; i < VFS_MAX_MOUNTS; i++)
        if (mounts[i].used && !strcmp(mounts[i].path, abs)) {
            mounts[i].used = 0;
            target_t t;
            if (resolve(cwd, 0, &t) != 0) kstrncpy(cwd, "/", sizeof(cwd));
            return 0;
        }
    return FATV_NOT_FOUND;
}

static mount_t *mount_at(const char *abs) {
    for (int i = 0; i < VFS_MAX_MOUNTS; i++)
        if (mounts[i].used && !strcmp(mounts[i].path, abs)) return &mounts[i];
    return 0;
}

void vfs_print_mounts(void) {
    for (int i = 0; i < VFS_MAX_MOUNTS; i++) {
        mount_t *m = &mounts[i];
        if (!m->used) continue;
        uint64_t mb = (uint64_t)m->vol.clusters * m->vol.bpc / (1024 * 1024);
        kprintf("  %-8s %s, FAT32 \"%s\", %u MiB%s\n", m->path, m->vol.dev.name,
                m->vol.label, (uint32_t)mb, m->vol.dev.kind == DISK_RAM ? " (lost at reboot)" : "");
    }
}

const char *vfs_system_dir(void) {
    for (int i = 0; i < VFS_MAX_MOUNTS; i++)
        if (mounts[i].used && mounts[i].vol.dev.kind == DISK_RAM) return mounts[i].path;
    return "/";
}

const char *vfs_root_name(void) {
    mount_t *m = mount_at("/");
    return m ? m->vol.dev.name : "none";
}

int vfs_read_sectors(uint64_t lba, uint32_t count, void *buf) {
    mount_t *m = mount_at("/");
    return m ? m->vol.dev.read(m->vol.dev.ctx, lba, count, buf) : -1;
}

int vfs_root_persistent(void) {
    mount_t *m = mount_at("/");
    return m && m->vol.dev.kind != DISK_RAM;
}

/* ---- The file calls (drivers/fat32.h) ------------------------------------------------ */

int fat32_read_file(const char *path, uint8_t *buffer, uint32_t max_size) {
    target_t t;
    int r = resolve(path, 1, &t);
    if (r) return -1;
    if (read_guard && !read_guard(t.abs)) return FAT32_ERR_PERMISSION;
    if (!t.base[0]) return -1;
    fatent_t f;
    if (fatvol_lookup(&t.m->vol, t.dir, t.base, &f) != 0 || (f.e.attributes & FAT_ATTR_DIRECTORY)) return -1;
    r = fatvol_read(&t.m->vol, &f, buffer, max_size);
    return r < 0 ? -2 : r;
}

int fat32_file_exists(const char *path) {
    target_t t;
    if (resolve(path, 1, &t) != 0) return 0;
    if (!t.base[0] || mount_at(t.abs)) return 1;
    fatent_t f;
    return fatvol_lookup(&t.m->vol, t.dir, t.base, &f) == 0;
}

uint32_t fat32_get_file_size(const char *path) {
    target_t t;
    if (resolve(path, 1, &t) != 0) return 0xFFFFFFFFu;
    if (!t.base[0] || mount_at(t.abs)) return 0;
    fatent_t f;
    if (fatvol_lookup(&t.m->vol, t.dir, t.base, &f) != 0) return 0xFFFFFFFFu;
    return f.e.file_size;
}

int fat32_write_file(const char *path, const uint8_t *buffer, uint32_t size) {
    target_t t;
    int r = resolve(path, 1, &t);
    if (r) return r;
    if (write_guard && !write_guard(t.abs)) return FAT32_ERR_PERMISSION;
    if (!t.base[0] || mount_at(t.abs)) return FATV_EXISTS;
    fatent_t f;
    r = fatvol_lookup(&t.m->vol, t.dir, t.base, &f);
    if (r == FATV_NOT_FOUND) r = fatvol_create(&t.m->vol, t.dir, t.base, 0, &f);
    if (r) return r;
    return fatvol_write(&t.m->vol, &f, buffer, size);
}

int fat32_create_file(const char *path) {
    target_t t;
    int r = resolve(path, 1, &t);
    if (r) return r;
    if (write_guard && !write_guard(t.abs)) return FAT32_ERR_PERMISSION;
    if (!t.base[0] || mount_at(t.abs)) return -1;
    r = fatvol_create(&t.m->vol, t.dir, t.base, 0, 0);
    return r == FATV_EXISTS ? -1 : r;
}

int fat32_mkdir(const char *path) {
    target_t t;
    int r = resolve(path, 1, &t);
    if (r) return r;
    if (write_guard && !write_guard(t.abs)) return FAT32_ERR_PERMISSION;
    if (!t.base[0] || mount_at(t.abs)) return FATV_EXISTS;
    return fatvol_create(&t.m->vol, t.dir, t.base, 1, 0);
}

int fat32_delete_file(const char *path) {
    target_t t;
    int r = resolve(path, 1, &t);
    if (r) return -1;
    if (write_guard && !write_guard(t.abs)) return FAT32_ERR_PERMISSION;
    if (!t.base[0] || mount_at(t.abs)) return FATV_EXISTS;   /* a mount point: `umount` it */
    fatent_t f;
    if (fatvol_lookup(&t.m->vol, t.dir, t.base, &f) != 0) return -1;
    return fatvol_delete(&t.m->vol, &f);
}

int fat32_change_directory(const char *path) {
    target_t t;
    if (resolve(path, 0, &t) != 0) return -1;
    kstrncpy(cwd, t.abs, sizeof(cwd));
    return 0;
}

int fat32_get_current_directory(char *buffer, uint32_t size) {
    kstrncpy(buffer, cwd, size);
    return (int)strlen(buffer);
}

int fat32_list_directory_ex(const char *path, fat32_file_info_t *files, uint32_t max_files) {
    target_t t;
    if (resolve(path && path[0] ? path : ".", 0, &t) != 0) return -1;
    int n = fatvol_list(&t.m->vol, t.dir, files, (int)max_files);
    if (n < 0) return -1;
    /* Volumes mounted right below this folder show up as folders in it. */
    size_t len = strlen(t.abs);
    for (int i = 0; i < VFS_MAX_MOUNTS && n < (int)max_files; i++) {
        mount_t *m = &mounts[i];
        if (!m->used || !strcmp(m->path, "/") || strncmp(m->path, t.abs, len)) continue;
        const char *name = m->path + (len == 1 ? 1 : len + 1);
        if ((len > 1 && m->path[len] != '/') || !*name) continue;
        int nested = 0, dup = 0;
        for (const char *c = name; *c; c++) nested |= *c == '/';
        for (int k = 0; k < n; k++) dup |= !strcmp(files[k].name, name);
        if (nested || dup) continue;
        memset(&files[n], 0, sizeof(files[n]));
        kstrncpy(files[n].name, name, sizeof(files[n].name));
        files[n].is_directory = 1;
        files[n].attributes = FAT_ATTR_DIRECTORY;
        n++;
    }
    return n;
}

int fat32_list_directory(fat32_file_info_t *files, uint32_t max_files) {
    return fat32_list_directory_ex(0, files, max_files);
}
