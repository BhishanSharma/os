// install.c - the installer (see sys/install.h)
//
// What it may touch: only a partition that is clearly Terminal OS's (FAT32
// labelled TERMINALOS: files on it are kept), or an empty one (never
// formatted: it is formatted). Windows' partitions, the EFI system partition
// and recovery are listed but never offered. The boot menu entry is a
// firmware variable (Boot####) added at the end of the boot order, so
// Windows stays the default.
#include "sys/install.h"
#include "core/multiboot2.h"
#include "drivers/efi.h"
#include "drivers/nvme.h"
#include "drivers/part.h"
#include "drivers/disk.h"
#include "drivers/fatvol.h"
#include "drivers/fat32.h"
#include "drivers/heap.h"
#include "drivers/memory.h"
#include "drivers/paging.h"
#include "sys/vfs.h"
#include "sys/users.h"
#include "sys/system.h"
#include "lib/print.h"
#include "lib/string.h"

#define ENTRY_NAME   "Terminal OS"
#define LOADER_PATH  "\\EFI\\TERMINALOS\\TERMOS.EFI"

static struct { const char *module, *file; uint64_t start, end; } files[3] = {
    { "install-loader", "TERMOS.EFI", 0, 0 },
    { "install-kernel", "KERNEL.BIN", 0, 0 },
    { "install-ramdisk", "RAMDISK.GZ", 0, 0 },
};
static int live, own_loader;
static char boot_part[40];                 /* the GUID of the partition the loader came from */

void install_save_boot_info(void) {
    live = mb2_cmdline_has("live");
    own_loader = mb2_cmdline_has("termos");
    if (mb2_cmdline_value("bootpart", boot_part, sizeof(boot_part)) != 0) boot_part[0] = 0;
    for (int i = 0; i < 3; i++) {
        uint64_t s, e;
        if (mb2_get_module(files[i].module, &s, &e) != 0) continue;
        files[i].start = s;
        files[i].end = e;
        memory_reserve(s, e);                               /* keep the heap off them */
        paging_add_direct_region(s, e - s);
    }
}

const char *install_boot_description(void) {
    if (live) return "from the USB stick (`install` puts Terminal OS on this PC)";
    if (boot_part[0]) return "installed, from the SSD";
    return "";
}

/* ---- Helpers ------------------------------------------------------------------------ */

static void guid_text(const uint8_t g[16], char *out) {
    static const int order[16] = { 3, 2, 1, 0, 5, 4, 7, 6, 8, 9, 10, 11, 12, 13, 14, 15 };
    int n = 0;
    for (int i = 0; i < 16; i++) {
        if (i == 4 || i == 6 || i == 8 || i == 10) out[n++] = '-';
        out[n++] = "0123456789abcdef"[g[order[i]] >> 4];
        out[n++] = "0123456789abcdef"[g[order[i]] & 15];
    }
    out[n] = 0;
}

static void size_text(uint64_t sectors, char *out, int len) {
    uint64_t mb = sectors / 2048;
    if (mb >= 10240) k_snprintf(out, len, "%u.%u GB", (uint32_t)(mb / 1024), (uint32_t)(mb % 1024 * 10 / 1024));
    else k_snprintf(out, len, "%u MB", (uint32_t)mb);
}

static int can_install(const partition_t *p) {
    if (part_is_protected(p)) return 0;
    /* An empty one is formatted: FAT32 needs 33 MiB at least. */
    return (p->fat32 && p->ours) || (p->fs && !strcmp(p->fs, "empty") && p->sectors >= 67584);
}

static const char *why_not(const partition_t *p) {
    if (p->kind && !strcmp(p->kind, "EFI system")) return "the PC boots from it: not touched";
    if (p->kind && (!strcmp(p->kind, "Microsoft reserved") || !strcmp(p->kind, "Windows recovery")))
        return "Windows needs it: not touched";
    if (p->fs && (!strcmp(p->fs, "NTFS") || !strcmp(p->fs, "BitLocker") || !strcmp(p->fs, "exFAT")))
        return "Windows files: not touched";
    if (p->fat32) return "FAT32 of something else: not touched";
    return "unknown contents: not touched";
}

static int ask(const char *prompt, char *buf, int size) {
    print_str(prompt);
    int n = users_read_line(buf, size, 1);
    if (n < 0) return -1;
    while (n > 0 && buf[n - 1] == ' ') buf[--n] = 0;
    return n;
}

