// ext2.c - ext2/ext3 volumes (ext2.h): groups, inodes, folders, the journal
//
// Metadata (superblock, group descriptors, bitmaps, inode tables, folders,
// indirect blocks) is read through a block cache, and a change takes its
// blocks with bmod(), which marks them dirty: the dirty blocks are the
// transaction. Each public call is one transaction. finish() commits it:
// the blocks go to the journal (descriptor blocks naming them, the blocks, a
// commit block), the journal's superblock is pointed at them, then they are
// written in place and the journal is emptied again. The journal never holds
// more than that one transaction, so a block freed and reused later is never
// overwritten by a replay (no revoke records needed). A call that fails
// drops its dirty blocks: the volume is left as it was.
//
// File contents bypass the cache: written straight to newly allocated
// blocks before the commit (the old blocks are freed in the same
// transaction), and read straight from the disk.
#include "drivers/ext2.h"
#include "drivers/heap.h"
#include "drivers/rtc.h"
#include "lib/string.h"

#define MAGIC             0xEF53
#define COMPAT_JOURNAL    0x0004
#define INCOMPAT_FILETYPE 0x0002
#define INCOMPAT_RECOVER  0x0004
#define RO_SPARSE         0x0001
#define RO_LARGE_FILE     0x0002
#define RO_BTREE_DIR      0x0004
#define INDEX_FL          0x1000              /* a hashed folder (Linux's dir_index) */
#define JOURNAL_INO       8
#define NDIR              12                  /* direct block pointers in an inode */
#define FT_REG            1
#define FT_DIR            2
#define FT_LNK            7
#define CACHE_CLEAN       256                 /* clean blocks kept between calls */
#define MAX_DIRTY         2048                /* blocks in one transaction */

/* The journal (JBD, big-endian). */
#define JMAGIC            0xC03B3998u
#define JDESC             1
#define JCOMMIT           2
#define JSB_V1            3
#define JSB_V2            4
#define JREVOKE           5
#define TAG_ESCAPE        1
#define TAG_SAME_UUID     2
#define TAG_LAST          8

typedef struct {
    uint32_t inodes_count, blocks_count, r_blocks_count, free_blocks_count, free_inodes_count;
    uint32_t first_data_block, log_block_size, log_frag_size, blocks_per_group, frags_per_group;
    uint32_t inodes_per_group, mtime, wtime;
    uint16_t mnt_count, max_mnt_count, magic, state, errors, minor_rev_level;
    uint32_t lastcheck, checkinterval, creator_os, rev_level;
    uint16_t def_resuid, def_resgid;
    uint32_t first_ino;
    uint16_t inode_size, block_group_nr;
    uint32_t feature_compat, feature_incompat, feature_ro_compat;
    uint8_t uuid[16];
    char volume_name[16];
    char last_mounted[64];
    uint32_t algorithm_usage_bitmap;
    uint8_t prealloc_blocks, prealloc_dir_blocks;
    uint16_t reserved_gdt_blocks;
    uint8_t journal_uuid[16];
    uint32_t journal_inum, journal_dev, last_orphan;
    uint32_t hash_seed[4];
    uint8_t def_hash_version, jnl_backup_type;
    uint16_t desc_size;
    uint32_t default_mount_opts, first_meta_bg, mkfs_time;
    uint32_t jnl_blocks[17];
} __attribute__((packed)) super_t;

typedef struct {
    uint32_t block_bitmap, inode_bitmap, inode_table;
    uint16_t free_blocks, free_inodes, used_dirs, flags;
    uint32_t exclude_bitmap;
    uint16_t block_bitmap_csum, inode_bitmap_csum, itable_unused, checksum;
} __attribute__((packed)) group_t;

typedef struct {
    uint16_t mode, uid;
    uint32_t size, atime, ctime, mtime, dtime;
    uint16_t gid, links;
    uint32_t blocks;                          /* in 512-byte units, indirect blocks included */
    uint32_t flags, osd1;
    uint32_t block[15];
    uint32_t generation, file_acl, size_high, faddr;
    uint16_t blocks_high, file_acl_high, uid_high, gid_high, checksum_lo, reserved;
} inode_t;                                    /* (no padding: every field is aligned) */

typedef struct {
    uint32_t inode;
    uint16_t rec_len;
    uint8_t name_len, file_type;
    char name[];
} __attribute__((packed)) dirent_t;

_Static_assert(sizeof(inode_t) == 128, "inode");
_Static_assert(sizeof(group_t) == 32, "group descriptor");
_Static_assert(__builtin_offsetof(super_t, jnl_blocks) == 268, "superblock");

static super_t *super_mod(ext2vol_t *v);

/* ---- Small things ------------------------------------------------------------------ */

static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
static uint16_t be16(const uint8_t *p) { return (uint16_t)(p[0] << 8 | p[1]); }
static void put_be32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v; }
static void put_be16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }

static char lower(char c) { return c >= 'A' && c <= 'Z' ? (char)(c + 32) : c; }

static int same_nocase(const char *a, const char *b, int n) {
    for (int i = 0; i < n; i++)
        if (lower(a[i]) != lower(b[i])) return 0;
    return 1;
}

/* Seconds since 1970 (UTC), from the hardware clock. */
static uint32_t now(void) {
    static const uint16_t before[12] = { 0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334 };
    rtc_time_t t;
    if (rtc_read(&t) != 0 || t.year < 1970 || t.month < 1 || t.month > 12) return 0;
    uint32_t y = t.year, p = y - 1;
    uint32_t days = (y - 1970) * 365 + (p / 4 - 492) - (p / 100 - 19) + (p / 400 - 4);
    int leap = (y % 4 == 0 && y % 100 != 0) || y % 400 == 0;
    days += before[t.month - 1] + (t.month > 2 && leap) + t.day - 1u;
    return days * 86400u + t.hour * 3600u + t.minute * 60u + t.second;
}

/* For the volume's UUID and the journal's first sequence number: they only
 * need to differ from the last file system made here. */
static void random_fill(uint8_t *out, int n) {
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    uint64_t x = ((uint64_t)hi << 32 | lo) ^ (uint64_t)now() * 0x9E3779B97F4A7C15ull;
    for (int i = 0; i < n; i++) {
        x ^= x << 13;
        x ^= x >> 7;
        x ^= x << 17;
        out[i] = (uint8_t)(x >> 29);
    }
}

static int valid_name(const char *name) {
    size_t n = strlen(name);
    if (n == 0 || n > 255 || !strcmp(name, ".") || !strcmp(name, "..")) return 0;
    for (const char *c = name; *c; c++)
        if (*c == '/' || (unsigned char)*c < 0x20) return 0;
    return 1;
}

/* ---- The device ------------------------------------------------------------------- */

static int dev_read(ext2vol_t *v, uint32_t blk, uint32_t n, void *buf) {
    if ((uint64_t)blk + n > v->blocks) return FATV_IO;
    return v->dev.read(v->dev.ctx, (uint64_t)blk * v->spb, n * v->spb, buf) == 0 ? 0 : FATV_IO;
}

static int dev_write(ext2vol_t *v, uint32_t blk, uint32_t n, const void *buf) {
    if (v->readonly) return FATV_READ_ONLY;
    if ((uint64_t)blk + n > v->blocks) return FATV_IO;
    return v->dev.write(v->dev.ctx, (uint64_t)blk * v->spb, n * v->spb, buf) == 0 ? 0 : FATV_IO;
}

static void dev_flush(ext2vol_t *v) {
    if (v->dev.flush) v->dev.flush(v->dev.ctx);
}

/* ---- The block cache and the transaction ---------------------------------------------- */

static ext2buf_t *cfind(ext2vol_t *v, uint32_t blk) {
    for (int i = 0; i < v->ncache; i++)
        if (v->cache[i].blk == blk) return &v->cache[i];
    return 0;
}

static ext2buf_t *cadd(ext2vol_t *v, uint32_t blk) {
    if (v->ncache == v->capcache) {
        int cap = v->capcache ? v->capcache * 2 : 64;
        ext2buf_t *n = kmalloc((uint64_t)cap * sizeof(*n));
        if (!n) return 0;
        if (v->ncache) memcpy(n, v->cache, (size_t)v->ncache * sizeof(*n));
        kfree(v->cache);
        v->cache = n;
        v->capcache = cap;
    }
    uint8_t *d = kmalloc(v->bs);
    if (!d) return 0;
    ext2buf_t *b = &v->cache[v->ncache++];
    b->blk = blk;
    b->dirty = 0;
    b->used = ++v->tick;
    b->data = d;
    return b;
}

static void cdrop(ext2vol_t *v, int i) {
    kfree(v->cache[i].data);
    v->cache[i] = v->cache[--v->ncache];
}

