// fat32.c - FAT32 volumes (fatvol.h): the FAT, clusters, folders and files
//
// One fatvol_t per mounted volume, each with its own block device. The FAT
// is read one sector at a time through a one-sector cache (writes go
// straight through to every FAT copy). Folder entries are updated where they
// are, in whichever cluster of the folder holds them. Long names (VFAT) are
// read and written; see "Names" below.
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
        /* FSInfo's free-cluster count and hint are not kept up to date here:
         * mark them "unknown" (0xFFFFFFFF), so other systems count again. */
        uint16_t fsinfo = b->fs_info;
        if (fsinfo && fsinfo < b->reserved_sectors && dev_read(v, fsinfo, 1, s) == 0 &&
            !memcmp(s, "RRaA", 4) && !memcmp(s + 484, "rrAa", 4) &&
            (*(uint32_t *)(s + 488) != 0xFFFFFFFFu || *(uint32_t *)(s + 492) != 0xFFFFFFFFu)) {
            *(uint32_t *)(s + 488) = 0xFFFFFFFFu;
            *(uint32_t *)(s + 492) = 0xFFFFFFFFu;
            dev_write(v, fsinfo, 1, s);
        }
    }
    return 0;
}

/* ---- Names ------------------------------------------------------------------------ */

/* Long names: up to 255 characters, any printable ASCII except these. A name
 * that is a plain 8.3 name in one case per part ("notes.txt", "README.MD")
 * gets a short entry only (lower case through the NT case flags, as Windows
 * does); anything else gets long-name entries plus a generated 8.3 alias
 * ("MYNOTE~1.TXT"), which old systems see and which also finds the file. */
#define LFN_CHARS      13
#define LFN_MAX        255
#define CASE_LOW_BASE  0x08            /* fat32_dir_entry_t.reserved: base name in lower case */
#define CASE_LOW_EXT   0x10            /* extension in lower case */

static char upper(char c) { return c >= 'a' && c <= 'z' ? (char)(c - 32) : c; }
static char lower(char c) { return c >= 'A' && c <= 'Z' ? (char)(c + 32) : c; }

static int names_equal(const char *a, const char *b) {
    for (; *a && *b; a++, b++)
        if (upper(*a) != upper(*b)) return 0;
    return *a == *b;
}

int fatvol_valid_name(const char *s) {
    size_t n = strlen(s);
    if (n == 0 || n > LFN_MAX || !strcmp(s, ".") || !strcmp(s, "..")) return 0;
    if (s[n - 1] == '.' || s[n - 1] == ' ' || s[0] == ' ') return 0;
    for (const char *p = s; *p; p++) {
        if ((uint8_t)*p < ' ' || (uint8_t)*p >= 127) return 0;
        for (const char *bad = "\"*/:<>?\\|"; *bad; bad++)
            if (*p == *bad) return 0;
    }
    return 1;
}

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
        c = upper(c);
        if ((uint8_t)c <= ' ' || (uint8_t)c >= 127) return FATV_BAD_NAME;
        for (const char *bad = "\"*+,/:;<=>?[\\]|"; *bad; bad++)
            if (c == *bad) return FATV_BAD_NAME;
        if ((!ext && i >= 8) || (ext && i >= 11)) return FATV_BAD_NAME;   /* 8.3 names only */
        out[i++] = (uint8_t)c;
    }
    if (ext && i == 8) return FATV_BAD_NAME;                /* "NAME." */
    return 0;
}

/* The 8.3 name as text, lower-cased where the case flags say so. */
static void short_name_string(const uint8_t name[11], uint8_t flags, char *out) {
    int j = 0;
    for (int i = 0; i < 8 && name[i] != ' '; i++)
        out[j++] = (flags & CASE_LOW_BASE) ? lower((char)name[i]) : (char)name[i];
    if (name[8] != ' ') {
        out[j++] = '.';
        for (int i = 8; i < 11 && name[i] != ' '; i++)
            out[j++] = (flags & CASE_LOW_EXT) ? lower((char)name[i]) : (char)name[i];
    }
    out[j] = 0;
}

