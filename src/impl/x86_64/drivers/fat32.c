// fat32.c - FAT32 volumes (fatvol.h): the FAT, clusters, folders and files
//
// One fatvol_t per mounted volume, each with its own block device. The FAT
// is read one sector at a time through a one-sector cache (writes go
// straight through to every FAT copy). Folder entries are updated where they
// are, in whichever cluster of the folder holds them.
#include "drivers/fatvol.h"
#include "drivers/heap.h"
#include "drivers/rtc.h"
#include "lib/string.h"

#define EOC        0x0FFFFFF8u
#define END_MARK   0x0FFFFFFFu
#define ATTR_LFN   0x0F

/* ---- Device, FAT, clusters --------------------------------------------------- */

static int dev_read(fatvol_t *v, uint32_t lba, uint32_t n, void *buf) {
    return v->dev.read(v->dev.ctx, lba, n, buf) == 0 ? 0 : FATV_IO;
}

static int dev_write(fatvol_t *v, uint32_t lba, uint32_t n, const void *buf) {
    if (!v->dev.writable) return FATV_READ_ONLY;
    return v->dev.write(v->dev.ctx, lba, n, buf) == 0 ? 0 : FATV_IO;
}

static int fat_load(fatvol_t *v, uint32_t sector) {
    if (v->cache_valid && v->cache_sector == sector) return 0;
    v->cache_valid = 0;
    if (dev_read(v, sector, 1, v->cache) != 0) return FATV_IO;
    v->cache_sector = sector;
    v->cache_valid = 1;
    return 0;
}

static int valid_cluster(fatvol_t *v, uint32_t c) { return c >= 2 && c < v->clusters + 2; }

static uint32_t fat_get(fatvol_t *v, uint32_t c) {
    if (!valid_cluster(v, c)) return END_MARK;
    uint32_t off = c * 4;
    if (fat_load(v, v->fat_start + off / 512) != 0) return END_MARK;
    return *(uint32_t *)(v->cache + off % 512) & 0x0FFFFFFF;
}

static int fat_set(fatvol_t *v, uint32_t c, uint32_t value) {
    uint32_t off = c * 4, sector = v->fat_start + off / 512;
    int r = fat_load(v, sector);
    if (r) return r;
    uint32_t *e = (uint32_t *)(v->cache + off % 512);
    *e = (*e & 0xF0000000u) | (value & 0x0FFFFFFF);
    for (uint32_t i = 0; i < v->num_fats; i++) {
        r = dev_write(v, sector + i * v->fat_size, 1, v->cache);
        if (r) {
            v->cache_valid = 0;                             /* the cache no longer matches the disk */
            return r;
        }
    }
    return 0;
}

static uint32_t cluster_lba(fatvol_t *v, uint32_t c) { return v->data_start + (c - 2) * v->spc; }

static int read_cluster(fatvol_t *v, uint32_t c, void *buf) {
    return valid_cluster(v, c) ? dev_read(v, cluster_lba(v, c), v->spc, buf) : FATV_IO;
}

static int write_cluster(fatvol_t *v, uint32_t c, const void *buf) {
    return valid_cluster(v, c) ? dev_write(v, cluster_lba(v, c), v->spc, buf) : FATV_IO;
}

/* A free cluster, marked as the end of a chain. 0 if the volume is full. */
static uint32_t alloc_cluster(fatvol_t *v) {
    for (uint32_t i = 0; i < v->clusters; i++) {
        uint32_t c = 2 + (v->alloc_hint - 2 + i) % v->clusters;
        if (fat_get(v, c) == 0) {
            if (fat_set(v, c, END_MARK) != 0) return 0;
            v->alloc_hint = c + 1 < v->clusters + 2 ? c + 1 : 2;
            return c;
        }
    }
    return 0;
}

static void free_chain(fatvol_t *v, uint32_t c) {
    uint32_t steps = v->clusters;                           /* a broken (cyclic) chain ends too */
    while (valid_cluster(v, c) && steps--) {
        uint32_t next = fat_get(v, c);
        fat_set(v, c, 0);
        c = next;
    }
}