/* A block, read through the cache. Pointers stay valid until the call ends. */
static uint8_t *bget(ext2vol_t *v, uint32_t blk) {
    if (blk >= v->blocks) return 0;
    ext2buf_t *b = cfind(v, blk);
    if (b) {
        b->used = ++v->tick;
        return b->data;
    }
    b = cadd(v, blk);
    if (!b) return 0;
    if (dev_read(v, blk, 1, b->data) != 0) {
        cdrop(v, (int)(b - v->cache));
        return 0;
    }
    return b->data;
}

static int mark(ext2vol_t *v, ext2buf_t *b) {
    if (b->dirty) return 0;
    if (v->ndirty >= v->maxdirty) return FATV_FULL;
    b->dirty = 1;
    v->ndirty++;
    return 0;
}

/* A block about to be changed: taken into the transaction first, so a call
 * that fails later leaves no changed block behind. 0 on error (fail()). */
static uint8_t *bmod(ext2vol_t *v, uint32_t blk) {
    if (v->readonly) return 0;
    uint8_t *d = bget(v, blk);
    return d && mark(v, cfind(v, blk)) == 0 ? d : 0;
}

/* A block about to be filled whole (a new folder or indirect block): zeroed. */
static uint8_t *bnew(ext2vol_t *v, uint32_t blk) {
    if (v->readonly || blk >= v->blocks) return 0;
    ext2buf_t *b = cfind(v, blk);
    if (!b && !(b = cadd(v, blk))) return 0;
    if (mark(v, b) != 0) return 0;
    memset(b->data, 0, v->bs);
    return b->data;
}

/* Why bmod/bnew said no. */
static int fail(ext2vol_t *v) {
    return v->readonly ? FATV_READ_ONLY : v->ndirty >= v->maxdirty ? FATV_FULL : FATV_IO;
}

/* Why balloc/ialloc found nothing: the volume is full, or the transaction. */
static int no_space(ext2vol_t *v) { return v->readonly ? FATV_READ_ONLY : FATV_FULL; }

/* A freed block is not metadata any more: forget it unless the transaction has it. */
static void cforget(ext2vol_t *v, uint32_t blk) {
    ext2buf_t *b = cfind(v, blk);
    if (b && !b->dirty) cdrop(v, (int)(b - v->cache));
}

static void abort_tx(ext2vol_t *v) {
    for (int i = 0; i < v->ncache;) {
        if (v->cache[i].dirty) cdrop(v, i);
        else i++;
    }
    v->ndirty = 0;
}

/* Keep the most recently used clean blocks. */
static void trim(ext2vol_t *v) {
    while (v->ncache - v->ndirty > CACHE_CLEAN) {
        int old = -1;
        for (int i = 0; i < v->ncache; i++)
            if (!v->cache[i].dirty && (old < 0 || v->cache[i].used < v->cache[old].used)) old = i;
        cdrop(v, old);
    }
}

static uint32_t jnext(ext2vol_t *v, uint32_t j) {
    return j + 1 < v->jlen ? j + 1 : be32(v->jsb + 20);     /* wraps to s_first */
}

static int jwrite(ext2vol_t *v, uint32_t j, const void *buf) { return dev_write(v, v->jmap[j], 1, buf); }

/* The journal's superblock: `start` 0 = empty, else where the live transaction begins. */
static int jsb_write(ext2vol_t *v, uint32_t start) {
    put_be32(v->jsb + 24, v->jseq);
    put_be32(v->jsb + 28, start);
    int r = jwrite(v, 0, v->jsb);
    dev_flush(v);
    return r;
}

/* Single-block writes gathered into one device write while they follow each
 * other on the disk (a USB stick pays for every command). */
#define RUN_MAX 64

typedef struct {
    ext2vol_t *v;
    uint8_t *buf;
    uint32_t first, n;
    int r;
} run_t;

static int run_open(run_t *w, ext2vol_t *v) {
    w->v = v;
    w->n = 0;
    w->r = 0;
    w->buf = kmalloc((uint64_t)RUN_MAX * v->bs);
    return w->buf ? 0 : FATV_IO;
}

static void run_flush(run_t *w) {
    if (w->n && !w->r) w->r = dev_write(w->v, w->first, w->n, w->buf);
    w->n = 0;
}

/* Queue `data` for block `blk`: the copy it will be written from. */
static uint8_t *run_add(run_t *w, uint32_t blk, const uint8_t *data) {
    if (w->n && (blk != w->first + w->n || w->n == RUN_MAX)) run_flush(w);
    if (!w->n) w->first = blk;
    uint8_t *slot = w->buf + (uint64_t)w->n++ * w->v->bs;
    memcpy(slot, data, w->v->bs);
    return slot;
}

static int run_close(run_t *w) {
    run_flush(w);
    kfree(w->buf);
    return w->r;
}

static void jheader(uint8_t *b, uint32_t type, uint32_t seq) {
    put_be32(b, JMAGIC);
    put_be32(b + 4, type);
    put_be32(b + 8, seq);
}

/* The transaction into the journal, made live. */
static int journal_write(ext2vol_t *v, ext2buf_t **d, int n) {
    uint32_t per = (v->bs - 12 - 16) / 8;                    /* tags in a descriptor block */
    if ((uint32_t)n + (n + per - 1) / per + 2 > v->jlen) return FATV_FULL;
    uint8_t *buf = kmalloc(v->bs);
    run_t w;
    if (!buf || run_open(&w, v) != 0) {
        kfree(buf);
        return FATV_IO;
    }
    uint32_t j = be32(v->jsb + 20);
    int r = 0;
    for (int i = 0; i < n; i += (int)per) {
        int k = n - i < (int)per ? n - i : (int)per;
        memset(buf, 0, v->bs);
        jheader(buf, JDESC, v->jseq);
        uint8_t *p = buf + 12;
        for (int t = 0; t < k; t++) {
            uint16_t flags = t ? TAG_SAME_UUID : 0;
            if (be32(d[i + t]->data) == JMAGIC) flags |= TAG_ESCAPE;
            if (t == k - 1) flags |= TAG_LAST;
            put_be32(p, d[i + t]->blk);
            put_be16(p + 6, flags);
            p += 8;
            if (!t) {
                memcpy(p, v->jsb + 48, 16);
                p += 16;
            }
        }
        run_add(&w, v->jmap[j], buf);
        j = jnext(v, j);
        for (int t = 0; t < k; t++) {
            uint8_t *copy = run_add(&w, v->jmap[j], d[i + t]->data);
            if (be32(copy) == JMAGIC) memset(copy, 0, 4);    /* escaped: a replay puts the magic back */
            j = jnext(v, j);
        }
    }
    memset(buf, 0, v->bs);
    jheader(buf, JCOMMIT, v->jseq);
    run_add(&w, v->jmap[j], buf);
    r = run_close(&w);
    kfree(buf);
    dev_flush(v);                                            /* file contents and the journal, then: */
    return r ? r : jsb_write(v, be32(v->jsb + 20));
}

static int commit(ext2vol_t *v) {
    if (!v->ndirty) return 0;
    if (v->clean) {                                          /* in use again after ext2_mark_clean */
        super_t *s = super_mod(v);
        if (!s) {
            abort_tx(v);
            return FATV_IO;
        }
        if (v->jlen) s->feature_incompat |= INCOMPAT_RECOVER;
        v->clean = 0;
    }
    ext2buf_t **d = kmalloc((uint64_t)v->ndirty * sizeof(*d));
    if (!d) {
        abort_tx(v);
        return FATV_IO;
    }
    int n = 0;
    for (int i = 0; i < v->ncache; i++)
        if (v->cache[i].dirty) d[n++] = &v->cache[i];
    int r = 0;
    if (v->jlen) {
        r = journal_write(v, d, n);
        if (r == 0 && v->crash_after_commit) {               /* `crash journal`: as if the power went now */
            v->readonly = 1;
            r = FATV_IO;
        }
        if (r) {
            kfree(d);
            abort_tx(v);
            return r;
        }
    }
    for (int i = 1; i < n; i++)                              /* in disk order, neighbours in one write */
        for (int k = i; k > 0 && d[k - 1]->blk > d[k]->blk; k--) {
            ext2buf_t *t = d[k];
            d[k] = d[k - 1];
            d[k - 1] = t;
        }
    run_t w;
    if (run_open(&w, v) != 0) r = FATV_IO;
    for (int i = 0; i < n && !r; i++) run_add(&w, d[i]->blk, d[i]->data);
    if (!r) r = run_close(&w);
    dev_flush(v);
    for (int i = 0; i < n; i++) d[i]->dirty = 0;
    v->ndirty = 0;
    kfree(d);
    if (r) {
        v->readonly = 1;                                     /* the journal still has it: replayed at the next mount */
        return r;
    }
    if (v->jlen) {
        v->jseq++;
        r = jsb_write(v, 0);
    }
    return r;
}