void fatvol_name_string(const uint8_t name[11], char *out) {
    short_name_string(name, 0, out);
}

/* A plain 8.3 name, one case per part: the case flags it needs, or -1. */
static int short_only(const char *s, uint8_t fn[11]) {
    if (fatvol_name(s, fn) != 0) return -1;
    int flags = 0, part = 0, has_low[2] = { 0, 0 }, has_up[2] = { 0, 0 };
    for (const char *p = s; *p; p++) {
        if (*p == '.') { part = 1; continue; }
        has_low[part] |= *p >= 'a' && *p <= 'z';
        has_up[part] |= *p >= 'A' && *p <= 'Z';
    }
    for (int k = 0; k < 2; k++) {
        if (has_low[k] && has_up[k]) return -1;             /* "MyFile": needs a long name */
        if (has_low[k]) flags |= k ? CASE_LOW_EXT : CASE_LOW_BASE;
    }
    return flags;
}

/* Allowed in long names, not in 8.3 ones. */
static int bad_in_alias(char c) {
    return c == '+' || c == ',' || c == ';' || c == '=' || c == '[' || c == ']';
}

static uint8_t lfn_checksum(const uint8_t name[11]) {
    uint8_t sum = 0;
    for (int i = 0; i < 11; i++) sum = (uint8_t)(((sum & 1) << 7) + (sum >> 1) + name[i]);
    return sum;
}

/* Where each of the 13 characters sits in a long-name entry. */
static const uint8_t lfn_offsets[LFN_CHARS] = { 1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30 };

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

/* Write one 32-byte entry into slot `index` of folder cluster `c`. */
static int slot_store(fatvol_t *v, uint32_t c, uint32_t index, const void *entry) {
    uint8_t *buf = kmalloc(v->bpc);
    if (!buf) return FATV_IO;
    int r = read_cluster(v, c, buf);
    if (!r) {
        memcpy(buf + index * 32, entry, 32);
        r = write_cluster(v, c, buf);
    }
    kfree(buf);
    return r;
}

static int entry_store(fatvol_t *v, const fatent_t *f) {
    return slot_store(v, f->slot_cluster, f->slot_index, &f->e);
}

/* The slot after (c, i) in the folder's chain; 0 at its end. */
static int next_slot(fatvol_t *v, uint32_t *c, uint32_t *i) {
    if (++*i < v->bpc / 32) return 1;
    *i = 0;
    *c = fat_get(v, *c);
    return valid_cluster(v, *c);
}

/* ---- Folders --------------------------------------------------------------------- */

/* Every entry of a folder, with its long name put together; `fn` returns 1
 * to stop (then dir_scan returns 1 too). */
typedef int (*scan_fn)(void *ctx, fatent_t *f);

