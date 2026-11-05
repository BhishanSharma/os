// part.c - GPT (UEFI) and MBR (old BIOS) partition tables
#include "drivers/part.h"
#include "drivers/ext2.h"
#include "drivers/heap.h"
#include "lib/string.h"

static uint32_t le32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static uint64_t le64(const uint8_t *p) { return le32(p) | (uint64_t)le32(p + 4) << 32; }
static void put32(uint8_t *p, uint32_t v) { for (int i = 0; i < 4; i++) p[i] = (uint8_t)(v >> 8 * i); }
static void put64(uint8_t *p, uint64_t v) { put32(p, (uint32_t)v); put32(p + 4, (uint32_t)(v >> 32)); }

const uint8_t part_type_linux[16] = { 0xAF, 0x3D, 0xC6, 0x0F, 0x83, 0x84, 0x72, 0x47,
                                      0x8E, 0x79, 0x3D, 0x69, 0xD8, 0x47, 0x7D, 0xE4 };

/* GPT type GUIDs, as stored on disk (mixed endian). */
static const struct { uint8_t guid[16]; const char *kind; } gpt_types[] = {
    { { 0x28, 0x73, 0x2A, 0xC1, 0x1F, 0xF8, 0xD2, 0x11, 0xBA, 0x4B, 0x00, 0xA0, 0xC9, 0x3E, 0xC9, 0x3B }, "EFI system" },
    { { 0x16, 0xE3, 0xC9, 0xE3, 0x5C, 0x0B, 0xB8, 0x4D, 0x81, 0x7D, 0xF9, 0x2D, 0xF0, 0x02, 0x15, 0xAE }, "Microsoft reserved" },
    { { 0xA2, 0xA0, 0xD0, 0xEB, 0xE5, 0xB9, 0x33, 0x44, 0x87, 0xC0, 0x68, 0xB6, 0xB7, 0x26, 0x99, 0xC7 }, "Microsoft data" },
    { { 0xA4, 0xBB, 0x94, 0xDE, 0xD1, 0x06, 0x40, 0x4D, 0xA1, 0x6A, 0xBF, 0xD5, 0x01, 0x79, 0xD6, 0xAC }, "Windows recovery" },
    { { 0xAF, 0x3D, 0xC6, 0x0F, 0x83, 0x84, 0x72, 0x47, 0x8E, 0x79, 0x3D, 0x69, 0xD8, 0x47, 0x7D, 0xE4 }, "Linux" },
};

static void set_label(partition_t *p, const uint8_t *name) {
    memcpy(p->label, name, 11);
    p->label[11] = 0;
    for (int i = 10; i >= 0 && p->label[i] == ' '; i--) p->label[i] = 0;
    if (!strcmp(p->label, "NO NAME")) p->label[0] = 0;
}

/* Is there a FAT32 file system at `lba`? Fills the label. Windows writes the
 * label only into the root folder (a volume-ID entry) and leaves "NO NAME" in
 * the boot sector, so the root folder's entry wins. */
static void probe_fat32(sector_reader_t rd, partition_t *p, uint8_t *buf) {
    p->fat32 = 0;
    p->label[0] = 0;
    p->fs = "unknown";
    if (rd(p->start, 1, buf) != 0) return;
    int zero = 1;
    for (int i = 0; i < 512; i++) zero &= buf[i] == 0;
    if (zero) p->fs = "empty";                              /* never formatted ("RAW" in Windows) */
    else if (!memcmp(buf + 3, "NTFS    ", 8)) p->fs = "NTFS";
    else if (!memcmp(buf + 3, "EXFAT   ", 8)) p->fs = "exFAT";
    else if (!memcmp(buf + 3, "-FVE-FS-", 8)) p->fs = "BitLocker";
    if (buf[510] != 0x55 || buf[511] != 0xAA) {
        if (p->sectors > 4 && rd(p->start + 2, 1, buf) == 0 && ext2_probe(buf, p->label)) {
            uint32_t compat = le32(buf + 92), incompat = le32(buf + 96);
            p->ext2 = 1;
            p->fs = incompat & 0x2C0 ? "ext4" : compat & 4 ? "ext3" : "ext2";
            p->ours = !strcmp(p->label, OUR_LABEL) || !strcmp(p->label, DATA_LABEL);
        }
        return;
    }
    if (memcmp(buf + 82, "FAT32   ", 8) != 0) return;
    p->fs = "FAT32";
    p->fat32 = 1;
    set_label(p, buf + 71);

    uint32_t spc = buf[13], reserved = buf[14] | buf[15] << 8, nfats = buf[16];
    uint32_t fat_size = le32(buf + 36), root = le32(buf + 44);
    if ((buf[11] | buf[12] << 8) == 512 && spc && root >= 2) {
        uint64_t lba = p->start + reserved + (uint64_t)nfats * fat_size + (uint64_t)(root - 2) * spc;
        uint32_t count = spc < 8 ? spc : 8;               /* the start of the root folder */
        if (rd(lba, count, buf) == 0) {
            for (uint32_t off = 0; off < count * 512; off += 32) {
                const uint8_t *e = buf + off;
                if (e[0] == 0) break;
                if (e[0] == 0xE5 || (e[11] & 0x3F) == 0x0F || !(e[11] & 0x08)) continue;
                set_label(p, e);
                break;
            }
        }
    }
    p->ours = strcmp(p->label, OUR_LABEL) == 0;
}