/* The end of every public call: commit what it changed, or drop it. */
static int finish(ext2vol_t *v, int r) {
    if (r >= 0) {
        int c = commit(v);
        if (c) r = c;
    } else {
        abort_tx(v);
    }
    trim(v);
    return r;
}

/* ---- Superblock, groups, inodes ------------------------------------------------------ */

static super_t *super(ext2vol_t *v) {
    uint8_t *p = bget(v, v->sb_blk);
    return p ? (super_t *)(p + v->sb_off) : 0;
}

static super_t *super_mod(ext2vol_t *v) {
    uint8_t *p = bmod(v, v->sb_blk);
    return p ? (super_t *)(p + v->sb_off) : 0;
}

static uint32_t group_blk(ext2vol_t *v, uint32_t g) { return v->first_data + 1 + g / (v->bs / 32); }

static group_t *group(ext2vol_t *v, uint32_t g) {
    uint8_t *p = bget(v, group_blk(v, g));
    return p ? (group_t *)(p + (g % (v->bs / 32)) * 32) : 0;
}

static group_t *group_mod(ext2vol_t *v, uint32_t g) {
    uint8_t *p = bmod(v, group_blk(v, g));
    return p ? (group_t *)(p + (g % (v->bs / 32)) * 32) : 0;
}

/* Blocks in group g (the last one may be short). */
static uint32_t group_blocks(ext2vol_t *v, uint32_t g) {
    uint32_t start = v->first_data + g * v->bpg;
    return v->blocks - start < v->bpg ? v->blocks - start : v->bpg;
}

static uint32_t inode_blk(ext2vol_t *v, uint32_t ino, uint32_t *off) {
    if (!ino || ino > v->inodes) return 0;
    group_t *gd = group(v, (ino - 1) / v->ipg);
    if (!gd) return 0;
    uint32_t o = (ino - 1) % v->ipg * v->isize;
    *off = o % v->bs;
    return gd->inode_table + o / v->bs;
}

static inode_t *inode(ext2vol_t *v, uint32_t ino) {
    uint32_t off, b = inode_blk(v, ino, &off);
    uint8_t *p = b ? bget(v, b) : 0;
    return p ? (inode_t *)(p + off) : 0;
}

static inode_t *inode_mod(ext2vol_t *v, uint32_t ino) {
    uint32_t off, b = inode_blk(v, ino, &off);
    uint8_t *p = b ? bmod(v, b) : 0;
    return p ? (inode_t *)(p + off) : 0;
}

static int is_dir(const inode_t *in) { return (in->mode & EXT2_S_IFMT) == EXT2_S_IFDIR; }

static uint64_t isize_of(const inode_t *in) {
    return in->size | ((in->mode & EXT2_S_IFMT) == EXT2_S_IFREG ? (uint64_t)in->size_high << 32 : 0);
}

static void fill(uint32_t ino, const inode_t *in, ext2ent_t *e) {
    e->ino = ino;
    e->mode = in->mode;
    e->links = in->links;
    e->uid = in->uid | (uint32_t)in->uid_high << 16;
    e->gid = in->gid | (uint32_t)in->gid_high << 16;
    e->size = isize_of(in);
    e->mtime = in->mtime;
}

static uint8_t file_type(uint16_t mode) {
    switch (mode & EXT2_S_IFMT) {
        case EXT2_S_IFDIR: return FT_DIR;
        case EXT2_S_IFLNK: return FT_LNK;
        default: return FT_REG;
    }
}

/* ---- Allocation ---------------------------------------------------------------------- */

#define NONE 0xFFFFFFFFu

static uint32_t find_zero(const uint8_t *bm, uint32_t from, uint32_t to) {
    for (uint32_t i = from; i < to; i++) {
        if ((i & 7) == 0 && i + 8 <= to && bm[i >> 3] == 0xFF) {
            i += 7;
            continue;
        }
        if (!(bm[i >> 3] & (1 << (i & 7)))) return i;
    }
    return NONE;
}

/* A free block, near `goal` if possible. 0 if the volume is full (or on error). */
static uint32_t balloc(ext2vol_t *v, uint32_t goal) {
    if (goal < v->first_data || goal >= v->blocks) goal = v->first_data + v->goal_group * v->bpg;
    uint32_t g0 = (goal - v->first_data) / v->bpg;
    for (uint32_t k = 0; k < v->groups; k++) {
        uint32_t g = (g0 + k) % v->groups;
        group_t *gd = group(v, g);
        if (!gd) return 0;
        if (!gd->free_blocks) continue;
        uint8_t *bm = bget(v, gd->block_bitmap);
        if (!bm) return 0;
        uint32_t n = group_blocks(v, g), start = k ? 0 : (goal - v->first_data) % v->bpg;
        uint32_t bit = find_zero(bm, start, n);
        if (bit == NONE) bit = find_zero(bm, 0, start);
        if (bit == NONE) continue;                           /* the count was wrong */
        uint8_t *mb = bmod(v, gd->block_bitmap);
        group_t *mg = group_mod(v, g);
        super_t *s = super_mod(v);
        if (!mb || !mg || !s) return 0;
        mb[bit >> 3] |= (uint8_t)(1 << (bit & 7));
        mg->free_blocks--;
        s->free_blocks_count--;
        v->goal_group = g;
        return v->first_data + g * v->bpg + bit;
    }
    return 0;
}

static int bfree(ext2vol_t *v, uint32_t b) {
    if (b < v->first_data || b >= v->blocks) return FATV_IO;
    uint32_t g = (b - v->first_data) / v->bpg, bit = (b - v->first_data) % v->bpg;
    group_t *gd = group(v, g);
    if (!gd) return FATV_IO;
    uint8_t *mb = bmod(v, gd->block_bitmap);
    group_t *mg = group_mod(v, g);
    super_t *s = super_mod(v);
    if (!mb || !mg || !s) return fail(v);
    if (mb[bit >> 3] & (1 << (bit & 7))) {
        mb[bit >> 3] &= (uint8_t)~(1 << (bit & 7));
        mg->free_blocks++;
        s->free_blocks_count++;
    }
    cforget(v, b);
    return 0;
}

static uint32_t ialloc(ext2vol_t *v, uint32_t near, int dir) {
    uint32_t g0 = near ? (near - 1) / v->ipg : 0;
    for (uint32_t k = 0; k < v->groups; k++) {
        uint32_t g = (g0 + k) % v->groups;
        group_t *gd = group(v, g);
        if (!gd) return 0;
        if (!gd->free_inodes) continue;
        uint8_t *bm = bget(v, gd->inode_bitmap);
        if (!bm) return 0;
        uint32_t bit = find_zero(bm, g ? 0 : v->first_ino - 1, v->ipg);
        if (bit == NONE) continue;
        uint8_t *mb = bmod(v, gd->inode_bitmap);
        group_t *mg = group_mod(v, g);
        super_t *s = super_mod(v);
        if (!mb || !mg || !s) return 0;
        mb[bit >> 3] |= (uint8_t)(1 << (bit & 7));
        mg->free_inodes--;
        if (dir) mg->used_dirs++;
        s->free_inodes_count--;
        return g * v->ipg + bit + 1;
    }
    return 0;
}

static int ifree(ext2vol_t *v, uint32_t ino, int dir) {
    uint32_t g = (ino - 1) / v->ipg, bit = (ino - 1) % v->ipg;
    group_t *gd = group(v, g);
    if (!gd) return FATV_IO;
    uint8_t *mb = bmod(v, gd->inode_bitmap);
    group_t *mg = group_mod(v, g);
    super_t *s = super_mod(v);
    if (!mb || !mg || !s) return fail(v);
    if (mb[bit >> 3] & (1 << (bit & 7))) {
        mb[bit >> 3] &= (uint8_t)~(1 << (bit & 7));
        mg->free_inodes++;
        if (dir && mg->used_dirs) mg->used_dirs--;
        s->free_inodes_count++;
    }
    return 0;
}

/* ---- Block maps ---------------------------------------------------------------------- */

static uint32_t ind(ext2vol_t *v, uint32_t blk, uint32_t i) {
    if (!blk) return 0;
    uint32_t *p = (uint32_t *)bget(v, blk);
    return p ? p[i] : 0;
}

/* The disk block holding block `lb` of a file: 0 for a hole. */
static uint32_t bmap(ext2vol_t *v, const uint32_t *blk, uint32_t lb) {
    uint32_t per = v->bs / 4;
    if (lb < NDIR) return blk[lb];
    lb -= NDIR;
    if (lb < per) return ind(v, blk[12], lb);
    lb -= per;
    if (lb < per * per) return ind(v, ind(v, blk[13], lb / per), lb % per);
    lb -= per * per;
    return ind(v, ind(v, ind(v, blk[14], lb / (per * per)), lb / per % per), lb % per);
}

/* Point block `lb` of the tree in blk[15] at `pb`, allocating indirect blocks
 * (counted in *meta) as needed. blk[] must be in a block taken with bmod
 * (an inode) or a copy of its own. */