int fatvol_open(fatvol_t *v, const blockdev_t *dev) {
    memset(v, 0, sizeof(*v));
    v->dev = *dev;
    uint8_t s[512];
    if (dev_read(v, 0, 1, s) != 0) return FATV_IO;
    fat32_boot_sector_t *b = (fat32_boot_sector_t *)s;
    uint8_t spc = b->sectors_per_cluster;
    if (s[510] != 0x55 || s[511] != 0xAA || b->bytes_per_sector != 512 || !spc || (spc & (spc - 1)) ||
        !b->num_fats || !b->fat_size_32 || b->root_cluster < 2)
        return FATV_BAD_NAME;
    v->spc = spc;
    v->bpc = spc * 512u;
    v->fat_start = b->reserved_sectors;
    v->fat_size = b->fat_size_32;
    v->num_fats = b->num_fats;
    v->data_start = v->fat_start + v->num_fats * v->fat_size;
    uint32_t total = b->total_sectors_32 ? b->total_sectors_32 : b->total_sectors_16;
    if (total <= v->data_start) return FATV_BAD_NAME;
    v->clusters = (total - v->data_start) / spc;
    v->root = b->root_cluster;
    v->media = b->media_type;
    v->alloc_hint = 2;
    memcpy(v->label, b->volume_label, 11);
    v->label[11] = 0;
    for (int i = 10; i >= 0 && v->label[i] == ' '; i--) v->label[i] = 0;
    /* FAT[0] and FAT[1] are markers, not cluster chains (old builds zeroed them). */
    if (v->dev.writable) {
        if (fat_get(v, 0) != (0x0FFFFF00u | v->media)) fat_set(v, 0, 0x0FFFFF00u | v->media);
        if (fat_get(v, 1) != END_MARK) fat_set(v, 1, END_MARK);
    }
    return 0;
}

/* ---- Names and entries --------------------------------------------------------- */

int fatvol_name(const char *s, uint8_t out[11]) {
    memset(out, ' ', 11);
    if (!s[0]) return FATV_BAD_NAME;
    int i = 0, ext = 0;
    for (const char *p = s; *p; p++) {
        char c = *p;
        if (c == '.') {
            if (ext || i == 0) return FATV_BAD_NAME;
            ext = 1;
            i = 8;
            continue;
        }
        if (c >= 'a' && c <= 'z') c = (char)(c - 32);
        if ((uint8_t)c <= ' ' || (uint8_t)c >= 127) return FATV_BAD_NAME;
        for (const char *bad = "\"*+,/:;<=>?[\\]|"; *bad; bad++)
            if (c == *bad) return FATV_BAD_NAME;
        if ((!ext && i >= 8) || (ext && i >= 11)) return FATV_BAD_NAME;   /* 8.3 names only */
        out[i++] = (uint8_t)c;
    }
    if (ext && i == 8) return FATV_BAD_NAME;                /* "NAME." */
    return 0;
}

void fatvol_name_string(const uint8_t name[11], char *out) {
    int j = 0;
    for (int i = 0; i < 8 && name[i] != ' '; i++) out[j++] = (char)name[i];
    if (name[8] != ' ') {
        out[j++] = '.';
        for (int i = 8; i < 11 && name[i] != ' '; i++) out[j++] = (char)name[i];
    }
    out[j] = 0;
}

uint32_t fatent_cluster(const fatent_t *f) {
    return (uint32_t)f->e.first_cluster_high << 16 | f->e.first_cluster_low;
}

static void set_cluster(fatent_t *f, uint32_t c) {
    f->e.first_cluster_high = (uint16_t)(c >> 16);
    f->e.first_cluster_low = (uint16_t)c;
}

static void stamp(fat32_dir_entry_t *e, int created) {
    rtc_time_t t;
    uint16_t date = 0, time = 0;
    if (rtc_read(&t) == 0) {
        rtc_add_minutes(&t, RTC_LOCAL_OFFSET_MIN);
        if (t.year >= 1980) {
            date = (uint16_t)((t.year - 1980) << 9 | t.month << 5 | t.day);
            time = (uint16_t)(t.hour << 11 | t.minute << 5 | t.second / 2);
        }
    }
    e->last_mod_date = e->last_access_date = date;
    e->last_mod_time = time;
    if (created) {
        e->creation_date = date;
        e->creation_time = time;
    }
}

static int entry_store(fatvol_t *v, const fatent_t *f) {
    uint8_t *buf = kmalloc(v->bpc);
    if (!buf) return FATV_IO;
    int r = read_cluster(v, f->slot_cluster, buf);
    if (!r) {
        ((fat32_dir_entry_t *)buf)[f->slot_index] = f->e;
        r = write_cluster(v, f->slot_cluster, buf);
    }
    kfree(buf);
    return r;
}

static int skip_entry(const fat32_dir_entry_t *e) {
    return e->name[0] == 0xE5 || (e->attributes & 0x3F) == ATTR_LFN || (e->attributes & FAT_ATTR_VOLUME_ID);
}

/* ---- Folders --------------------------------------------------------------------- */

