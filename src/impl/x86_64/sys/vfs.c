// vfs.c - the mount table, path lookup, permissions, and the file calls of drivers/fat32.h
//
// A mount is a FAT32 volume (fatvol.h) or an ext2/ext3 one (ext2.h); each
// call below does the same for either. Permissions: see vfs.h.
#include "sys/vfs.h"
#include "drivers/fatvol.h"
#include "drivers/ext2.h"
#include "drivers/fat32.h"
#include "lib/print.h"
#include "lib/string.h"
#include "drivers/heap.h"

typedef struct {
    int used;
    char path[64];                        /* "/", "/SYS" */
    int ext;                              /* ext2/ext3 (else FAT32) */
    fatvol_t vol;
    ext2vol_t ev;
} mount_t;

static mount_t mounts[VFS_MAX_MOUNTS];
static char cwd[FAT32_MAX_PATH] = "/";

/* Names keep their case but are compared without it (as on Windows). */
static char up(char c) { return c >= 'a' && c <= 'z' ? (char)(c - 32) : c; }

static int same_prefix(const char *a, const char *b, size_t n) {
    for (size_t i = 0; i < n; i++) {
        if (up(a[i]) != up(b[i])) return 0;
        if (!a[i]) return 1;
    }
    return 1;
}

static int same_name(const char *a, const char *b) {
    return same_prefix(a, b, (size_t)-1);
}
static int (*write_guard)(const char *path);
static int (*read_guard)(const char *path);
static int (*creds)(uint32_t *uid, uint32_t *gid);

void fat32_set_write_guard(int (*guard)(const char *path)) { write_guard = guard; }
void fat32_set_read_guard(int (*guard)(const char *path)) { read_guard = guard; }
void vfs_set_creds(int (*fn)(uint32_t *uid, uint32_t *gid)) { creds = fn; }

const char *vfs_error(int code) {
    switch (code) {
        case FATV_NOT_FOUND:      return "no such file or folder";
        case FATV_EXISTS:         return "already exists";
        case FATV_BAD_NAME:       return "not a valid name";
        case FATV_NOT_EMPTY:      return "the folder is not empty";
        case FATV_FULL:           return "the disk is full";
        case FATV_READ_ONLY:      return "read-only volume";
        case FAT32_ERR_PERMISSION: return "permission denied";
        case VFS_ERR_NO_OWNERS:   return "not possible on FAT32 (no owners or permissions there)";
        case VFS_ERR_CROSS:       return "not on the same volume";
        default:                  return "disk error";
    }
}

static const blockdev_t *mdev(const mount_t *m) { return m->ext ? &m->ev.dev : &m->vol.dev; }

/* ---- Permissions (ext2) -------------------------------------------------------------- */

/* The current user; 1 if no permission applies (the kernel, root). */
static int who(uint32_t *uid, uint32_t *gid) {
    *uid = *gid = 0;
    return !creds || creds(uid, gid);
}

static int allowed(const ext2ent_t *o, int want) {
    uint32_t uid, gid;
    if (who(&uid, &gid)) return 1;
    int bits = uid == o->uid ? o->mode >> 6 : gid == o->gid ? o->mode >> 3 : o->mode;
    return (bits & want) == want;
}

/* May the current user remove (or rename) `f` from `folder`? Write and search
 * on the folder; in a sticky folder also being the file's or the folder's owner. */
static int may_remove(const ext2ent_t *folder, const ext2ent_t *f) {
    uint32_t uid, gid;
    if (who(&uid, &gid)) return 1;
    if (!allowed(folder, VFS_W | VFS_X)) return 0;
    return !(folder->mode & EXT2_S_ISVTX) || uid == f->uid || uid == folder->uid;
}

/* ---- Paths ------------------------------------------------------------------------- */

int vfs_absolute(const char *base, const char *path, char *out, int size) {
    int starts[MAX_PATH_DEPTH * 2], n = 0, o = 0;          /* where each "/name" begins in out */
    if (size < 2) return -1;
    for (int pass = 0; pass < 2; pass++) {
        const char *p = pass == 0 ? (path[0] == '/' ? "" : base) : path;
        while (*p) {
            while (*p == '/') p++;
            if (!*p) break;
            const char *c = p;
            while (*p && *p != '/') p++;
            int len = (int)(p - c);
            if (len == 1 && c[0] == '.') continue;
            if (len == 2 && c[0] == '.' && c[1] == '.') {
                if (n) o = starts[--n];
                continue;
            }
            if (len > 255 || n >= MAX_PATH_DEPTH * 2 || o + len + 2 > size) return -1;
            starts[n++] = o;
            out[o++] = '/';
            memcpy(out + o, c, (size_t)len);
            o += len;
        }
    }
    if (o == 0) out[o++] = '/';
    out[o] = 0;
    return 0;
}