static int set_map(ext2vol_t *v, uint32_t *blk, uint32_t lb, uint32_t pb, uint32_t *meta) {
    uint32_t per = v->bs / 4, idx[3];
    int depth;
    uint32_t *slot;
    if (lb < NDIR) {
        blk[lb] = pb;
        return 0;
    }
    lb -= NDIR;
    if (lb < per) {
        slot = &blk[12];
        depth = 1;
        idx[0] = lb;
    } else if ((lb -= per) < per * per) {
        slot = &blk[13];
        depth = 2;
        idx[0] = lb / per;
        idx[1] = lb % per;
    } else {
        lb -= per * per;
        slot = &blk[14];
        depth = 3;
        idx[0] = lb / (per * per);
        idx[1] = lb / per % per;
        idx[2] = lb % per;
    }
    uint32_t holder = 0;                                     /* the indirect block holding *slot */
    for (int d = 0;; d++) {
        if (d == depth) {
            *slot = pb;
            return 0;
        }
        if (!*slot) {
            uint32_t nb = balloc(v, pb);
            if (!nb) return no_space(v);
            if (!bnew(v, nb)) return fail(v);
            *slot = nb;
            (*meta)++;
        }
        holder = *slot;
        uint32_t *p = (uint32_t *)bmod(v, holder);
        if (!p) return fail(v);
        slot = &p[idx[d]];
    }
}

static int free_ind(ext2vol_t *v, uint32_t b, int depth) {
    if (!b) return 0;
    uint32_t *p = (uint32_t *)bget(v, b);
    if (!p) return FATV_IO;
    int r = 0;
    for (uint32_t i = 0; i < v->bs / 4 && !r; i++)
        if (p[i]) r = depth > 1 ? free_ind(v, p[i], depth - 1) : bfree(v, p[i]);
    return r ? r : bfree(v, b);
}

static int free_tree(ext2vol_t *v, const uint32_t *blk) {
    int r = 0;
    for (int i = 0; i < NDIR && !r; i++)
        if (blk[i]) r = bfree(v, blk[i]);
    for (int d = 1; d <= 3 && !r; d++) r = free_ind(v, blk[11 + d], d);
    return r;
}

/* ---- Folders ------------------------------------------------------------------------- */

typedef int (*dir_fn)(ext2vol_t *v, dirent_t *e, dirent_t *prev, uint32_t pb, void *ctx);

/* Each entry of folder `dir` (live or not) until fn returns non-zero. */
static int dir_scan(ext2vol_t *v, uint32_t dir, dir_fn fn, void *ctx) {
    inode_t *in = inode(v, dir);
    if (!in) return FATV_IO;
    if (!is_dir(in)) return FATV_NOT_FOUND;
    uint32_t nb = in->size / v->bs;
    for (uint32_t lb = 0; lb < nb; lb++) {
        uint32_t pb = bmap(v, in->block, lb);
        if (!pb) continue;
        uint8_t *b = bget(v, pb);
        if (!b) return FATV_IO;
        dirent_t *prev = 0;
        for (uint32_t off = 0; off + 8 <= v->bs;) {
            dirent_t *e = (dirent_t *)(b + off);
            if (e->rec_len < 8 || (e->rec_len & 3) || off + e->rec_len > v->bs ||
                (e->inode && 8u + e->name_len > e->rec_len))
                break;                                       /* damaged: the rest of this block is skipped */
            int r = fn(v, e, prev, pb, ctx);
            if (r) return r;
            prev = e;
            off += e->rec_len;
        }
    }
    return 0;
}

struct find_ctx {
    const char *name;
    int len, exact;
    uint32_t ino;
    char found[256];
};

static int find_one(ext2vol_t *v, dirent_t *e, dirent_t *prev, uint32_t pb, void *ctx) {
    (void)v; (void)prev; (void)pb;
    struct find_ctx *k = ctx;
    if (!e->inode || e->name_len != k->len) return 0;
    int exact = !memcmp(e->name, k->name, (size_t)k->len);
    if (!exact && (k->ino || !same_nocase(e->name, k->name, k->len))) return 0;
    k->ino = e->inode;
    memcpy(k->found, e->name, (size_t)k->len);
    k->found[k->len] = 0;
    return exact;                                            /* an exact match ends the search */
}

static uint32_t find(ext2vol_t *v, uint32_t dir, const char *name, char *spelled, int *err) {
    struct find_ctx k;
    k.name = name;
    k.len = (int)strlen(name);
    k.ino = 0;
    *err = 0;
    if (k.len > 255) return 0;
    int r = dir_scan(v, dir, find_one, &k);
    if (r < 0) *err = r;
    if (k.ino && spelled) memcpy(spelled, k.found, (size_t)k.len + 1);
    return k.ino;
}

static int add_entry(ext2vol_t *v, uint32_t dir, const char *name, uint32_t ino, uint8_t ft) {
    super_t *s = super(v);
    inode_t *in = inode(v, dir);
    if (!s || !in) return FATV_IO;
    if (!(s->feature_incompat & INCOMPAT_FILETYPE)) ft = 0;
    uint32_t len = (uint32_t)strlen(name), need = (8 + len + 3) & ~3u;
    uint32_t nb = in->size / v->bs;
    for (uint32_t lb = 0; lb < nb; lb++) {
        uint32_t pb = bmap(v, in->block, lb);
        uint8_t *b = pb ? bget(v, pb) : 0;
        for (uint32_t off = 0; b && off + 8 <= v->bs;) {
            dirent_t *e = (dirent_t *)(b + off);
            if (e->rec_len < 8 || (e->rec_len & 3) || off + e->rec_len > v->bs) break;
            uint32_t used = e->inode ? (8 + e->name_len + 3) & ~3u : 0;
            if (e->rec_len >= used + need) {
                if (!bmod(v, pb)) return fail(v);
                if (used) {
                    dirent_t *n = (dirent_t *)((uint8_t *)e + used);
                    n->rec_len = (uint16_t)(e->rec_len - used);
                    e->rec_len = (uint16_t)used;
                    e = n;
                }
                e->inode = ino;
                e->name_len = (uint8_t)len;
                e->file_type = ft;
                memcpy(e->name, name, len);
                goto touch;
            }
            off += e->rec_len;
        }
    }
    {                                                        /* no room: one more block */
        uint32_t last = nb ? bmap(v, in->block, nb - 1) : 0;
        uint32_t pb = balloc(v, last ? last + 1 : v->first_data + (dir - 1) / v->ipg * v->bpg);
        if (!pb) return FATV_FULL;
        dirent_t *e = (dirent_t *)bnew(v, pb);
        inode_t *mi = inode_mod(v, dir);
        if (!e || !mi) return fail(v);
        e->inode = ino;
        e->rec_len = (uint16_t)v->bs;
        e->name_len = (uint8_t)len;
        e->file_type = ft;
        memcpy(e->name, name, len);
        uint32_t meta = 0;
        int r = set_map(v, mi->block, nb, pb, &meta);
        if (r) return r;
        mi->size += v->bs;
        mi->blocks += (1 + meta) * (v->bs / 512);
    }
touch:;
    inode_t *mi = inode_mod(v, dir);
    if (!mi) return fail(v);
    mi->mtime = mi->ctime = now();
    mi->flags &= ~INDEX_FL;                                  /* (its hash index would be stale) */
    return 0;
}

struct remove_ctx {
    uint32_t ino;
    const char *name;
    int len, pass;
    int done;
};

static int remove_one(ext2vol_t *v, dirent_t *e, dirent_t *prev, uint32_t pb, void *ctx) {
    struct remove_ctx *k = ctx;
    if (e->inode != k->ino || e->name_len != k->len) return 0;
    if (k->pass == 0 ? memcmp(e->name, k->name, (size_t)k->len) != 0 : !same_nocase(e->name, k->name, k->len))
        return 0;
    if (!bmod(v, pb)) return fail(v);
    if (prev) prev->rec_len = (uint16_t)(prev->rec_len + e->rec_len);
    else e->inode = 0;
    k->done = 1;
    return 1;
}

static int remove_entry(ext2vol_t *v, uint32_t dir, uint32_t ino, const char *name) {
    struct remove_ctx k = { ino, name, (int)strlen(name), 0, 0 };
    int r = dir_scan(v, dir, remove_one, &k);
    if (r == 0 && !k.done) {                                 /* the spelling differs: any case */
        k.pass = 1;
        r = dir_scan(v, dir, remove_one, &k);
    }
    if (r < 0) return r;
    if (!k.done) return FATV_NOT_FOUND;
    inode_t *mi = inode_mod(v, dir);
    if (!mi) return fail(v);
    mi->mtime = mi->ctime = now();
    mi->flags &= ~INDEX_FL;
    return 0;
}