int fatvol_lookup(fatvol_t *v, uint32_t dir, const char *name, fatent_t *out) {
    uint8_t want[11];
    if (fatvol_name(name, want) != 0) return FATV_BAD_NAME;
    uint8_t *buf = kmalloc(v->bpc);
    if (!buf) return FATV_IO;
    uint32_t per = v->bpc / 32, steps = v->clusters;
    for (uint32_t c = dir; valid_cluster(v, c) && steps--; c = fat_get(v, c)) {
        if (read_cluster(v, c, buf) != 0) break;
        fat32_dir_entry_t *e = (fat32_dir_entry_t *)buf;
        for (uint32_t i = 0; i < per; i++) {
            if (e[i].name[0] == 0) goto missing;
            if (skip_entry(&e[i]) || memcmp(e[i].name, want, 11) != 0) continue;
            out->e = e[i];
            out->slot_cluster = c;
            out->slot_index = i;
            kfree(buf);
            return 0;
        }
    }
missing:
    kfree(buf);
    return FATV_NOT_FOUND;
}

int fatvol_list(fatvol_t *v, uint32_t dir, fat32_file_info_t *out, int max) {
    uint8_t *buf = kmalloc(v->bpc);
    if (!buf) return FATV_IO;
    int n = 0;
    uint32_t per = v->bpc / 32, steps = v->clusters;
    for (uint32_t c = dir; valid_cluster(v, c) && steps-- && n < max; c = fat_get(v, c)) {
        if (read_cluster(v, c, buf) != 0) break;
        fat32_dir_entry_t *e = (fat32_dir_entry_t *)buf;
        for (uint32_t i = 0; i < per && n < max; i++) {
            if (e[i].name[0] == 0) goto done;
            if (skip_entry(&e[i])) continue;
            fatvol_name_string(e[i].name, out[n].name);
            out[n].size = e[i].file_size;
            out[n].first_cluster = (uint32_t)e[i].first_cluster_high << 16 | e[i].first_cluster_low;
            out[n].attributes = e[i].attributes;
            out[n].is_directory = (e[i].attributes & FAT_ATTR_DIRECTORY) != 0;
            n++;
        }
    }
done:
    kfree(buf);
    return n;
}

/* A free entry in `dir`, growing the folder by a cluster if it is full. */
static int free_slot(fatvol_t *v, uint32_t dir, uint8_t *buf, uint32_t *slot_cluster, uint32_t *slot_index) {
    uint32_t per = v->bpc / 32, last = dir, steps = v->clusters;
    for (uint32_t c = dir; valid_cluster(v, c) && steps--; c = fat_get(v, c)) {
        if (read_cluster(v, c, buf) != 0) return FATV_IO;
        fat32_dir_entry_t *e = (fat32_dir_entry_t *)buf;
        for (uint32_t i = 0; i < per; i++)
            if (e[i].name[0] == 0 || e[i].name[0] == 0xE5) {
                *slot_cluster = c;
                *slot_index = i;
                return 0;
            }
        last = c;
    }
    uint32_t fresh = alloc_cluster(v);
    if (!fresh) return FATV_FULL;
    memset(buf, 0, v->bpc);
    if (write_cluster(v, fresh, buf) != 0 || fat_set(v, last, fresh) != 0) {
        fat_set(v, fresh, 0);
        return FATV_IO;
    }
    *slot_cluster = fresh;
    *slot_index = 0;
    return 0;
}

