// part.c - GPT (UEFI) and MBR (old BIOS) partition tables
#include "drivers/part.h"
#include "drivers/heap.h"
#include "lib/string.h"

static uint32_t le32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static uint64_t le64(const uint8_t *p) { return le32(p) | (uint64_t)le32(p + 4) << 32; }

/* GPT type GUIDs, as stored on disk (mixed endian). */
static const struct { uint8_t guid[16]; const char *kind; } gpt_types[] = {
    { { 0x28, 0x73, 0x2A, 0xC1, 0x1F, 0xF8, 0xD2, 0x11, 0xBA, 0x4B, 0x00, 0xA0, 0xC9, 0x3E, 0xC9, 0x3B }, "EFI system" },
    { { 0x16, 0xE3, 0xC9, 0xE3, 0x5C, 0x0B, 0xB8, 0x4D, 0x81, 0x7D, 0xF9, 0x2D, 0xF0, 0x02, 0x15, 0xAE }, "Microsoft reserved" },
    { { 0xA2, 0xA0, 0xD0, 0xEB, 0xE5, 0xB9, 0x33, 0x44, 0x87, 0xC0, 0x68, 0xB6, 0xB7, 0x26, 0x99, 0xC7 }, "Microsoft data" },
    { { 0xA4, 0xBB, 0x94, 0xDE, 0xD1, 0x06, 0x40, 0x4D, 0xA1, 0x6A, 0xBF, 0xD5, 0x01, 0x79, 0xD6, 0xAC }, "Windows recovery" },
    { { 0xAF, 0x3D, 0xC6, 0x0F, 0x83, 0x84, 0x72, 0x47, 0x8E, 0x79, 0x3D, 0x69, 0xD8, 0x47, 0x7D, 0xE4 }, "Linux" },
};

/* Is there a FAT32 file system at `lba`? Fills the label. */
static void probe_fat32(sector_reader_t rd, partition_t *p, uint8_t *buf) {
    p->fat32 = 0;
    p->label[0] = 0;
    if (rd(p->start, 1, buf) != 0 || buf[510] != 0x55 || buf[511] != 0xAA) return;
    if (memcmp(buf + 82, "FAT32   ", 8) != 0) return;
    p->fat32 = 1;
    memcpy(p->label, buf + 71, 11);
    p->label[11] = 0;
    for (int i = 10; i >= 0 && p->label[i] == ' '; i--) p->label[i] = 0;
    if (!strcmp(p->label, "NO NAME")) p->label[0] = 0;
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