static int count_one(ext2vol_t *v, dirent_t *e, dirent_t *prev, uint32_t pb, void *ctx) {
    (void)v; (void)prev; (void)pb;
    if (!e->inode) return 0;
    if ((e->name_len == 1 && e->name[0] == '.') || (e->name_len == 2 && e->name[0] == '.' && e->name[1] == '.'))
        return 0;
    (*(int *)ctx)++;
    return 1;
}

static int dir_empty(ext2vol_t *v, uint32_t dir) {
    int n = 0;
    int r = dir_scan(v, dir, count_one, &n);
    return r < 0 ? r : n == 0;
}

/* Drop one link to `ino` (its name is gone); with none left, free it. */
static int unlink_inode(ext2vol_t *v, uint32_t ino) {
    inode_t *in = inode_mod(v, ino);
    if (!in) return fail(v);
    int dir = is_dir(in);
    if (dir || in->links <= 1) in->links = 0;
    else in->links--;
    in->ctime = now();
    if (in->links) return 0;
    uint32_t blk[15];
    memcpy(blk, in->block, sizeof(blk));
    int fast_link = (in->mode & EXT2_S_IFMT) == EXT2_S_IFLNK && in->blocks == 0;
    int r = fast_link ? 0 : free_tree(v, blk);
    if (r) return r;
    memset(in, 0, sizeof(*in));
    in->dtime = now();
    return ifree(v, ino, dir);
}

/* ---- The journal at mount ------------------------------------------------------------ */

/* Write the committed transactions of the journal to their places. */
static int replay(ext2vol_t *v) {
    uint32_t first = be32(v->jsb + 20), seq = be32(v->jsb + 24), j = be32(v->jsb + 28);
    if (first < 1 || first >= v->jlen || j < first || j >= v->jlen) return FATV_BAD_NAME;
    uint32_t *home = kmalloc((uint64_t)v->jlen * 4), *src = kmalloc((uint64_t)v->jlen * 4);
    uint8_t *esc = kmalloc(v->jlen), *b = kmalloc(v->bs);
    int r = 0, n = 0, from = 0;
    if (!home || !src || !esc || !b) r = FATV_IO;
    for (uint32_t steps = 0; !r && steps < v->jlen; steps++) {
        if (dev_read(v, v->jmap[j], 1, b) != 0) {
            r = FATV_IO;
            break;
        }
        if (be32(b) != JMAGIC || be32(b + 8) != seq) break;
        uint32_t type = be32(b + 4);
        if (type == JDESC) {
            uint32_t pos = jnext(v, j);
            for (uint8_t *p = b + 12; p + 8 <= b + v->bs && n < (int)v->jlen;) {
                uint16_t flags = be16(p + 6);
                home[n] = be32(p);
                src[n] = pos;
                esc[n] = flags & TAG_ESCAPE;
                n++;
                pos = jnext(v, pos);
                p += 8 + (flags & TAG_SAME_UUID ? 0 : 16);
                if (flags & TAG_LAST) break;
            }
            j = pos;
        } else if (type == JCOMMIT) {                        /* complete: in place with it */
            for (int i = from; i < n && !r; i++) {
                if (home[i] >= v->blocks || dev_read(v, v->jmap[src[i]], 1, b) != 0) {
                    r = FATV_IO;
                    break;
                }
                if (esc[i]) put_be32(b, JMAGIC);
                r = dev_write(v, home[i], 1, b);
            }
            from = n;
            seq++;
            v->replayed++;
            j = jnext(v, j);
        } else {
            if (type == JREVOKE) r = FATV_BAD_NAME;          /* Linux's: e2fsck must do this one */
            break;
        }
    }
    kfree(home);
    kfree(src);
    kfree(esc);
    kfree(b);
    if (r) return r;
    dev_flush(v);
    v->jseq = seq;
    return jsb_write(v, 0);
}

/* The journal inode's blocks, its superblock checked. */
static int journal_load(ext2vol_t *v, uint32_t ino) {
    inode_t *in = inode(v, ino);
    if (!in || (in->mode & EXT2_S_IFMT) != EXT2_S_IFREG) return FATV_BAD_NAME;
    uint32_t blk[15];
    memcpy(blk, in->block, sizeof(blk));
    uint64_t len = isize_of(in) / v->bs;
    if (len < 64 || len > 0x100000) return FATV_BAD_NAME;
    v->jmap = kmalloc(len * 4);
    v->jsb = kmalloc(v->bs);
    if (!v->jmap || !v->jsb) return FATV_IO;
    for (uint32_t i = 0; i < len; i++) {
        v->jmap[i] = bmap(v, blk, i);
        if (!v->jmap[i] || v->jmap[i] >= v->blocks) return FATV_BAD_NAME;
        if ((i & 255) == 255) trim(v);
    }
    v->jlen = (uint32_t)len;
    if (dev_read(v, v->jmap[0], 1, v->jsb) != 0) return FATV_IO;
    uint32_t type = be32(v->jsb + 4), maxlen = be32(v->jsb + 16);
    if (be32(v->jsb) != JMAGIC || (type != JSB_V1 && type != JSB_V2) || be32(v->jsb + 12) != v->bs ||
        maxlen < 64 || maxlen > v->jlen)
        return FATV_BAD_NAME;
    if (type == JSB_V2 && (be32(v->jsb + 40) || be32(v->jsb + 44))) return FATV_BAD_NAME;   /* checksums, 64-bit */
    v->jlen = maxlen;
    v->jseq = be32(v->jsb + 24);
    return 0;
}

/* ---- Mount ----------------------------------------------------------------------------- */

int ext2_probe(const uint8_t *raw, char label[17]) {
    const super_t *s = (const super_t *)raw;
    if (s->magic != MAGIC || s->log_block_size > 6) return 0;
    if (label) {
        memcpy(label, s->volume_name, 16);
        label[16] = 0;
    }
    return 1;
}

int ext2_open(ext2vol_t *v, const blockdev_t *dev) {
    memset(v, 0, sizeof(*v));
    v->dev = *dev;
    uint8_t raw[1024];
    if (dev->sectors < 4 || dev->read(dev->ctx, 2, 2, raw) != 0) return FATV_IO;
    super_t *s = (super_t *)raw;
    if (s->magic != MAGIC || s->log_block_size > 2) return FATV_BAD_NAME;
    v->bs = 1024u << s->log_block_size;
    v->spb = v->bs / 512;
    v->isize = s->rev_level ? s->inode_size : 128;
    v->first_ino = s->rev_level ? s->first_ino : 11;
    v->blocks = s->blocks_count;
    v->bpg = s->blocks_per_group;
    v->ipg = s->inodes_per_group;
    v->inodes = s->inodes_count;
    v->first_data = s->first_data_block;
    uint32_t incompat = s->rev_level ? s->feature_incompat : 0, ro = s->rev_level ? s->feature_ro_compat : 0;
    if (v->isize < 128 || v->isize > v->bs || (v->isize & (v->isize - 1)) || !v->bpg || v->bpg > v->bs * 8 ||
        !v->ipg || v->ipg > v->bs * 8 || v->first_ino < 11 || v->blocks <= v->first_data ||
        (uint64_t)v->blocks * v->spb > dev->sectors)
        return FATV_BAD_NAME;
    if (incompat & ~(uint32_t)(INCOMPAT_FILETYPE | INCOMPAT_RECOVER)) return FATV_BAD_NAME;  /* ext4 */
    v->groups = (v->blocks - v->first_data + v->bpg - 1) / v->bpg;
    if ((uint64_t)v->groups * v->ipg < v->inodes || v->groups * 32 > 64 * v->bs) return FATV_BAD_NAME;
    v->sb_blk = 1024 / v->bs;
    v->sb_off = 1024 % v->bs;
    memcpy(v->label, s->volume_name, 16);
    v->label[16] = 0;
    v->readonly = !dev->writable || (ro & ~(uint32_t)(RO_SPARSE | RO_LARGE_FILE | RO_BTREE_DIR));
    v->maxdirty = MAX_DIRTY;
    int journal = s->rev_level && (s->feature_compat & COMPAT_JOURNAL);
    uint32_t jino = s->journal_inum;
    int r = 0;
    if (journal) {
        if (!jino || s->journal_dev) r = FATV_BAD_NAME;    /* on another device */
        else r = journal_load(v, jino);
        if (r == 0 && be32(v->jsb + 28)) {                   /* a crash left a transaction in it */
            if (v->readonly) r = FATV_READ_ONLY;
            else r = replay(v);
            for (int i = v->ncache - 1; i >= 0; i--) cdrop(v, i);
        }
        uint32_t per = (v->bs - 28) / 8;
        if (r == 0 && v->jlen - 4 - v->jlen / per < (uint32_t)v->maxdirty)
            v->maxdirty = (int)(v->jlen - 4 - v->jlen / per);
    }
    if (r == 0 && !v->readonly) {                            /* in use: a crash from now on needs the journal */
        super_t *m = super(v);
        uint8_t *b = bget(v, v->sb_blk);
        if (!m || !b) r = FATV_IO;
        else {
            if (journal) m->feature_incompat |= INCOMPAT_RECOVER;
            m->mnt_count++;
            m->mtime = now();
            r = dev_write(v, v->sb_blk, 1, b);
            dev_flush(v);
        }
    }
    trim(v);
    if (r) {
        v->readonly = 1;                                     /* (nothing is written: not mounted) */
        ext2_close(v);
        return r;
    }
    return 0;
}