static int is_yes(const char *s) {
    return (s[0] == 'y' || s[0] == 'Y') && (!s[1] || ((s[1] == 'e' || s[1] == 'E') && (s[2] == 's' || s[2] == 'S') && !s[3]));
}

/* ---- The firmware boot menu ------------------------------------------------------------- */

static void put16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void put32(uint8_t *p, uint32_t v) { put16(p, (uint16_t)v); put16(p + 2, (uint16_t)(v >> 16)); }
static void put64(uint8_t *p, uint64_t v) { put32(p, (uint32_t)v); put32(p + 4, (uint32_t)(v >> 32)); }

/* An EFI_LOAD_OPTION: "Terminal OS", HD(partition)/File(\EFI\TERMINALOS\TERMOS.EFI). */
static int load_option(const partition_t *p, uint8_t *o) {
    int n = 0;
    put32(o, 1);                                            /* LOAD_OPTION_ACTIVE */
    n = 6;                                                  /* (the path length goes at 4) */
    for (const char *c = ENTRY_NAME; ; c++) {
        put16(o + n, (uint8_t)*c);
        n += 2;
        if (!*c) break;
    }
    int path = n;
    o[n] = 4; o[n + 1] = 1; put16(o + n + 2, 42);           /* media: hard drive */
    put32(o + n + 4, (uint32_t)p->number);
    put64(o + n + 8, p->start);
    put64(o + n + 16, p->sectors);
    memcpy(o + n + 24, p->guid, 16);
    o[n + 40] = 2;                                          /* GPT */
    o[n + 41] = 2;                                          /* signature: a GUID */
    n += 42;
    int flen = 4 + (int)(strlen(LOADER_PATH) + 1) * 2;
    o[n] = 4; o[n + 1] = 4; put16(o + n + 2, (uint16_t)flen);   /* media: file path */
    for (int i = 0; ; i++) {
        put16(o + n + 4 + i * 2, (uint8_t)LOADER_PATH[i]);
        if (!LOADER_PATH[i]) break;
    }
    n += flen;
    o[n] = 0x7F; o[n + 1] = 0xFF; put16(o + n + 2, 4);      /* end */
    n += 4;
    put16(o + 4, (uint16_t)(n - path));
    return n;
}

/* Is Boot#### an entry called "Terminal OS"? */
static int ours(const uint8_t *opt, int len) {
    const char *want = ENTRY_NAME;
    for (int i = 0; ; i++) {
        if (6 + i * 2 + 1 >= len) return 0;
        uint16_t c = (uint16_t)(opt[6 + i * 2] | opt[7 + i * 2] << 8);
        if (c != (uint8_t)want[i]) return 0;
        if (!c) return 1;
    }
}

/* Add (or update) the boot menu entry; returns its number, or -1. */
static int add_boot_entry(const partition_t *p) {
    uint8_t opt[256], old[512];
    int len = load_option(p, opt);
    char name[16];
    int number = -1, free_number = -1;
    for (int i = 0; i < 0x100 && number < 0; i++) {
        k_snprintf(name, sizeof(name), "Boot%04X", i);
        int n = efi_get_variable(name, efi_global_guid, old, sizeof(old), 0);
        if (n < 0) {
            if (free_number < 0) free_number = i;
        } else if (ours(old, n)) {
            number = i;
        }
    }
    if (number < 0) number = free_number;
    if (number < 0) return -1;
    k_snprintf(name, sizeof(name), "Boot%04X", number);
    if (efi_set_variable(name, efi_global_guid, EFI_VAR_NV | EFI_VAR_BS | EFI_VAR_RT, opt, (uint32_t)len) != 0) return -1;

    uint16_t order[128];
    int count = efi_get_variable("BootOrder", efi_global_guid, order, sizeof(order) - 2, 0);
    count = count < 0 ? 0 : count / 2;
    for (int i = 0; i < count; i++)
        if (order[i] == number) return number;             /* already in the menu */
    order[count++] = (uint16_t)number;                      /* last: Windows stays first */
    if (efi_set_variable("BootOrder", efi_global_guid, EFI_VAR_NV | EFI_VAR_BS | EFI_VAR_RT,
                         order, (uint32_t)count * 2) != 0)
        return -1;
    return number;
}

/* ---- Copying ------------------------------------------------------------------------------ */