static int dir_scan(fatvol_t *v, uint32_t dir, scan_fn fn, void *ctx) {
    uint8_t *buf = kmalloc(v->bpc);
    if (!buf) return FATV_IO;
    static uint16_t lfn[20 * LFN_CHARS];
    fatent_t *f = kmalloc(sizeof(*f));
    if (!f) {
        kfree(buf);
        return FATV_IO;
    }
    int expect = 0, total = 0, stop = 0;                    /* expect: the next long-name piece */
    uint8_t sum = 0;
    uint32_t lfn_c = 0, lfn_i = 0;
    uint32_t per = v->bpc / 32, steps = v->clusters;
    for (uint32_t c = dir; !stop && valid_cluster(v, c) && steps--; c = fat_get(v, c)) {
        if (read_cluster(v, c, buf) != 0) break;
        for (uint32_t i = 0; i < per; i++) {
            const uint8_t *raw = buf + i * 32;
            const fat32_dir_entry_t *e = (const fat32_dir_entry_t *)raw;
            if (raw[0] == 0) goto done;
            if (raw[0] == 0xE5) { expect = total = 0; continue; }
            if ((e->attributes & 0x3F) == ATTR_LFN) {
                int ord = raw[0] & 0x1F;
                if (raw[0] & 0x40) {                        /* the last piece comes first */
                    if (ord < 1 || ord > 20) { expect = total = 0; continue; }
                    total = ord;
                    sum = raw[13];
                    lfn_c = c;
                    lfn_i = i;
                    memset(lfn, 0, sizeof(lfn));
                } else if (!expect || ord != expect || raw[13] != sum) {
                    expect = total = 0;
                    continue;
                }
                for (int k = 0; k < LFN_CHARS; k++)
                    lfn[(ord - 1) * LFN_CHARS + k] = (uint16_t)(raw[lfn_offsets[k]] | raw[lfn_offsets[k] + 1] << 8);
                expect = ord - 1;
                continue;
            }
            int complete = total && expect == 0 && sum == lfn_checksum(e->name);
            int had_lfn = total != 0;
            total = 0;
            if (e->attributes & FAT_ATTR_VOLUME_ID) continue;
            memset(f, 0, sizeof(*f));
            f->e = *e;
            f->slot_cluster = c;
            f->slot_index = i;
            if (complete && had_lfn) {
                int n = 0;
                for (; n < LFN_MAX && lfn[n] && lfn[n] != 0xFFFF; n++)
                    f->name[n] = lfn[n] < 127 && lfn[n] >= ' ' ? (char)lfn[n] : '?';
                f->name[n] = 0;
                uint32_t pieces = 0;
                for (uint32_t cc = lfn_c, ii = lfn_i; !(cc == c && ii == i); next_slot(v, &cc, &ii)) pieces++;
                f->lfn_cluster = lfn_c;
                f->lfn_index = lfn_i;
                f->lfn_count = pieces;
            }
            if (!f->name[0]) short_name_string(e->name, e->reserved, f->name);
            expect = 0;
            if (fn(ctx, f)) {
                stop = 1;
                break;
            }
        }
    }
done:
    kfree(f);
    kfree(buf);
    return stop;
}

struct find_ctx { const char *name; fatent_t *out; };

static int find_one(void *ctx, fatent_t *f) {
    struct find_ctx *k = ctx;
    char alias[13];
    fatvol_name_string(f->e.name, alias);
    if (!names_equal(f->name, k->name) && !names_equal(alias, k->name)) return 0;
    *k->out = *f;
    return 1;
}

int fatvol_lookup(fatvol_t *v, uint32_t dir, const char *name, fatent_t *out) {
    if (!name[0] || strlen(name) > LFN_MAX) return FATV_BAD_NAME;
    struct find_ctx k = { name, out };
    int r = dir_scan(v, dir, find_one, &k);
    return r == 1 ? 0 : r < 0 ? r : FATV_NOT_FOUND;
}

struct list_ctx { fat32_file_info_t *out; int n, max; };

static int list_one(void *ctx, fatent_t *f) {
    struct list_ctx *k = ctx;
    fat32_file_info_t *o = &k->out[k->n++];
    kstrncpy(o->name, f->name, sizeof(o->name));
    o->size = f->e.file_size;
    o->first_cluster = fatent_cluster(f);
    o->attributes = f->e.attributes;
    o->is_directory = (f->e.attributes & FAT_ATTR_DIRECTORY) != 0;
    return k->n >= k->max;
}

int fatvol_list(fatvol_t *v, uint32_t dir, fat32_file_info_t *out, int max) {
    if (max <= 0) return 0;
    struct list_ctx k = { out, 0, max };
    int r = dir_scan(v, dir, list_one, &k);
    return r < 0 ? r : k.n;
}

/* Is this 8.3 name taken in `dir` (as anyone's alias)? */
struct alias_ctx { const uint8_t *fn; };

static int alias_taken(void *ctx, fatent_t *f) {
    return memcmp(f->e.name, ((struct alias_ctx *)ctx)->fn, 11) == 0;
}