void ext2_mark_clean(ext2vol_t *v) {
    abort_tx(v);
    if (v->readonly || !v->bs || v->clean) return;
    super_t *m = super(v);                                   /* nothing to recover */
    uint8_t *b = bget(v, v->sb_blk);
    if (m && b) {
        m->feature_incompat &= ~(uint32_t)INCOMPAT_RECOVER;
        m->wtime = now();
        dev_write(v, v->sb_blk, 1, b);
        dev_flush(v);
        v->clean = 1;
    }
    trim(v);
}

void ext2_close(ext2vol_t *v) {
    ext2_mark_clean(v);
    for (int i = v->ncache - 1; i >= 0; i--) cdrop(v, i);
    kfree(v->cache);
    kfree(v->jmap);
    kfree(v->jsb);
    v->cache = 0;
    v->jmap = 0;
    v->jsb = 0;
    v->ncache = v->capcache = 0;
}

/* ---- Files and folders -------------------------------------------------------------------- */

int ext2_stat(ext2vol_t *v, uint32_t ino, ext2ent_t *out) {
    inode_t *in = inode(v, ino);
    int r = in ? 0 : FATV_IO;
    if (in) {
        memset(out, 0, sizeof(*out));
        fill(ino, in, out);
    }
    return finish(v, r);
}

int ext2_lookup(ext2vol_t *v, uint32_t dir, const char *name, ext2ent_t *out) {
    int err;
    uint32_t ino = find(v, dir, name, out->name, &err);
    int r = err ? err : ino ? 0 : FATV_NOT_FOUND;
    inode_t *in = r ? 0 : inode(v, ino);
    if (!r && !in) r = FATV_IO;
    if (!r) {
        fill(ino, in, out);
        out->dir = dir;
    }
    return finish(v, r);
}

struct list_ctx {
    fat32_file_info_t *out;
    int n, max;
};

static int list_one(ext2vol_t *v, dirent_t *e, dirent_t *prev, uint32_t pb, void *ctx) {
    (void)prev; (void)pb;
    struct list_ctx *k = ctx;
    if (!e->inode || (e->name_len == 1 && e->name[0] == '.') ||
        (e->name_len == 2 && e->name[0] == '.' && e->name[1] == '.'))
        return 0;
    inode_t *in = inode(v, e->inode);
    if (!in) return 0;
    fat32_file_info_t *o = &k->out[k->n++];
    memset(o, 0, sizeof(*o));
    memcpy(o->name, e->name, e->name_len);
    o->name[e->name_len] = 0;
    uint64_t size = isize_of(in);
    o->size = size > 0xFFFFFFFFu ? 0xFFFFFFFFu : (uint32_t)size;
    o->first_cluster = e->inode;
    o->is_directory = (uint8_t)is_dir(in);
    o->attributes = o->is_directory ? FAT_ATTR_DIRECTORY : 0;
    o->mode = in->mode;
    o->uid = in->uid | (uint32_t)in->uid_high << 16;
    o->gid = in->gid | (uint32_t)in->gid_high << 16;
    o->mtime = in->mtime;
    if (o->is_directory) o->size = 0;
    return k->n >= k->max;
}

int ext2_list(ext2vol_t *v, uint32_t dir, fat32_file_info_t *out, int max) {
    if (max <= 0) return 0;
    struct list_ctx k = { out, 0, max };
    int r = dir_scan(v, dir, list_one, &k);
    r = finish(v, r < 0 ? r : 0);
    return r < 0 ? r : k.n;
}

int ext2_read_at(ext2vol_t *v, const ext2ent_t *f, uint32_t offset, uint8_t *buf, uint32_t len) {
    inode_t *in = inode(v, f->ino);
    if (!in) return finish(v, FATV_IO);
    uint64_t size = isize_of(in);
    if (offset >= size) return finish(v, 0);
    if (len > size - offset) len = (uint32_t)(size - offset);
    uint32_t blk[15];
    memcpy(blk, in->block, sizeof(blk));
    uint8_t *tmp = 0;
    uint32_t done = 0;
    int r = 0;
    while (done < len && !r) {
        uint64_t at = (uint64_t)offset + done;
        uint32_t lb = (uint32_t)(at / v->bs), boff = (uint32_t)(at % v->bs);
        uint32_t pb = bmap(v, blk, lb);
        if (boff == 0 && len - done >= v->bs) {              /* whole blocks: as many in a row as there are */
            uint32_t n = 1;
            while (pb && n < 256 && len - done >= (n + 1) * v->bs && bmap(v, blk, lb + n) == pb + n) n++;
            if (pb) r = dev_read(v, pb, n, buf + done);
            else memset(buf + done, 0, v->bs);
            done += n * v->bs;
            continue;
        }
        if (!tmp && !(tmp = kmalloc(v->bs))) {
            r = FATV_IO;
            break;
        }
        if (pb) r = dev_read(v, pb, 1, tmp);
        else memset(tmp, 0, v->bs);
        uint32_t n = v->bs - boff < len - done ? v->bs - boff : len - done;
        memcpy(buf + done, tmp + boff, n);
        done += n;
    }
    kfree(tmp);
    r = finish(v, r);
    return r < 0 ? r : (int)done;
}

int ext2_create(ext2vol_t *v, uint32_t dir, const char *name, uint16_t mode, uint32_t uid, uint32_t gid,
                ext2ent_t *out) {
    if (v->readonly) return FATV_READ_ONLY;
    if (!valid_name(name)) return FATV_BAD_NAME;
    int err, r = 0;
    if (find(v, dir, name, 0, &err)) return finish(v, FATV_EXISTS);
    if (err) return finish(v, err);
    int dirent = (mode & EXT2_S_IFMT) == EXT2_S_IFDIR;
    uint32_t ino = ialloc(v, dir, dirent);
    if (!ino) return finish(v, no_space(v));
    inode_t *in = inode_mod(v, ino);
    if (!in) return finish(v, fail(v));
    memset(in, 0, sizeof(*in));
    in->mode = mode;
    in->uid = (uint16_t)uid;
    in->uid_high = (uint16_t)(uid >> 16);
    in->gid = (uint16_t)gid;
    in->gid_high = (uint16_t)(gid >> 16);
    in->atime = in->ctime = in->mtime = now();
    in->links = dirent ? 2 : 1;
    if (dirent) {                                            /* "." and ".." */
        uint32_t pb = balloc(v, v->first_data + (ino - 1) / v->ipg * v->bpg);
        uint8_t *b = pb ? bnew(v, pb) : 0;
        inode_t *parent = inode_mod(v, dir);
        if (!pb) r = FATV_FULL;
        else if (!b || !parent) r = fail(v);
        else {
            dirent_t *dot = (dirent_t *)b, *up = (dirent_t *)(b + 12);
            dot->inode = ino;
            dot->rec_len = 12;
            dot->name_len = 1;
            dot->file_type = FT_DIR;
            dot->name[0] = '.';
            up->inode = dir;
            up->rec_len = (uint16_t)(v->bs - 12);
            up->name_len = 2;
            up->file_type = FT_DIR;
            up->name[0] = up->name[1] = '.';
            in->block[0] = pb;
            in->size = v->bs;
            in->blocks = v->bs / 512;
            parent->links++;
        }
    }
    if (!r) r = add_entry(v, dir, name, ino, file_type(mode));
    if (!r && out) {
        memset(out, 0, sizeof(*out));
        fill(ino, inode(v, ino), out);
        out->dir = dir;
        kstrncpy(out->name, name, sizeof(out->name));
    }
    return finish(v, r);
}