int part_scan(sector_reader_t rd, partition_t *out, int max) {
    uint8_t *buf = kmalloc(4096);
    if (!buf) return 0;
    int n = 0;
    if (rd(0, 1, buf) != 0 || buf[510] != 0x55 || buf[511] != 0xAA) {
        kfree(buf);
        return 0;
    }
    int gpt = 0;
    for (int i = 0; i < 4; i++)
        if (buf[0x1BE + 16 * i + 4] == 0xEE) gpt = 1;
    if (!gpt) {                                             /* MBR: four primary entries */
        uint8_t mbr[64];
        memcpy(mbr, buf + 0x1BE, 64);
        for (int i = 0; i < 4 && n < max; i++) {
            const uint8_t *e = mbr + 16 * i;
            uint8_t type = e[4];
            if (!type || type == 0x05 || type == 0x0F) continue;   /* empty, extended */
            partition_t *p = &out[n++];
            memset(p, 0, sizeof(*p));
            p->number = i + 1;
            p->start = le32(e + 8);
            p->sectors = le32(e + 12);
            p->kind = type == 0x07 ? "NTFS / exFAT" : (type == 0x0B || type == 0x0C) ? "FAT32" :
                      type == 0x83 ? "Linux" : type == 0xEF ? "EFI system" : "other";
            probe_fat32(rd, p, buf);
        }
        kfree(buf);
        return n;
    }
    if (rd(1, 1, buf) != 0 || memcmp(buf, "EFI PART", 8) != 0) {
        kfree(buf);
        return 0;
    }
    uint64_t entries_lba = le64(buf + 72);
    uint32_t count = le32(buf + 80), esize = le32(buf + 84);
    if (esize < 128 || esize > 512 || count > 256) {
        kfree(buf);
        return 0;
    }
    uint32_t per = 4096 / esize;
    static uint8_t table[4096];
    for (uint32_t i = 0; i < count && n < max; i++) {
        if (i % per == 0 && rd(entries_lba + (uint64_t)i * esize / 512, 8, table) != 0) break;
        const uint8_t *e = table + (i % per) * esize;
        int empty = 1;
        for (int k = 0; k < 16; k++) empty &= e[k] == 0;
        if (empty) continue;
        partition_t *p = &out[n++];
        memset(p, 0, sizeof(*p));
        p->number = (int)i + 1;
        p->gpt = 1;
        memcpy(p->guid, e + 16, 16);
        p->start = le64(e + 32);
        p->sectors = le64(e + 40) - p->start + 1;
        for (int k = 0; k < 36; k++) {                      /* UTF-16 name: keep ASCII */
            uint16_t c = (uint16_t)(e[56 + 2 * k] | e[57 + 2 * k] << 8);
            if (!c) break;
            p->name[k] = c < 128 ? (char)c : '?';
        }
        p->kind = "other";
        for (unsigned t = 0; t < sizeof(gpt_types) / sizeof(gpt_types[0]); t++)
            if (!memcmp(e, gpt_types[t].guid, 16)) p->kind = gpt_types[t].kind;
        probe_fat32(rd, p, buf);
    }
    kfree(buf);
    return n;
}

static uint32_t crc32(const uint8_t *p, uint32_t n) {
    uint32_t c = 0xFFFFFFFFu;
    while (n--) {
        c ^= *p++;
        for (int k = 0; k < 8; k++) c = c >> 1 ^ (0xEDB88320u & (0u - (c & 1)));
    }
    return ~c;
}