int vfs_normalize(const char *path, char *out, int size) {
    return vfs_absolute(cwd, path, out, size);
}

/* The mount holding absolute path `abs`; *rest is the part below it. */
static mount_t *find_mount(const char *abs, const char **rest) {
    mount_t *best = 0;
    size_t best_len = 0;
    for (int i = 0; i < VFS_MAX_MOUNTS; i++) {
        mount_t *m = &mounts[i];
        if (!m->used) continue;
        size_t len = strlen(m->path);
        int match = len == 1 || (same_prefix(abs, m->path, len) && (abs[len] == '/' || abs[len] == 0));
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
    char abs[FAT32_MAX_PATH];             /* with each folder's name as the disk spells it */
    mount_t *m;
    uint32_t dir;                         /* the folder holding `base` (or the folder itself): cluster or inode */
    char base[256];                       /* last component; "" for a mount's root */
    ext2ent_t folder;                     /* ext2: that folder, for its owner and mode */
} target_t;

/* Add "/name" to t->abs. */
static int abs_append(target_t *t, const char *name) {
    size_t o = strlen(t->abs), n = strlen(name);
    if (o == 1) o = 0;                                      /* "/" + "A" is "/A" */
    if (o + n + 2 > sizeof(t->abs)) return FATV_BAD_NAME;
    t->abs[o] = '/';
    memcpy(t->abs + o + 1, name, n + 1);
    return 0;
}

/* One step of a path: `name` in folder `dir`, which must be a folder too. */
static int step(target_t *t, const char *name, void *scratch) {
    if (t->m->ext) {
        ext2ent_t *e = scratch;
        if (!allowed(&t->folder, VFS_X)) return FAT32_ERR_PERMISSION;
        int r = ext2_lookup(&t->m->ev, t->dir, name, e);
        if (r) return r;
        if ((e->mode & EXT2_S_IFMT) != EXT2_S_IFDIR) return FATV_NOT_FOUND;
        t->dir = e->ino;
        t->folder = *e;
        return abs_append(t, e->name);
    }
    fatent_t *f = scratch;
    int r = fatvol_lookup(&t->m->vol, t->dir, name, f);
    if (r) return r;
    if (!(f->e.attributes & FAT_ATTR_DIRECTORY)) return FATV_NOT_FOUND;
    uint32_t cl = fatent_cluster(f);
    t->dir = cl ? cl : t->m->vol.root;
    return abs_append(t, f->name);
}

/* Resolve `path`. With `parent`, stop at the folder holding the last name
 * (search permission on it checked); without, the last name is a folder that
 * was entered (search permission on it too). */
static int resolve(const char *path, int parent, target_t *t) {
    char norm[FAT32_MAX_PATH];
    if (vfs_normalize(path, norm, sizeof(norm)) != 0) return FATV_BAD_NAME;
    const char *rest;
    t->m = find_mount(norm, &rest);
    if (!t->m) return FATV_NOT_FOUND;
    kstrncpy(t->abs, t->m->path, sizeof(t->abs));
    t->base[0] = 0;
    if (t->m->ext) {
        t->dir = EXT2_ROOT_INO;
        if (ext2_stat(&t->m->ev, EXT2_ROOT_INO, &t->folder) != 0) return FATV_IO;
    } else {
        t->dir = t->m->vol.root;
    }
    void *scratch = kmalloc(sizeof(fatent_t) > sizeof(ext2ent_t) ? sizeof(fatent_t) : sizeof(ext2ent_t));
    if (!scratch) return FATV_IO;
    int r = 0;
    while (*rest && !r) {
        char c[256];
        int len = 0;
        while (*rest && *rest != '/') c[len++] = *rest++;
        c[len] = 0;
        while (*rest == '/') rest++;
        if (parent && !*rest) {
            if (t->m->ext && !allowed(&t->folder, VFS_X)) r = FAT32_ERR_PERMISSION;
            memcpy(t->base, c, (size_t)len + 1);
            if (!r) r = abs_append(t, c);
            break;
        }
        r = step(t, c, scratch);
    }
    kfree(scratch);
    if (!r && !parent && t->m->ext && !allowed(&t->folder, VFS_X)) r = FAT32_ERR_PERMISSION;
    return r;
}

/* ---- Mounts ----------------------------------------------------------------------- */

static mount_t *mount_at(const char *abs) {
    for (int i = 0; i < VFS_MAX_MOUNTS; i++)
        if (mounts[i].used && same_name(mounts[i].path, abs)) return &mounts[i];
    return 0;
}

static void release(mount_t *m) {
    if (m->used && m->ext) ext2_close(&m->ev);
    m->used = 0;
}

static int vfs_mount_unlocked(const char *path, const blockdev_t *dev) {
    char abs[64];
    if (path[0] != '/' || vfs_normalize(path, abs, sizeof(abs)) != 0) return FATV_BAD_NAME;
    for (char *c = abs; *c; c++) *c = up(*c);              /* mount points in capitals: /NVME */
    mount_t *slot = 0;
    for (int i = 0; i < VFS_MAX_MOUNTS; i++)
        if (mounts[i].used && !strcmp(mounts[i].path, abs)) slot = &mounts[i];
    if (!slot)
        for (int i = 0; i < VFS_MAX_MOUNTS && !slot; i++)
            if (!mounts[i].used) slot = &mounts[i];
    if (!slot) return FATV_FULL;
    static fatvol_t probe;
    int r = fatvol_open(&probe, dev);
    if (r == 0) {
        release(slot);
        slot->vol = probe;
        slot->ext = 0;
    } else {
        if (slot->used && slot->ext) release(slot);         /* (one cache per ext2 volume) */
        int e = ext2_open(&slot->ev, dev);
        if (e) return r == FATV_BAD_NAME ? e : r;
        if (slot->ev.replayed)
            kprintf("%s: finished %d change%s a crash cut off (from the journal)\n", dev->name, slot->ev.replayed,
                    slot->ev.replayed == 1 ? "" : "s");
        slot->ext = 1;
    }
    kstrncpy(slot->path, abs, sizeof(slot->path));
    slot->used = 1;
    target_t t;
    if (resolve(cwd, 0, &t) != 0) kstrncpy(cwd, "/", sizeof(cwd));
    return 0;
}

static int vfs_unmount_unlocked(const char *path) {
    char abs[64];
    if (vfs_normalize(path, abs, sizeof(abs)) != 0 || !strcmp(abs, "/")) return FATV_BAD_NAME;
    for (int i = 0; i < VFS_MAX_MOUNTS; i++)
        if (mounts[i].used && same_name(mounts[i].path, abs)) {
            release(&mounts[i]);
            target_t t;
            if (resolve(cwd, 0, &t) != 0) kstrncpy(cwd, "/", sizeof(cwd));
            return 0;
        }
    return FATV_NOT_FOUND;
}

void vfs_print_mounts(void) {
    for (int i = 0; i < VFS_MAX_MOUNTS; i++) {
        mount_t *m = &mounts[i];
        if (!m->used) continue;
        if (m->ext) {
            uint64_t total, free_bytes;
            ext2_space(&m->ev, &total, &free_bytes);
            kprintf("  %-8s %s, %s \"%s\", %u MiB (%u MiB free)%s\n", m->path, m->ev.dev.name,
                    m->ev.jlen ? "ext3" : "ext2", m->ev.label, (uint32_t)(total >> 20), (uint32_t)(free_bytes >> 20),
                    m->ev.readonly ? ", read-only" : "");
            continue;
        }
        uint64_t mb = (uint64_t)m->vol.clusters * m->vol.bpc / (1024 * 1024);
        kprintf("  %-8s %s, FAT32 \"%s\", %u MiB%s\n", m->path, m->vol.dev.name,
                m->vol.label, (uint32_t)mb, m->vol.dev.kind == DISK_RAM ? " (lost at reboot)" : "");
    }
}

const char *vfs_system_dir(void) {
    for (int i = 0; i < VFS_MAX_MOUNTS; i++)
        if (mounts[i].used && mdev(&mounts[i])->kind == DISK_RAM) return mounts[i].path;
    return "/";
}

const char *vfs_root_name(void) {
    mount_t *m = mount_at("/");
    return m ? mdev(m)->name : "none";
}

static int vfs_read_sectors_unlocked(uint64_t lba, uint32_t count, void *buf) {
    mount_t *m = mount_at("/");
    return m ? mdev(m)->read(mdev(m)->ctx, lba, count, buf) : -1;
}

int vfs_root_persistent(void) {
    mount_t *m = mount_at("/");
    return m && mdev(m)->kind != DISK_RAM;
}

const char *vfs_partition_mount(uint64_t start) {
    for (int i = 0; i < VFS_MAX_MOUNTS; i++)
        if (mounts[i].used && mdev(&mounts[i])->kind == DISK_NVME && mdev(&mounts[i])->part_start == start)
            return mounts[i].path;
    return 0;
}

/* ---- The file calls (drivers/fat32.h) ------------------------------------------------ */

/* A new file or folder `t->base` in t->dir (ext2), owned by the current user. */
static int ext_create(target_t *t, int dir, ext2ent_t *out) {
    if (!allowed(&t->folder, VFS_W | VFS_X)) return FAT32_ERR_PERMISSION;
    uint32_t uid, gid;
    who(&uid, &gid);
    uint16_t mode = dir ? EXT2_S_IFDIR | 0755 : EXT2_S_IFREG | 0644;
    return ext2_create(&t->m->ev, t->dir, t->base, mode, uid, gid, out);
}

static int fat32_read_file_unlocked(const char *path, uint8_t *buffer, uint32_t max_size) {
    target_t t;
    int r = resolve(path, 1, &t);
    if (r) return r == FAT32_ERR_PERMISSION ? r : -1;
    if (!t.m->ext) {
        if (read_guard && !read_guard(t.abs)) return FAT32_ERR_PERMISSION;
        if (!t.base[0]) return -1;
        fatent_t f;
        if (fatvol_lookup(&t.m->vol, t.dir, t.base, &f) != 0 || (f.e.attributes & FAT_ATTR_DIRECTORY)) return -1;
        r = fatvol_read(&t.m->vol, &f, buffer, max_size);
        return r < 0 ? -2 : r;
    }
    ext2ent_t f;
    if (!t.base[0] || ext2_lookup(&t.m->ev, t.dir, t.base, &f) != 0 ||
        (f.mode & EXT2_S_IFMT) == EXT2_S_IFDIR)
        return -1;
    if (!allowed(&f, VFS_R)) return FAT32_ERR_PERMISSION;
    uint32_t done = 0;
    while (done < max_size) {
        int n = ext2_read_at(&t.m->ev, &f, done, buffer + done, max_size - done);
        if (n < 0) return -2;
        if (n == 0) break;
        done += (uint32_t)n;
    }
    return (int)done;
}

static int fat32_file_exists_unlocked(const char *path) {
    target_t t;
    if (resolve(path, 1, &t) != 0) return 0;
    if (!t.base[0] || mount_at(t.abs)) return 1;
    if (t.m->ext) {
        ext2ent_t f;
        return ext2_lookup(&t.m->ev, t.dir, t.base, &f) == 0;
    }
    fatent_t f;
    return fatvol_lookup(&t.m->vol, t.dir, t.base, &f) == 0;
}

static uint32_t fat32_get_file_size_unlocked(const char *path) {
    target_t t;
    if (resolve(path, 1, &t) != 0) return 0xFFFFFFFFu;
    if (!t.base[0] || mount_at(t.abs)) return 0;
    if (t.m->ext) {
        ext2ent_t f;
        if (ext2_lookup(&t.m->ev, t.dir, t.base, &f) != 0) return 0xFFFFFFFFu;
        if ((f.mode & EXT2_S_IFMT) == EXT2_S_IFDIR) return 0;
        return f.size > 0xFFFFFFF0u ? 0xFFFFFFF0u : (uint32_t)f.size;
    }
    fatent_t f;
    if (fatvol_lookup(&t.m->vol, t.dir, t.base, &f) != 0) return 0xFFFFFFFFu;
    return f.e.file_size;
}

static int fat32_write_file_unlocked(const char *path, const uint8_t *buffer, uint32_t size) {
    target_t t;
    int r = resolve(path, 1, &t);
    if (r) return r;
    if (!t.m->ext && write_guard && !write_guard(t.abs)) return FAT32_ERR_PERMISSION;
    if (!t.base[0] || mount_at(t.abs)) return FATV_EXISTS;
    if (t.m->ext) {
        ext2ent_t f;
        r = ext2_lookup(&t.m->ev, t.dir, t.base, &f);
        if (r == FATV_NOT_FOUND) r = ext_create(&t, 0, &f);
        else if (r == 0 && !allowed(&f, VFS_W)) r = FAT32_ERR_PERMISSION;
        return r ? r : ext2_write(&t.m->ev, &f, buffer, size);
    }
    fatent_t f;
    r = fatvol_lookup(&t.m->vol, t.dir, t.base, &f);
    if (r == FATV_NOT_FOUND) r = fatvol_create(&t.m->vol, t.dir, t.base, 0, &f);
    if (r) return r;
    return fatvol_write(&t.m->vol, &f, buffer, size);
}

static int fat32_create_file_unlocked(const char *path) {
    target_t t;
    int r = resolve(path, 1, &t);
    if (r) return r;
    if (!t.m->ext && write_guard && !write_guard(t.abs)) return FAT32_ERR_PERMISSION;
    if (!t.base[0] || mount_at(t.abs)) return -1;
    if (t.m->ext) {
        ext2ent_t f;
        if (ext2_lookup(&t.m->ev, t.dir, t.base, &f) == 0) return -1;
        return ext_create(&t, 0, 0);
    }
    r = fatvol_create(&t.m->vol, t.dir, t.base, 0, 0);
    return r == FATV_EXISTS ? -1 : r;
}

static int fat32_mkdir_unlocked(const char *path) {
    target_t t;
    int r = resolve(path, 1, &t);
    if (r) return r;
    if (!t.m->ext && write_guard && !write_guard(t.abs)) return FAT32_ERR_PERMISSION;
    if (!t.base[0] || mount_at(t.abs)) return FATV_EXISTS;
    if (t.m->ext) {
        ext2ent_t f;
        if (ext2_lookup(&t.m->ev, t.dir, t.base, &f) == 0) return FATV_EXISTS;
        return ext_create(&t, 1, 0);
    }
    return fatvol_create(&t.m->vol, t.dir, t.base, 1, 0);
}

static int fat32_delete_file_unlocked(const char *path) {
    target_t t;
    int r = resolve(path, 1, &t);
    if (r) return r == FAT32_ERR_PERMISSION ? r : -1;
    if (!t.m->ext && write_guard && !write_guard(t.abs)) return FAT32_ERR_PERMISSION;
    if (!t.base[0] || mount_at(t.abs)) return FATV_EXISTS;   /* a mount point: `umount` it */
    if (t.m->ext) {
        ext2ent_t f;
        if (ext2_lookup(&t.m->ev, t.dir, t.base, &f) != 0) return -1;
        if (!may_remove(&t.folder, &f)) return FAT32_ERR_PERMISSION;
        return ext2_delete(&t.m->ev, &f);
    }
    fatent_t f;
    if (fatvol_lookup(&t.m->vol, t.dir, t.base, &f) != 0) return -1;
    return fatvol_delete(&t.m->vol, &f);
}

static int fat32_change_directory_unlocked(const char *path) {
    target_t t;
    if (resolve(path, 0, &t) != 0) return -1;
    kstrncpy(cwd, t.abs, sizeof(cwd));
    return 0;
}

static int vfs_folder_unlocked(const char *path, char *out, int size) {
    target_t t;
    if (resolve(path, 0, &t) != 0) return -1;
    kstrncpy(out, t.abs, (size_t)size);
    return 0;
}

int fat32_get_current_directory(char *buffer, uint32_t size) {
    kstrncpy(buffer, cwd, size);
    return (int)strlen(buffer);
}

static int fat32_list_directory_ex_unlocked(const char *path, fat32_file_info_t *files, uint32_t max_files) {
    target_t t;
    if (resolve(path && path[0] ? path : ".", 0, &t) != 0) return -1;
    int n;
    if (t.m->ext) {
        if (!allowed(&t.folder, VFS_R)) return -1;
        n = ext2_list(&t.m->ev, t.dir, files, (int)max_files);
    } else {
        n = fatvol_list(&t.m->vol, t.dir, files, (int)max_files);
    }
    if (n < 0) return -1;
    /* Volumes mounted right below this folder show up as folders in it. */
    size_t len = strlen(t.abs);
    for (int i = 0; i < VFS_MAX_MOUNTS && n < (int)max_files; i++) {
        mount_t *m = &mounts[i];
        if (!m->used || !strcmp(m->path, "/") || !same_prefix(m->path, t.abs, len)) continue;
        const char *name = m->path + (len == 1 ? 1 : len + 1);
        if ((len > 1 && m->path[len] != '/') || !*name) continue;
        int nested = 0, dup = 0;
        for (const char *c = name; *c; c++) nested |= *c == '/';
        for (int k = 0; k < n; k++) dup |= same_name(files[k].name, name);
        if (nested || dup) continue;
        memset(&files[n], 0, sizeof(files[n]));
        kstrncpy(files[n].name, name, sizeof(files[n].name));
        files[n].is_directory = 1;
        files[n].attributes = FAT_ATTR_DIRECTORY;
        files[n].mode = EXT2_S_IFDIR | 0755;
        n++;
    }
    return n;
}

int fat32_list_directory(fat32_file_info_t *files, uint32_t max_files) {
    return fat32_list_directory_ex(0, files, max_files);
}

static int vfs_open_file_unlocked(const char *path, vfs_file_t *out) {
    target_t t;
    int r = resolve(path, 1, &t);
    if (r) return r;
    if (!t.m->ext && read_guard && !read_guard(t.abs)) return FAT32_ERR_PERMISSION;
    if (!t.base[0]) return FATV_NOT_FOUND;
    out->mount = t.m;
    out->pos.index = out->pos.cluster = 0;
    if (t.m->ext) {
        r = ext2_lookup(&t.m->ev, t.dir, t.base, &out->xent);
        if (r) return r;
        if ((out->xent.mode & EXT2_S_IFMT) == EXT2_S_IFDIR) return FATV_NOT_FOUND;
        return allowed(&out->xent, VFS_R) ? 0 : FAT32_ERR_PERMISSION;
    }
    r = fatvol_lookup(&t.m->vol, t.dir, t.base, &out->ent);
    if (r) return r;
    if (out->ent.e.attributes & FAT_ATTR_DIRECTORY) return FATV_NOT_FOUND;
    return 0;
}

static int vfs_read_at_unlocked(vfs_file_t *f, uint32_t offset, void *buf, uint32_t len) {
    mount_t *m = f->mount;
    if (!m->used) return FATV_IO;                           /* unmounted meanwhile */
    if (m->ext) return ext2_read_at(&m->ev, &f->xent, offset, buf, len);
    return fatvol_read_at(&m->vol, &f->ent, offset, buf, len, &f->pos);
}

uint32_t vfs_file_size(const vfs_file_t *f) {
    if (((mount_t *)f->mount)->ext) return f->xent.size > 0xFFFFFFF0u ? 0xFFFFFFF0u : (uint32_t)f->xent.size;
    return f->ent.e.file_size;
}

/* ---- Owners, permissions, names -------------------------------------------------------- */

/* The file or folder at `path` (ext2), the mount's root included. */
static int ext_find(target_t *t, ext2ent_t *f) {
    if (!t->base[0]) return ext2_stat(&t->m->ev, EXT2_ROOT_INO, f);
    return ext2_lookup(&t->m->ev, t->dir, t->base, f);
}

static int vfs_stat_unlocked(const char *path, vfs_stat_t *out) {
    target_t t;
    int r = resolve(path, 1, &t);
    if (r) return r;
    memset(out, 0, sizeof(*out));
    mount_t *at = t.base[0] ? mount_at(t.abs) : 0;          /* a volume mounted here */
    if (at || !t.base[0]) {
        mount_t *m = at ? at : t.m;
        if (m->ext) {
            ext2ent_t f;
            r = ext2_stat(&m->ev, EXT2_ROOT_INO, &f);
            if (r) return r;
            out->mode = f.mode & 07777;
            out->uid = f.uid;
            out->gid = f.gid;
            out->mtime = f.mtime;
            out->owners = 1;
        } else {
            out->mode = 0755;
        }
        out->is_dir = 1;
        return 0;
    }
    if (t.m->ext) {
        ext2ent_t f;
        r = ext2_lookup(&t.m->ev, t.dir, t.base, &f);
        if (r) return r;
        out->is_dir = (f.mode & EXT2_S_IFMT) == EXT2_S_IFDIR;
        out->size = out->is_dir ? 0 : f.size > 0xFFFFFFF0u ? 0xFFFFFFF0u : (uint32_t)f.size;
        out->mode = f.mode & 07777;
        out->uid = f.uid;
        out->gid = f.gid;
        out->mtime = f.mtime;
        out->owners = 1;
        return 0;
    }
    fatent_t f;
    r = fatvol_lookup(&t.m->vol, t.dir, t.base, &f);
    if (r) return r;
    out->is_dir = (f.e.attributes & FAT_ATTR_DIRECTORY) != 0;
    out->size = out->is_dir ? 0 : f.e.file_size;
    out->mode = out->is_dir ? 0755 : 0644;
    return 0;
}

static int vfs_chmod_unlocked(const char *path, uint32_t mode) {
    target_t t;
    int r = resolve(path, 1, &t);
    if (r) return r;
    if (!t.m->ext) return VFS_ERR_NO_OWNERS;
    ext2ent_t f;
    r = ext_find(&t, &f);
    if (r) return r;
    uint32_t uid, gid;
    if (!who(&uid, &gid) && uid != f.uid) return FAT32_ERR_PERMISSION;
    return ext2_setattr(&t.m->ev, &f, (int32_t)(mode & 07777), -1, -1);
}

static int vfs_chown_unlocked(const char *path, int64_t uid, int64_t gid) {
    target_t t;
    int r = resolve(path, 1, &t);
    if (r) return r;
    if (!t.m->ext) return VFS_ERR_NO_OWNERS;
    ext2ent_t f;
    r = ext_find(&t, &f);
    if (r) return r;
    uint32_t me, my_gid;
    if (!who(&me, &my_gid)) return FAT32_ERR_PERMISSION;   /* root only */
    return ext2_setattr(&t.m->ev, &f, -1, uid, gid);
}

static int vfs_rename_unlocked(const char *from, const char *to) {
    target_t *a = kmalloc(2 * sizeof(target_t)), *b = a + 1;
    if (!a) return FATV_IO;
    int r = resolve(from, 1, a);
    if (!r) r = resolve(to, 1, b);
    if (!r && (!a->base[0] || !b->base[0] || mount_at(a->abs) || mount_at(b->abs))) r = FATV_BAD_NAME;
    if (!r && a->m != b->m) r = VFS_ERR_CROSS;
    if (!r && !a->m->ext) r = VFS_ERR_NO_OWNERS;
    ext2ent_t f, there;
    if (!r) r = ext2_lookup(&a->m->ev, a->dir, a->base, &f);
    if (!r && !may_remove(&a->folder, &f)) r = FAT32_ERR_PERMISSION;
    if (!r && !allowed(&b->folder, VFS_W | VFS_X)) r = FAT32_ERR_PERMISSION;
    if (!r && ext2_lookup(&a->m->ev, b->dir, b->base, &there) == 0 && there.ino != f.ino &&
        !may_remove(&b->folder, &there))
        r = FAT32_ERR_PERMISSION;
    if (!r && (f.mode & EXT2_S_IFMT) == EXT2_S_IFDIR && a->dir != b->dir && !allowed(&f, VFS_W))
        r = FAT32_ERR_PERMISSION;                           /* (its ".." changes) */
    if (!r) r = ext2_rename(&a->m->ev, &f, b->dir, b->base);
    kfree(a);
    return r;
}

static int vfs_access_unlocked(const char *path, int want) {
    target_t t;
    if (resolve(path, 1, &t) != 0) return 0;
    if (!t.m->ext) {
        if ((want & VFS_W) && write_guard && !write_guard(t.abs)) return 0;
        if ((want & VFS_R) && read_guard && !read_guard(t.abs)) return 0;
        return 1;
    }
    ext2ent_t f;
    if (ext_find(&t, &f) == 0) return allowed(&f, want);
    return !(want & VFS_W) || allowed(&t.folder, VFS_W | VFS_X);
}

static int vfs_space_unlocked(const char *path, uint64_t *total, uint64_t *free_bytes) {
    target_t t;
    int r = resolve(path, 1, &t);
    if (r) return r;
    mount_t *m = t.base[0] && mount_at(t.abs) ? mount_at(t.abs) : t.m;
    if (m->ext) {
        ext2_space(&m->ev, total, free_bytes);
        return 0;
    }
    *total = (uint64_t)m->vol.clusters * m->vol.bpc;
    *free_bytes = 0;                                        /* (FAT32 does not count them) */
    return 0;
}

/* ---- The lock -----------------------------------------------------------------------
 * Every call that reaches the disk holds vfs_lock: while one task waits for the
 * NVMe drive, others run, and the volume code (its caches, a folder read block
 * by block, a transaction) must not be entered twice at once. */

kmutex_t vfs_lock;

#define LOCKED(type, call) { kmutex_lock(&vfs_lock); type r_ = call; kmutex_unlock(&vfs_lock); return r_; }

int vfs_mount(const char *path, const blockdev_t *dev) { LOCKED(int, vfs_mount_unlocked(path, dev)); }
int vfs_unmount(const char *path) { LOCKED(int, vfs_unmount_unlocked(path)); }
int vfs_read_sectors(uint64_t lba, uint32_t count, void *buf) { LOCKED(int, vfs_read_sectors_unlocked(lba, count, buf)); }
int fat32_read_file(const char *path, uint8_t *buffer, uint32_t max_size) {
    LOCKED(int, fat32_read_file_unlocked(path, buffer, max_size));
}
int fat32_file_exists(const char *path) { LOCKED(int, fat32_file_exists_unlocked(path)); }
uint32_t fat32_get_file_size(const char *path) { LOCKED(uint32_t, fat32_get_file_size_unlocked(path)); }
int fat32_write_file(const char *path, const uint8_t *buffer, uint32_t size) {
    LOCKED(int, fat32_write_file_unlocked(path, buffer, size));
}
int fat32_create_file(const char *path) { LOCKED(int, fat32_create_file_unlocked(path)); }
int fat32_mkdir(const char *path) { LOCKED(int, fat32_mkdir_unlocked(path)); }
int fat32_delete_file(const char *path) { LOCKED(int, fat32_delete_file_unlocked(path)); }
int fat32_change_directory(const char *path) { LOCKED(int, fat32_change_directory_unlocked(path)); }
int vfs_folder(const char *path, char *out, int size) { LOCKED(int, vfs_folder_unlocked(path, out, size)); }
int fat32_list_directory_ex(const char *path, fat32_file_info_t *files, uint32_t max_files) {
    LOCKED(int, fat32_list_directory_ex_unlocked(path, files, max_files));
}
int vfs_open_file(const char *path, vfs_file_t *out) { LOCKED(int, vfs_open_file_unlocked(path, out)); }
int vfs_read_at(vfs_file_t *f, uint32_t offset, void *buf, uint32_t len) {
    LOCKED(int, vfs_read_at_unlocked(f, offset, buf, len));
}
int vfs_stat(const char *path, vfs_stat_t *out) { LOCKED(int, vfs_stat_unlocked(path, out)); }
int vfs_chmod(const char *path, uint32_t mode) { LOCKED(int, vfs_chmod_unlocked(path, mode)); }
int vfs_chown(const char *path, int64_t uid, int64_t gid) { LOCKED(int, vfs_chown_unlocked(path, uid, gid)); }
int vfs_rename(const char *from, const char *to) { LOCKED(int, vfs_rename_unlocked(from, to)); }
int vfs_access(const char *path, int want) { LOCKED(int, vfs_access_unlocked(path, want)); }
int vfs_space(const char *path, uint64_t *total, uint64_t *free_bytes) {
    LOCKED(int, vfs_space_unlocked(path, total, free_bytes));
}

void vfs_shutdown(void) {
    kmutex_lock(&vfs_lock);
    for (int i = 0; i < VFS_MAX_MOUNTS; i++)
        if (mounts[i].used && mounts[i].ext) ext2_mark_clean(&mounts[i].ev);
    kmutex_unlock(&vfs_lock);
}

void vfs_crash_after_commit(void) {
    kmutex_lock(&vfs_lock);
    for (int i = 0; i < VFS_MAX_MOUNTS; i++)
        if (mounts[i].used && mounts[i].ext) mounts[i].ev.crash_after_commit = 1;
    kmutex_unlock(&vfs_lock);
}