/* The 8.3 alias of a long name: "My long notes.txt" -> "MYLONG~1.TXT". */
static int make_alias(fatvol_t *v, uint32_t dir, const char *name, uint8_t fn[11]) {
    const char *dot = 0;
    for (const char *p = name; *p; p++)
        if (*p == '.' && p != name) dot = p;
    char base[9], ext[4];
    int nb = 0, ne = 0;
    for (const char *p = name; *p && p != dot && nb < 8; p++) {
        char c = upper(*p);
        if (c == ' ' || c == '.') continue;
        base[nb++] = bad_in_alias(c) ? '_' : c;
    }
    for (const char *p = dot ? dot + 1 : ""; *p && ne < 3; p++) {
        char c = upper(*p);
        if (c == ' ' || c == '.') continue;
        ext[ne++] = bad_in_alias(c) ? '_' : c;
    }
    if (nb == 0) base[nb++] = '_';
    for (uint32_t n = 1; n < 1000000; n++) {
        char tail[9];
        int nt = k_snprintf(tail, sizeof(tail), "~%u", n);
        int keep = nb + nt > 8 ? 8 - nt : nb;
        memset(fn, ' ', 11);
        memcpy(fn, base, (size_t)keep);
        memcpy(fn + keep, tail, (size_t)nt);
        memcpy(fn + 8, ext, (size_t)ne);
        struct alias_ctx k = { fn };
        int r = dir_scan(v, dir, alias_taken, &k);
        if (r < 0) return r;
        if (r == 0) return 0;
    }
    return FATV_FULL;
}

/* `count` free slots in a row in `dir`, growing the folder if needed. */
static int free_run(fatvol_t *v, uint32_t dir, uint32_t count, uint32_t *run_c, uint32_t *run_i) {
    uint8_t *buf = kmalloc(v->bpc);
    if (!buf) return FATV_IO;
    uint32_t per = v->bpc / 32, last = dir, steps = v->clusters, have = 0;
    int r = 0;
    for (uint32_t c = dir; valid_cluster(v, c) && steps--; c = fat_get(v, c)) {
        if (read_cluster(v, c, buf) != 0) { r = FATV_IO; goto out; }
        for (uint32_t i = 0; i < per; i++) {
            if (buf[i * 32] == 0 || buf[i * 32] == 0xE5) {
                if (have++ == 0) { *run_c = c; *run_i = i; }
                if (have == count) goto out;
            } else {
                have = 0;
            }
        }
        last = c;
    }
    while (have < count) {                                  /* the run goes on in new clusters */
        uint32_t fresh = alloc_cluster(v);
        if (!fresh) { r = FATV_FULL; goto out; }
        memset(buf, 0, v->bpc);
        if (write_cluster(v, fresh, buf) != 0 || fat_set(v, last, fresh) != 0) {
            fat_set(v, fresh, 0);
            r = FATV_IO;
            goto out;
        }
        if (have == 0) { *run_c = fresh; *run_i = 0; }
        have += per;
        last = fresh;
    }
out:
    kfree(buf);
    return r;
}