static int copy_files(const char *base) {
    char path[96];
    k_snprintf(path, sizeof(path), "%s/EFI", base);
    fat32_mkdir(path);
    k_snprintf(path, sizeof(path), "%s/EFI/TERMINALOS", base);
    fat32_mkdir(path);
    for (int i = 0; i < 3; i++) {
        uint32_t len = (uint32_t)(files[i].end - files[i].start);
        const uint8_t *data = (const uint8_t *)phys_to_virt(files[i].start);
        k_snprintf(path, sizeof(path), "%s/EFI/TERMINALOS/%s", base, files[i].file);
        kprintf("  %-12s %u KiB ", files[i].file, len / 1024);
        int r = fat32_write_file(path, data, len);
        if (r != (int)len) {
            kprintf("- could not write it (%s)\n", r < 0 ? vfs_error(r) : "short");
            return -1;
        }
        uint8_t *back = kmalloc(len);
        int ok = back && fat32_read_file(path, back, len) == (int)len && !memcmp(back, data, len);
        if (back) kfree(back);
        if (!ok) {
            print_str("- the copy does not read back the same\n");
            return -1;
        }
        print_str("- written and checked\n");
    }
    return 0;
}

/* ---- The installer ------------------------------------------------------------------------- */

static void how_to_make_space(void) {
    print_str("\nNo partition to install on. Make one in Windows:\n"
              "  1. Right-click Start, Disk Management.\n"
              "  2. Right-click C:, Shrink Volume (1024 MB or more).\n"
              "  3. Right-click the new Unallocated space, New Simple Volume:\n"
              "     either FAT32 with the label TERMINALOS (up to 32 GB),\n"
              "     or \"Do not format this volume\" (any size: the installer formats it).\n"
              "  4. Start Terminal OS from the USB stick again and run `install`.\n");
}

void install_run(void) {
    if (!files[0].start || !files[1].start || !files[2].start) {
        print_str("install: start Terminal OS from its USB stick to install it (the files come from there).\n");
        return;
    }
    if (!efi_available()) {
        print_str("install: this PC did not start Terminal OS through UEFI, so its boot menu cannot be changed.\n");
        return;
    }
    if (!nvme_present()) {
        print_str("install: no NVMe SSD found.\n");
        return;
    }
    static partition_t parts[16];
    int n = part_scan(nvme_partition_reader, parts, 16);
    if (n <= 0) {
        print_str("install: the SSD has no partition table.\n");
        return;
    }

    print_highlight("\nInstall Terminal OS on this PC\n\n");
    char size[16], gtext[40];
    int candidates = 0;
    kprintf("  %-3s %-10s %-20s %-22s\n", "#", "Size", "Type", "Contents");
    for (int i = 0; i < n; i++) {
        partition_t *p = &parts[i];
        size_text(p->sectors, size, sizeof(size));
        char contents[32];
        if (p->fat32) k_snprintf(contents, sizeof(contents), "FAT32 \"%s\"", p->label);
        else k_snprintf(contents, sizeof(contents), "%s", p->fs ? p->fs : "?");
        kprintf("  %-3d %-10s %-20s %-22s ", p->number, size, p->kind ? p->kind : "?", contents);
        guid_text(p->guid, gtext);
        int running_here = boot_part[0] && !live && !strcmp(gtext, boot_part);
        if (can_install(p)) {
            candidates++;
            print_accent(p->fat32 ? "<- can install here (its files are kept)" : "<- can install here (it gets formatted)");
            if (running_here) print_str(" (installed)");
            print_str("\n");
        } else {
            kprintf("%s\n", why_not(p));
        }
    }
    if (!candidates) {
        how_to_make_space();
        return;
    }

    char line[32];
    if (ask("\nInstall on partition number (Enter cancels): ", line, sizeof(line)) <= 0) {
        print_str("Cancelled.\n");
        return;
    }
    int want = 0;
    for (const char *c = line; *c >= '0' && *c <= '9'; c++) want = want * 10 + (*c - '0');
    partition_t *p = 0;
    for (int i = 0; i < n; i++)
        if (parts[i].number == want && can_install(&parts[i])) p = &parts[i];
    if (!p) {
        print_str("That is not one of the partitions marked \"can install here\".\n");
        return;
    }

    int format = !p->fat32;
    size_text(p->sectors, size, sizeof(size));
    kprintf("\nPartition %d (%s) will get:\n", p->number, size);
    if (format) print_str("  - a new FAT32 file system labelled TERMINALOS (it is empty now)\n");
    print_str("  - Terminal OS in \\EFI\\TERMINALOS (your files there are kept)\n"
              "  - an entry \"Terminal OS\" at the end of this PC's boot menu (Windows stays first)\n"
              "Nothing else on the SSD is changed.\n");
    if (ask("Type yes to install: ", line, sizeof(line)) < 0 || !is_yes(line)) {
        print_str("Cancelled.\n");
        return;
    }

    blockdev_t dev;
    if (format) {
        uint8_t first[512];
        if (nvme_read(p->start, 1, first) != 0) {
            print_str("install: cannot read the partition.\n");
            return;
        }
        for (int i = 0; i < 512; i++)
            if (first[i]) {
                print_str("install: the partition is not empty any more; nothing was changed.\n");
                return;
            }
        print_str("Formatting... ");
        if (disk_open_nvme_partition(p->start, p->sectors, 1, p->number, &dev) != 0 ||
            fatvol_format(&dev, p->start, OUR_LABEL) != 0) {
            print_str("failed.\n");
            return;
        }
        print_str("done.\n");
    }

    /* Write through the mount it already has (two mounts of one volume would
     * each cache the FAT), or mount it for the copy. */
    const char *mounted = vfs_partition_mount(p->start);
    int mounted_here = 0;
    if (!mounted) {
        if (disk_open_nvme_partition(p->start, p->sectors, 1, p->number, &dev) != 0 ||
            vfs_mount("/INSTALL", &dev) != 0) {
            print_str("install: cannot open the partition's file system.\n");
            return;
        }
        mounted = "/INSTALL";
        mounted_here = 1;
    }
    print_str("Copying Terminal OS:\n");
    int r = copy_files(strcmp(mounted, "/") ? mounted : "");
    if (mounted_here) vfs_unmount("/INSTALL");
    if (r != 0) {
        print_str("install: stopped; the boot menu was not changed.\n");
        return;
    }

    int number = add_boot_entry(p);
    if (number < 0) {
        print_str("install: the files are on the SSD, but the firmware did not take the boot menu entry.\n"
                  "Your PC's setup (F2) may let you add \\EFI\\TERMINALOS\\TERMOS.EFI as a boot option.\n");
        return;
    }
    kprintf("Added \"Terminal OS\" to the boot menu (Boot%04X).\n\n", number);
    print_highlight("Installed.");
    print_str(" Start it with your PC's boot menu key (F12 on Acer laptops) and pick Terminal OS.\n");
    if (ask("Restart into it now? (yes/no) ", line, sizeof(line)) > 0 && is_yes(line)) {
        uint16_t next = (uint16_t)number;
        if (efi_set_variable("BootNext", efi_global_guid, EFI_VAR_NV | EFI_VAR_BS | EFI_VAR_RT, &next, 2) != 0)
            print_str("(The firmware did not take BootNext: pick Terminal OS in the boot menu.)\n");
        print_str("Restarting (you can take the USB stick out)...\n");
        efi_reset(0);
        reboot();
    }
}

