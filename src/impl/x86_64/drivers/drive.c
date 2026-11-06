// drive.c - the computer's own drives (see drivers/drive.h)
#include "drivers/drive.h"
#include "lib/print.h"
#include "lib/string.h"

static drive_t drives[MAX_DRIVES];
static int count;

int drive_add(const drive_t *d) {
    if (count >= MAX_DRIVES) return -1;
    drives[count] = *d;
    return count++;
}

int drive_count(void) { return count; }

const drive_t *drive_get(int n) { return n >= 0 && n < count ? &drives[n] : 0; }

int drive_read(int n, uint64_t lba, uint32_t count_, void *buf) {
    const drive_t *d = drive_get(n);
    if (!d || lba + count_ > d->sectors) return -1;
    return d->read(d->ctx, lba, count_, buf);
}

int drive_write(int n, uint64_t lba, uint32_t count_, const void *buf) {
    const drive_t *d = drive_get(n);
    if (!d || lba + count_ > d->sectors) return -1;
    return d->write(d->ctx, lba, count_, buf);
}

static int scanning;
static int scan_reader(uint64_t lba, uint32_t n, void *buf) { return drive_read(scanning, lba, n, buf); }

int drive_scan(int n, partition_t *out, int max) {
    if (!drive_get(n)) return 0;
    scanning = n;                        /* (one scan at a time: the callers hold the kernel lock) */
    return part_scan(scan_reader, out, max);
}

void drive_label(int n, char *out, int size) {
    const drive_t *d = drive_get(n);
    if (!d) {
        k_snprintf(out, (size_t)size, "drive %d", n + 1);
        return;
    }
    int same = 0, nth = 0;
    for (int i = 0; i < count; i++)
        if (!strcmp(drives[i].kind, d->kind)) {
            same++;
            if (i <= n) nth++;
        }
    if (same > 1) k_snprintf(out, (size_t)size, "%s drive %d", d->kind, nth);
    else k_snprintf(out, (size_t)size, "%s drive", d->kind);
}

void drive_print_info(void) {
    if (!count) {
        print_str("No internal drive (NVMe or SATA) was found.\n");
        return;
    }
    static partition_t parts[16];
    for (int i = 0; i < count; i++) {
        char label[32];
        drive_label(i, label, sizeof(label));
        uint64_t mb = drives[i].sectors / 2048;
        kprintf("%d. %s: %s, %u.%u GB\n", i + 1, label, drives[i].model[0] ? drives[i].model : "?",
                (uint32_t)(mb / 1000), (uint32_t)(mb % 1000 / 100));
        int n = drive_scan(i, parts, 16);
        if (!n) {
            kprintf("     no partition table\n");
            continue;
        }
        for (int k = 0; k < n; k++) {
            partition_t *p = &parts[k];
            kprintf("     %d: %-18s %6u MB  %s", p->number, p->kind ? p->kind : "?", (uint32_t)(p->sectors / 2048),
                    p->name);
            if (p->fat32) kprintf("  FAT32");
            else if (p->ext2) kprintf("  ext2/3");
            if (p->label[0]) kprintf(" \"%s\"", p->label);
            if (p->ours) kprintf("  <- Terminal OS files (read-write)");
            kprintf("\n");
        }
    }
    if (count > 1) print_str("(`mount disk <drive> <partition>` opens a FAT32 partition)\n");
}