int part_gpt_add(sector_reader_t rd, sector_writer_t wr, uint64_t sectors, uint64_t start, uint64_t end,
                 const uint8_t type[16], const char *name) {
    uint8_t *mbr = kmalloc(512), *hdr = kmalloc(512), *tab = kmalloc(65536);
    int r = -1;
    if (!mbr || !hdr || !tab || rd(0, 1, mbr) != 0 || rd(1, 1, hdr) != 0 || memcmp(hdr, "EFI PART", 8)) goto out;
    uint32_t hsize = le32(hdr + 12), count = le32(hdr + 80), esize = le32(hdr + 84);
    uint64_t entries = le64(hdr + 72), first = le64(hdr + 40);
    uint32_t bytes = count * esize, nsec = bytes / 512;
    if (hsize < 92 || hsize > 512 || esize < 128 || esize > 512 || bytes > 65536 || bytes % 512 ||
        le64(hdr + 24) != 1 || rd(entries, nsec, tab) != 0 || crc32(tab, bytes) != le32(hdr + 88))
        goto out;
    uint64_t last = sectors - 2 - nsec;                     /* room for the backup table at the end */
    if (start < first || end > last || start > end) goto out;
    int slot = -1;
    for (uint32_t i = 0; i < count; i++) {
        const uint8_t *e = tab + i * esize;
        int empty = 1;
        for (int k = 0; k < 16; k++) empty &= e[k] == 0;
        if (empty) {
            if (slot < 0) slot = (int)i;
        } else if (le64(e + 32) <= end && le64(e + 40) >= start) {
            goto out;                                       /* overlaps a partition */
        }
    }
    if (slot < 0) goto out;
    uint8_t *e = tab + (uint32_t)slot * esize;
    memset(e, 0, esize);
    memcpy(e, type, 16);
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    uint64_t x = ((uint64_t)hi << 32 | lo) ^ 0x9E3779B97F4A7C15ull;
    for (int k = 0; k < 16; k++) {                          /* its own GUID */
        x ^= x << 13; x ^= x >> 7; x ^= x << 17;
        e[16 + k] = (uint8_t)(x >> 29);
    }
    e[16 + 7] = (uint8_t)((e[16 + 7] & 0x0F) | 0x40);       /* version 4 (random) */
    e[16 + 8] = (uint8_t)((e[16 + 8] & 0x3F) | 0x80);
    put64(e + 32, start);
    put64(e + 40, end);
    for (int k = 0; name[k] && k < 36; k++) e[56 + 2 * k] = (uint8_t)name[k];
    put32(hdr + 88, crc32(tab, bytes));
    put64(hdr + 48, last);

    /* The backup (header in the last sector, the table before it), then the primary. */
    uint8_t *bak = mbr + 0;                                 /* (mbr is read again below) */
    memcpy(bak, hdr, 512);
    put64(bak + 24, sectors - 1);
    put64(bak + 32, 1);
    put64(bak + 72, sectors - 1 - nsec);
    put32(bak + 16, 0);
    put32(bak + 16, crc32(bak, hsize));
    if (wr(sectors - 1 - nsec, nsec, tab) != 0 || wr(sectors - 1, 1, bak) != 0) goto out;
    put64(hdr + 32, sectors - 1);
    put32(hdr + 16, 0);
    put32(hdr + 16, crc32(hdr, hsize));
    if (wr(entries, nsec, tab) != 0 || wr(1, 1, hdr) != 0) goto out;
    /* The protective MBR covers the whole drive now. */
    if (rd(0, 1, mbr) != 0) goto out;
    for (int i = 0; i < 4; i++) {
        uint8_t *m = mbr + 0x1BE + 16 * i;
        if (m[4] == 0xEE) put32(m + 12, sectors - 1 > 0xFFFFFFFFull ? 0xFFFFFFFFu : (uint32_t)(sectors - 1));
    }
    r = wr(0, 1, mbr);
out:
    kfree(mbr);
    kfree(hdr);
    kfree(tab);
    return r;
}

int part_is_protected(const partition_t *p) {
    if (!p->kind || !strcmp(p->kind, "EFI system") || !strcmp(p->kind, "Microsoft reserved") ||
        !strcmp(p->kind, "Windows recovery"))
        return 1;
    return !p->fs || !strcmp(p->fs, "NTFS") || !strcmp(p->fs, "exFAT") || !strcmp(p->fs, "BitLocker");
}