int fatvol_create(fatvol_t *v, uint32_t dir, const char *name, int is_dir, fatent_t *out) {
    if (!fatvol_valid_name(name)) return FATV_BAD_NAME;
    if (!v->dev.writable) return FATV_READ_ONLY;
    fatent_t *existing = kmalloc(sizeof(*existing));
    if (!existing) return FATV_IO;
    int found = fatvol_lookup(v, dir, name, existing);
    kfree(existing);
    if (found == 0) return FATV_EXISTS;

    uint8_t fn[11];
    int flags = short_only(name, fn);
    size_t len = strlen(name);
    uint32_t pieces = flags >= 0 ? 0 : (uint32_t)((len + LFN_CHARS - 1) / LFN_CHARS);
    if (flags < 0) {
        int r = make_alias(v, dir, name, fn);
        if (r) return r;
    }

    fat32_dir_entry_t e;
    memset(&e, 0, sizeof(e));
    memcpy(e.name, fn, 11);
    e.reserved = flags > 0 ? (uint8_t)flags : 0;
    e.attributes = is_dir ? FAT_ATTR_DIRECTORY : FAT_ATTR_ARCHIVE;
    stamp(&e, 1);
    uint8_t *buf = kmalloc(v->bpc);
    if (!buf) return FATV_IO;
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
        d[0].reserved = 0;
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
    kfree(buf);

    uint32_t c, i;
    int r = free_run(v, dir, pieces + 1, &c, &i);
    uint32_t first_c = c, first_i = i;
    uint8_t sum = lfn_checksum(fn);
    for (uint32_t ord = pieces; !r && ord >= 1; ord--) {   /* the long name, last piece first */
        uint8_t raw[32];
        memset(raw, 0, sizeof(raw));
        raw[0] = (uint8_t)(ord | (ord == pieces ? 0x40 : 0));
        raw[11] = ATTR_LFN;
        raw[13] = sum;
        for (int k = 0; k < LFN_CHARS; k++) {
            size_t at = (ord - 1) * LFN_CHARS + (size_t)k;
            uint16_t ch = at < len ? (uint8_t)name[at] : at == len ? 0 : 0xFFFF;
            raw[lfn_offsets[k]] = (uint8_t)ch;
            raw[lfn_offsets[k] + 1] = (uint8_t)(ch >> 8);
        }
        r = slot_store(v, c, i, raw);
        if (!r && !next_slot(v, &c, &i)) r = FATV_IO;
    }
    if (!r) r = slot_store(v, c, i, &e);
    if (r) {
        if (sub) fat_set(v, sub, 0);
        return r;
    }
    if (out) {
        memset(out, 0, sizeof(*out));
        out->e = e;
        out->slot_cluster = c;
        out->slot_index = i;
        out->lfn_cluster = first_c;
        out->lfn_index = first_i;
        out->lfn_count = pieces;
        kstrncpy(out->name, name, sizeof(out->name));
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
    uint32_t lc = f->lfn_cluster, li = f->lfn_index;      /* its long name goes too */
    for (uint32_t k = 0; k < f->lfn_count; k++) {
        uint8_t raw[32];
        uint8_t *buf = kmalloc(v->bpc);
        if (!buf) return FATV_IO;
        int r = read_cluster(v, lc, buf);
        if (!r) memcpy(raw, buf + li * 32, 32);
        kfree(buf);
        if (r) return r;
        raw[0] = 0xE5;
        if ((r = slot_store(v, lc, li, raw)) != 0) return r;
        if (!next_slot(v, &lc, &li)) break;
    }
    f->e.name[0] = 0xE5;
    return entry_store(v, f);
}

/* ---- Formatting ----------------------------------------------------------------- */

/* Cluster size by volume size, as Windows chooses it for FAT32. */
static uint32_t format_cluster_sectors(uint64_t sectors) {
    if (sectors <= 532480) return 1;                        /* up to 260 MiB: 512 bytes */
    if (sectors <= 16777216) return 8;                      /* 8 GiB: 4 KiB */
    if (sectors <= 33554432) return 16;                     /* 16 GiB: 8 KiB */
    if (sectors <= 67108864) return 32;                     /* 32 GiB: 16 KiB */
    return 64;                                              /* 32 KiB */
}

static void put16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void put32(uint8_t *p, uint32_t v) { put16(p, (uint16_t)v); put16(p + 2, (uint16_t)(v >> 16)); }

int fatvol_format(const blockdev_t *dev, uint64_t hidden, const char *label) {
    if (!dev->writable) return FATV_READ_ONLY;
    uint64_t total = dev->sectors;
    if (total < 67584 || total > 0xFFFFFFFFull) return FATV_BAD_NAME;   /* 33 MiB .. 2 TiB */
    uint32_t spc = format_cluster_sectors(total), reserved = 32, fats = 2;
    /* The FAT size, as the FAT specification computes it. */
    uint64_t tmp1 = total - reserved, tmp2 = (256ull * spc + fats) / 2;
    uint32_t fat_size = (uint32_t)((tmp1 + tmp2 - 1) / tmp2);
    uint32_t data_start = reserved + fats * fat_size;
    uint32_t clusters = (uint32_t)((total - data_start) / spc);
    if (clusters < 65525) return FATV_BAD_NAME;            /* too small to be FAT32 */

    enum { CHUNK = 64 };                                    /* sectors written at a time */
    uint8_t *buf = kmalloc(CHUNK * 512);
    if (!buf) return FATV_IO;
    int r = 0;
    memset(buf, 0, CHUNK * 512);
    /* Wipe the reserved area, both FATs and the root folder's cluster, the
     * boot sector last of all (so a half-done format is not a file system). */
    uint64_t wipe_end = data_start + spc;
    for (uint64_t s = 1; s < wipe_end && !r; s += CHUNK) {
        uint32_t n = (uint32_t)(wipe_end - s < CHUNK ? wipe_end - s : CHUNK);
        r = dev->write(dev->ctx, s, n, buf);
    }
    /* FAT[0] = media, FAT[1] = end marker, FAT[2] = the root folder (one cluster). */
    for (uint32_t f = 0; f < fats && !r; f++) {
        memset(buf, 0, 512);
        put32(buf, 0x0FFFFFF8u);
        put32(buf + 4, 0x0FFFFFFFu);
        put32(buf + 8, 0x0FFFFFFFu);
        r = dev->write(dev->ctx, reserved + f * fat_size, 1, buf);
    }
    /* The root folder: just the volume label. */
    char name[11];
    memset(name, ' ', 11);
    for (int i = 0; i < 11 && label[i]; i++) name[i] = upper(label[i]);
    if (!r) {
        memset(buf, 0, 512);
        memcpy(buf, name, 11);
        buf[11] = FAT_ATTR_VOLUME_ID;
        r = dev->write(dev->ctx, data_start, 1, buf);
    }
    /* FSInfo (sector 1, backup 7): free count unknown. */
    if (!r) {
        memset(buf, 0, 512);
        put32(buf, 0x41615252u);
        put32(buf + 484, 0x61417272u);
        put32(buf + 488, 0xFFFFFFFFu);
        put32(buf + 492, 0xFFFFFFFFu);
        put32(buf + 508, 0xAA550000u);
        r = dev->write(dev->ctx, 1, 1, buf);
        if (!r) r = dev->write(dev->ctx, 7, 1, buf);
    }
    /* The boot sector (backup at 6, then the real one). */
    if (!r) {
        memset(buf, 0, 512);
        buf[0] = 0xEB; buf[1] = 0x58; buf[2] = 0x90;
        memcpy(buf + 3, "TERMOS  ", 8);
        put16(buf + 11, 512);
        buf[13] = (uint8_t)spc;
        put16(buf + 14, (uint16_t)reserved);
        buf[16] = (uint8_t)fats;
        buf[21] = 0xF8;                                     /* fixed disk */
        put16(buf + 24, 63);
        put16(buf + 26, 255);
        put32(buf + 28, (uint32_t)hidden);
        put32(buf + 32, (uint32_t)total);
        put32(buf + 36, fat_size);
        put32(buf + 44, 2);                                 /* root folder cluster */
        put16(buf + 48, 1);                                 /* FSInfo */
        put16(buf + 50, 6);                                 /* backup boot sector */
        buf[64] = 0x80;
        buf[66] = 0x29;
        put32(buf + 67, (uint32_t)(hidden * 2654435761u) ^ 0x7E57u);   /* volume serial number */
        memcpy(buf + 71, name, 11);
        memcpy(buf + 82, "FAT32   ", 8);
        buf[510] = 0x55; buf[511] = 0xAA;
        r = dev->write(dev->ctx, 6, 1, buf);
        if (!r) r = dev->write(dev->ctx, 0, 1, buf);
    }
    kfree(buf);
    return r ? FATV_IO : 0;
}