int ext2_write(ext2vol_t *v, ext2ent_t *f, const uint8_t *data, uint32_t size) {
    if (v->readonly) return FATV_READ_ONLY;
    inode_t *in = inode(v, f->ino);
    if (!in) return finish(v, FATV_IO);
    if ((in->mode & EXT2_S_IFMT) != EXT2_S_IFREG) return finish(v, FATV_EXISTS);
    uint32_t nb = (uint32_t)(((uint64_t)size + v->bs - 1) / v->bs), nblk[15], meta = 0;
    memset(nblk, 0, sizeof(nblk));
    uint32_t *pbs = nb ? kmalloc((uint64_t)nb * 4) : 0;
    uint8_t *tmp = 0;
    int r = nb && !pbs ? FATV_IO : 0;
    uint32_t goal = v->first_data + (f->ino - 1) / v->ipg * v->bpg;
    for (uint32_t i = 0; i < nb && !r; i++) {                /* new blocks (the old ones stay until the commit) */
        uint32_t pb = balloc(v, goal);
        if (!pb) {
            r = FATV_FULL;
            break;
        }
        pbs[i] = pb;
        goal = pb + 1;
        r = set_map(v, nblk, i, pb, &meta);
    }
    for (uint32_t i = 0; i < nb && !r;) {                    /* the contents, in runs of adjacent blocks */
        uint32_t j = i + 1;
        while (j < nb && pbs[j] == pbs[j - 1] + 1 && j - i < 256) j++;
        uint32_t whole = j == nb && size % v->bs ? j - i - 1 : j - i;
        if (whole) r = dev_write(v, pbs[i], whole, data + (uint64_t)i * v->bs);
        if (!r && whole < j - i) {                           /* the last block, partly filled */
            if (!tmp && !(tmp = kmalloc(v->bs))) r = FATV_IO;
            else {
                memset(tmp, 0, v->bs);
                memcpy(tmp, data + (uint64_t)(j - 1) * v->bs, size % v->bs);
                r = dev_write(v, pbs[j - 1], 1, tmp);
            }
        }
        i = j;
    }
    kfree(pbs);
    kfree(tmp);
    inode_t *mi = r ? 0 : inode_mod(v, f->ino);
    if (!r && !mi) r = fail(v);
    if (!r) {
        uint32_t old[15];
        memcpy(old, mi->block, sizeof(old));
        r = free_tree(v, old);
    }
    if (!r) {
        memcpy(mi->block, nblk, sizeof(nblk));
        mi->size = size;
        mi->size_high = 0;
        mi->blocks = (nb + meta) * (v->bs / 512);
        mi->blocks_high = 0;
        mi->mtime = mi->ctime = now();
        if (size >= 0x80000000u) {
            super_t *s = super_mod(v);
            if (s) s->feature_ro_compat |= RO_LARGE_FILE;
            else r = fail(v);
        }
        f->size = size;
        f->mtime = mi->mtime;
    }
    return finish(v, r);
}

int ext2_delete(ext2vol_t *v, ext2ent_t *f) {
    if (v->readonly) return FATV_READ_ONLY;
    inode_t *in = inode(v, f->ino);
    if (!in) return finish(v, FATV_IO);
    int dir = is_dir(in), r = 0;
    if (dir) {
        r = dir_empty(v, f->ino);
        if (r == 0) r = FATV_NOT_EMPTY;
        else if (r > 0) r = 0;
    }
    if (!r) r = remove_entry(v, f->dir, f->ino, f->name);
    if (!r && dir) {
        inode_t *parent = inode_mod(v, f->dir);
        if (!parent) r = fail(v);
        else if (parent->links > 2) parent->links--;
    }
    if (!r) r = unlink_inode(v, f->ino);
    return finish(v, r);
}

int ext2_setattr(ext2vol_t *v, ext2ent_t *f, int32_t mode, int64_t uid, int64_t gid) {
    if (v->readonly) return FATV_READ_ONLY;
    inode_t *in = inode_mod(v, f->ino);
    if (!in) return finish(v, fail(v));
    if (mode >= 0) in->mode = (uint16_t)((in->mode & EXT2_S_IFMT) | (mode & 07777));
    if (uid >= 0) {
        in->uid = (uint16_t)uid;
        in->uid_high = (uint16_t)(uid >> 16);
    }
    if (gid >= 0) {
        in->gid = (uint16_t)gid;
        in->gid_high = (uint16_t)(gid >> 16);
    }
    in->ctime = now();
    fill(f->ino, in, f);
    return finish(v, 0);
}

int ext2_rename(ext2vol_t *v, ext2ent_t *f, uint32_t dir, const char *name) {
    if (v->readonly) return FATV_READ_ONLY;
    if (!valid_name(name)) return FATV_BAD_NAME;
    inode_t *in = inode(v, f->ino), *din = inode(v, dir);
    if (!in || !din) return finish(v, FATV_IO);
    if (!is_dir(din)) return finish(v, FATV_NOT_FOUND);
    int moving_dir = is_dir(in), err, r = 0;
    char spelled[256];
    uint32_t there = find(v, dir, name, spelled, &err);
    if (err) return finish(v, err);
    if (there == f->ino && dir == f->dir && !strcmp(spelled, name)) return finish(v, 0);
    if (there && there != f->ino) {                          /* a file by that name is replaced */
        inode_t *t = inode(v, there);
        if (!t) return finish(v, FATV_IO);
        if (is_dir(t) || moving_dir) return finish(v, FATV_EXISTS);
        r = remove_entry(v, dir, there, spelled);
        if (!r) r = unlink_inode(v, there);
    }
    if (!r && moving_dir && dir != f->dir) {                 /* not into itself or a folder inside it */
        uint32_t up = dir;
        for (int depth = 0; !r && up != EXT2_ROOT_INO && depth < 4096; depth++) {
            if (up == f->ino) r = FATV_BAD_NAME;
            else up = find(v, up, "..", 0, &err);
            if (!up || err) r = r ? r : FATV_IO;
        }
    }
    if (!r) r = add_entry(v, dir, name, f->ino, file_type(in->mode));
    if (!r) r = remove_entry(v, f->dir, f->ino, f->name);
    if (!r && moving_dir && dir != f->dir) {                 /* its ".." and both parents' link counts */
        inode_t *mi = inode(v, f->ino);
        uint32_t pb = mi ? mi->block[0] : 0;
        uint8_t *b = pb ? bmod(v, pb) : 0;
        inode_t *op = inode_mod(v, f->dir), *np = inode_mod(v, dir);
        if (!b || !op || !np) r = fail(v);
        else {
            dirent_t *dot = (dirent_t *)b;
            if (dot->rec_len >= 12 && dot->rec_len + 12u <= v->bs) {
                dirent_t *dd = (dirent_t *)(b + dot->rec_len);
                if (dd->name_len == 2 && dd->name[0] == '.' && dd->name[1] == '.') dd->inode = dir;
            }
            if (op->links > 2) op->links--;
            np->links++;
        }
    }
    if (!r) {
        inode_t *mi = inode_mod(v, f->ino);
        if (!mi) r = fail(v);
        else mi->ctime = now();
    }
    if (!r) {
        f->dir = dir;
        kstrncpy(f->name, name, sizeof(f->name));
    }
    return finish(v, r);
}

void ext2_space(ext2vol_t *v, uint64_t *total, uint64_t *free_bytes) {
    super_t *s = super(v);
    *total = (uint64_t)(v->blocks - v->first_data) * v->bs;
    *free_bytes = s ? (uint64_t)s->free_blocks_count * v->bs : 0;
    finish(v, 0);
}

/* ---- mkfs ----------------------------------------------------------------------------- */

static int has_super(uint32_t g) {
    if (g <= 1) return 1;
    for (uint32_t p = 3; p <= 7; p += 2) {                   /* powers of 3, 5 and 7 */
        uint32_t x = p;
        while (x < g) x *= p;
        if (x == g) return 1;
    }
    return 0;
}

static int fmt_write(const blockdev_t *dev, uint32_t bs, uint32_t blk, uint32_t n, const void *buf) {
    return dev->write(dev->ctx, (uint64_t)blk * (bs / 512), n * (bs / 512), buf) == 0 ? 0 : FATV_IO;
}

static void set_bits(uint8_t *bm, uint32_t from, uint32_t to) {
    for (uint32_t i = from; i < to; i++) bm[i >> 3] |= (uint8_t)(1 << (i & 7));
}