int fatvol_create(fatvol_t *v, uint32_t dir, const char *name, int is_dir, fatent_t *out) {
    uint8_t fn[11];
    if (fatvol_name(name, fn) != 0) return FATV_BAD_NAME;
    if (!v->dev.writable) return FATV_READ_ONLY;
    fatent_t existing;
    if (fatvol_lookup(v, dir, name, &existing) == 0) return FATV_EXISTS;
    uint8_t *buf = kmalloc(v->bpc);
    if (!buf) return FATV_IO;

    fat32_dir_entry_t e;
    memset(&e, 0, sizeof(e));
    memcpy(e.name, fn, 11);
    e.attributes = is_dir ? FAT_ATTR_DIRECTORY : FAT_ATTR_ARCHIVE;
    stamp(&e, 1);
    uint32_t sub = 0;
    if (is_dir) {                                           /* its first cluster, with "." and ".." */
        sub = alloc_cluster(v);
        if (!sub) {
            kfree(buf);
            return FATV_FULL;
        }
        memset(buf, 0, v->bpc);
        fat32_dir_entry_t *d = (fat32_dir_entry_t *)buf;
        d[0] = e;
        memset(d[0].name, ' ', 11);
        d[0].name[0] = '.';
        d[0].first_cluster_high = (uint16_t)(sub >> 16);
        d[0].first_cluster_low = (uint16_t)sub;
        d[1] = d[0];
        d[1].name[1] = '.';
        uint32_t parent = dir == v->root ? 0 : dir;         /* ".." of a top folder is 0 */
        d[1].first_cluster_high = (uint16_t)(parent >> 16);
        d[1].first_cluster_low = (uint16_t)parent;
        if (write_cluster(v, sub, buf) != 0) {
            fat_set(v, sub, 0);
            kfree(buf);
            return FATV_IO;
        }
        e.first_cluster_high = (uint16_t)(sub >> 16);
        e.first_cluster_low = (uint16_t)sub;
    }
    uint32_t sc, si;
    int r = free_slot(v, dir, buf, &sc, &si);
    if (!r) {
        ((fat32_dir_entry_t *)buf)[si] = e;
        r = write_cluster(v, sc, buf);
    }
    kfree(buf);
    if (r) {
        if (sub) fat_set(v, sub, 0);
        return r;
    }
    if (out) {
        out->e = e;
        out->slot_cluster = sc;
        out->slot_index = si;
    }
    return 0;
}

/* ---- Files ------------------------------------------------------------------------ */

int fatvol_read(fatvol_t *v, const fatent_t *f, uint8_t *dst, uint32_t max) {
    uint32_t size = f->e.file_size < max ? f->e.file_size : max, done = 0;
    if (!size) return 0;
    uint8_t *buf = kmalloc(v->bpc);
    if (!buf) return FATV_IO;
    uint32_t steps = v->clusters;
    for (uint32_t c = fatent_cluster(f); done < size && valid_cluster(v, c) && steps--; c = fat_get(v, c)) {
        if (read_cluster(v, c, buf) != 0) {
            kfree(buf);
            return FATV_IO;
        }
        uint32_t n = size - done < v->bpc ? size - done : v->bpc;
        memcpy(dst + done, buf, n);
        done += n;
    }
    kfree(buf);
    return (int)done;
}

int fatvol_write(fatvol_t *v, fatent_t *f, const uint8_t *data, uint32_t size) {
    if (!v->dev.writable) return FATV_READ_ONLY;
    if (f->e.attributes & FAT_ATTR_DIRECTORY) return FATV_EXISTS;
    uint32_t first = fatent_cluster(f);
    if (!valid_cluster(v, first)) first = 0;
    uint32_t need = (size + v->bpc - 1) / v->bpc, written = 0;
    int r = 0;
    if (!need) {
        if (first) free_chain(v, first);
        first = 0;
    } else {
        uint8_t *buf = kmalloc(v->bpc);
        if (!buf) return FATV_IO;
        uint32_t prev = 0, cur = first;
        for (uint32_t k = 0; k < need; k++) {
            if (!valid_cluster(v, cur)) {                   /* the chain is too short: grow it */
                cur = alloc_cluster(v);
                if (!cur) { r = FATV_FULL; break; }
                if (prev) fat_set(v, prev, cur);
                else first = cur;
            }
            uint32_t n = size - written < v->bpc ? size - written : v->bpc;
            memcpy(buf, data + written, n);
            if (n < v->bpc) memset(buf + n, 0, v->bpc - n);
            if ((r = write_cluster(v, cur, buf)) != 0) break;
            written += n;
            uint32_t next = fat_get(v, cur);
            if (k == need - 1 && next < EOC) {              /* the chain is too long: cut it */
                fat_set(v, cur, END_MARK);
                free_chain(v, next);
            }
            prev = cur;
            cur = next;
        }
        kfree(buf);
    }
    set_cluster(f, first);
    f->e.file_size = written;
    stamp(&f->e, 0);
    int s = entry_store(v, f);
    return r ? r : s ? s : (int)written;
}

int fatvol_delete(fatvol_t *v, fatent_t *f) {
    if (!v->dev.writable) return FATV_READ_ONLY;
    uint32_t c = fatent_cluster(f);
    if (f->e.attributes & FAT_ATTR_DIRECTORY) {             /* folders: only empty ones */
        fat32_file_info_t list[3];
        int n = fatvol_list(v, c, list, 3);
        for (int i = 0; i < n; i++)
            if (strcmp(list[i].name, ".") && strcmp(list[i].name, "..")) return FATV_NOT_EMPTY;
    }
    if (valid_cluster(v, c)) free_chain(v, c);
    f->e.name[0] = 0xE5;
    return entry_store(v, f);
}