int install_own_loader(void) {
    return own_loader;
}

/* The description of a load option, as ASCII. */
static void option_name(const uint8_t *opt, int len, char *out, int size) {
    int n = 0;
    for (int i = 6; i + 1 < len && n < size - 1; i += 2) {
        uint16_t c = (uint16_t)(opt[i] | opt[i + 1] << 8);
        if (!c) break;
        out[n++] = c < 127 && c >= ' ' ? (char)c : '?';
    }
    out[n] = 0;
}

void install_print_boot_menu(void) {
    if (!efi_available()) {
        print_str("bootmenu: not started through UEFI.\n");
        return;
    }
    uint16_t order[64], current = 0xFFFF, next = 0xFFFF;
    int count = efi_get_variable("BootOrder", efi_global_guid, order, sizeof(order), 0);
    count = count < 0 ? 0 : count / 2;
    efi_get_variable("BootCurrent", efi_global_guid, &current, 2, 0);
    int has_next = efi_get_variable("BootNext", efi_global_guid, &next, 2, 0) == 2;
    print_str("The firmware's boot menu, in order:\n");
    for (int i = 0; i < count; i++) {
        char name[16], desc[64];
        uint8_t opt[512];
        k_snprintf(name, sizeof(name), "Boot%04X", order[i]);
        int n = efi_get_variable(name, efi_global_guid, opt, sizeof(opt), 0);
        if (n < 6) k_snprintf(desc, sizeof(desc), "(missing)");
        else option_name(opt, n, desc, sizeof(desc));
        kprintf("  %s  %s%s%s\n", name, desc, order[i] == current ? "   <- started this time" : "",
                n >= 4 && !(opt[0] & 1) ? "   (off)" : "");
    }
    if (has_next) kprintf("Next start: Boot%04X (once)\n", next);
}