int ext2_format(const blockdev_t *dev, const char *label) {
    const uint32_t bs = 4096, bpg = bs * 8, isz = 128, ratio = 128 * 1024;
    if (!dev->writable) return FATV_READ_ONLY;
    uint32_t blocks = dev->sectors / (bs / 512) > 0xFFFFFFF0u ? 0xFFFFFFF0u : (uint32_t)(dev->sectors / (bs / 512));
    if (blocks < 2048) return FATV_FULL;
    uint32_t groups = (blocks + bpg - 1) / bpg;
    uint64_t want = (uint64_t)blocks * bs / ratio;
    if (want < 256) want = 256;
    uint32_t ipg = (uint32_t)((want + groups - 1) / groups + 31) & ~31u;
    if (ipg > bs * 8) ipg = bs * 8;
    uint32_t itb = ipg * isz / bs, gdtb = (groups * 32 + bs - 1) / bs;
    if (groups > 1 && blocks - (groups - 1) * bpg < (has_super(groups - 1) ? 1 + gdtb : 0) + 2 + itb + 64) {
        groups--;                                            /* a last group too small to hold anything */
        blocks = groups * bpg;
    }
    uint32_t jlen = blocks >= 131072 ? 4096 : 1024, per = bs / 4;
    uint32_t jmeta = jlen > NDIR ? 1 : 0;
    if (jlen > NDIR + per) jmeta += 1 + (jlen - NDIR - per + per - 1) / per;
    uint32_t data0 = 1 + gdtb + 2 + itb;                     /* group 0: sb, gdt, bitmaps, inode table */
    uint32_t root = data0, lpf = data0 + 1, jstart = data0 + 2, used0 = jstart + jlen + jmeta;
    if (used0 > (blocks < bpg ? blocks : bpg)) return FATV_FULL;

    uint8_t *zero = kmalloc(64 * bs), *b = kmalloc(bs), *gdt = kmalloc((uint64_t)gdtb * bs);
    uint8_t sbuf[1024];
    int r = zero && b && gdt ? 0 : FATV_IO;
    if (r) goto out;
    memset(zero, 0, 64 * bs);
    memset(gdt, 0, (uint64_t)gdtb * bs);
    uint32_t t = now(), free_total = 0;
    for (uint32_t g = 0; g < groups && !r; g++) {            /* bitmaps and empty inode tables */
        uint32_t base = g * bpg, n = blocks - base < bpg ? blocks - base : bpg;
        uint32_t meta = has_super(g) ? 1 + gdtb : 0, used = g ? meta + 2 + itb : used0;
        group_t *gd = (group_t *)(gdt + g * 32);
        gd->block_bitmap = base + meta;
        gd->inode_bitmap = base + meta + 1;
        gd->inode_table = base + meta + 2;
        gd->free_blocks = (uint16_t)(n - used);
        gd->free_inodes = (uint16_t)(g ? ipg : ipg - 11);
        gd->used_dirs = g ? 0 : 2;
        free_total += n - used;
        memset(b, 0, bs);
        set_bits(b, 0, used);
        set_bits(b, n, bs * 8);                              /* past the end: "used" */
        r = fmt_write(dev, bs, gd->block_bitmap, 1, b);
        memset(b, 0, bs);
        set_bits(b, 0, g ? 0 : 11);                          /* inodes 1-10 reserved, 11 lost+found */
        set_bits(b, ipg, bs * 8);
        if (!r) r = fmt_write(dev, bs, gd->inode_bitmap, 1, b);
        for (uint32_t k = 0; k < itb && !r; k += 64)
            r = fmt_write(dev, bs, gd->inode_table + k, itb - k < 64 ? itb - k : 64, zero);
    }
    if (r) goto out;

    /* The inodes: root (2), the journal (8), lost+found (11). */
    memset(b, 0, bs);
    inode_t *in = (inode_t *)b;
    in[1].mode = EXT2_S_IFDIR | 0755;
    in[1].links = 3;
    in[1].size = bs;
    in[1].blocks = bs / 512;
    in[1].block[0] = root;
    in[7].mode = EXT2_S_IFREG | 0600;
    in[7].links = 1;
    in[7].size = jlen * bs;
    in[7].blocks = (jlen + jmeta) * (bs / 512);
    in[10].mode = EXT2_S_IFDIR | 0700;
    in[10].links = 2;
    in[10].size = bs;
    in[10].blocks = bs / 512;
    in[10].block[0] = lpf;
    for (int i = 0; i < 11; i++)
        if (in[i].mode) in[i].atime = in[i].ctime = in[i].mtime = t;
    uint32_t next = jstart + jlen, jblk[15];                 /* the journal's indirect blocks follow it */
    for (uint32_t i = 0; i < NDIR && i < jlen; i++) in[7].block[i] = jstart + i;
    uint32_t *ib = (uint32_t *)zero;                         /* (zero is rezeroed below) */
    if (jlen > NDIR && !r) {
        in[7].block[12] = next++;
        memset(ib, 0, bs);
        for (uint32_t i = 0; i < per && NDIR + i < jlen; i++) ib[i] = jstart + NDIR + i;
        r = fmt_write(dev, bs, in[7].block[12], 1, ib);
    }
    if (jlen > NDIR + per && !r) {
        uint32_t dind = next++, left = jlen - NDIR - per, k = 0;
        uint32_t *dd = (uint32_t *)(zero + bs);
        memset(dd, 0, bs);
        in[7].block[13] = dind;
        for (; left && !r; k++) {
            dd[k] = next++;
            memset(ib, 0, bs);
            for (uint32_t i = 0; i < per && left; i++, left--) ib[i] = jstart + NDIR + per + k * per + i;
            r = fmt_write(dev, bs, dd[k], 1, ib);
        }
        if (!r) r = fmt_write(dev, bs, dind, 1, dd);
    }
    memset(zero, 0, 2 * bs);
    memcpy(jblk, in[7].block, sizeof(jblk));
    if (!r) r = fmt_write(dev, bs, ((group_t *)gdt)->inode_table, 1, b);
    if (r) goto out;

    /* The root folder and lost+found. */
    memset(b, 0, bs);
    dirent_t *e = (dirent_t *)b;
    e->inode = 2; e->rec_len = 12; e->name_len = 1; e->file_type = FT_DIR; e->name[0] = '.';
    e = (dirent_t *)(b + 12);
    e->inode = 2; e->rec_len = 12; e->name_len = 2; e->file_type = FT_DIR; memcpy(e->name, "..", 2);
    e = (dirent_t *)(b + 24);
    e->inode = 11; e->rec_len = (uint16_t)(bs - 24); e->name_len = 10; e->file_type = FT_DIR;
    memcpy(e->name, "lost+found", 10);
    r = fmt_write(dev, bs, root, 1, b);
    memset(b, 0, bs);
    e = (dirent_t *)b;
    e->inode = 11; e->rec_len = 12; e->name_len = 1; e->file_type = FT_DIR; e->name[0] = '.';
    e = (dirent_t *)(b + 12);
    e->inode = 2; e->rec_len = (uint16_t)(bs - 12); e->name_len = 2; e->file_type = FT_DIR;
    memcpy(e->name, "..", 2);
    if (!r) r = fmt_write(dev, bs, lpf, 1, b);

    /* The superblock (and its copies in groups 1, 3, 5, 7, 9, 25, ...). */
    memset(sbuf, 0, sizeof(sbuf));
    super_t *s = (super_t *)sbuf;
    s->inodes_count = ipg * groups;
    s->blocks_count = blocks;
    s->free_blocks_count = free_total;
    s->free_inodes_count = ipg * groups - 11;
    s->first_data_block = 0;
    s->log_block_size = s->log_frag_size = 2;
    s->blocks_per_group = s->frags_per_group = bpg;
    s->inodes_per_group = ipg;
    s->wtime = s->lastcheck = s->mkfs_time = t;
    s->max_mnt_count = 0xFFFF;
    s->magic = MAGIC;
    s->state = 1;
    s->errors = 1;
    s->rev_level = 1;
    s->first_ino = 11;
    s->inode_size = (uint16_t)isz;
    s->feature_compat = COMPAT_JOURNAL;
    s->feature_incompat = INCOMPAT_FILETYPE;
    s->feature_ro_compat = RO_SPARSE | RO_LARGE_FILE;
    random_fill(s->uuid, 16);
    random_fill((uint8_t *)s->hash_seed, 16);
    for (int i = 0; i < 16 && label && label[i]; i++) s->volume_name[i] = label[i];
    s->journal_inum = JOURNAL_INO;
    s->jnl_backup_type = 1;                                  /* a copy of the journal inode's map */
    memcpy(s->jnl_blocks, jblk, sizeof(jblk));
    s->jnl_blocks[16] = jlen * bs;
    for (uint32_t g = 0; g < groups && !r; g++) {
        if (!has_super(g)) continue;
        memset(b, 0, bs);
        s->block_group_nr = (uint16_t)g;
        memcpy(b + (g ? 0 : 1024), sbuf, sizeof(sbuf));
        r = fmt_write(dev, bs, g * bpg, 1, b);
        if (!r) r = fmt_write(dev, bs, g * bpg + 1, gdtb, gdt);
    }

    /* The journal's superblock: empty, the first transaction a random number. */
    if (!r) {
        memset(b, 0, bs);
        uint8_t seq[4];
        random_fill(seq, 4);
        jheader(b, JSB_V2, 0);
        put_be32(b + 12, bs);
        put_be32(b + 16, jlen);
        put_be32(b + 20, 1);
        put_be32(b + 24, (be32(seq) & 0x7FFFFFFFu) | 1);
        memcpy(b + 48, s->uuid, 16);
        put_be32(b + 64, 1);
        r = fmt_write(dev, bs, jstart, 1, b);
    }
    if (!r && dev->flush) dev->flush(dev->ctx);
out:
    kfree(zero);
    kfree(b);
    kfree(gdt);
    return r;
}
