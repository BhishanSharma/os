// main.c - Terminal OS's UEFI boot loader (TERMOS.EFI / BOOTX64.EFI)
//
// Loads \EFI\TERMINALOS\KERNEL.BIN and RAMDISK.GZ from the partition it was
// started from, and hands over to the kernel with the same multiboot2
// information GRUB used to give it (screen, memory map, ACPI, the RAM disk),
// plus the files the installer needs. Then:
//   1. leave the firmware (ExitBootServices),
//   2. switch to its own identity page tables (the firmware's may make the
//      low memory read-only or non-executable),
//   3. copy the kernel to its fixed address (1 MiB) and jump to start64.
// Steps 2 and 3 run from a small piece of code copied above 16 MiB, so the
// copy cannot overwrite it.
#include "efi.h"
#include "inflate.h"

#define KERNEL_DIR     L"\\EFI\\TERMINALOS\\"
#define MB2_MAGIC      0x36d76289u
#define MIB            (1024ull * 1024)

static EFI_SYSTEM_TABLE *ST;
static EFI_BOOT_SERVICES *BS;
static EFI_HANDLE self;

/* ---- The C library bits the compiler expects ------------------------------------- */

void *memset(void *d, int c, size_t n) {
    uint8_t *p = d;
    while (n--) *p++ = (uint8_t)c;
    return d;
}

void *memcpy(void *d, const void *s, size_t n) {
    uint8_t *a = d;
    const uint8_t *b = s;
    while (n--) *a++ = *b++;
    return d;
}

int memcmp(const void *x, const void *y, size_t n) {
    const uint8_t *a = x, *b = y;
    for (; n; n--, a++, b++)
        if (*a != *b) return *a - *b;
    return 0;
}

static size_t cstrlen(const char *s) {
    size_t n = 0;
    while (s[n]) n++;
    return n;
}

/* ---- Text ---------------------------------------------------------------------------- */

static void print(const char *s) {
    CHAR16 buf[128];
    size_t n = 0;
    for (; *s; s++) {
        if (*s == '\n') buf[n++] = '\r';
        buf[n++] = (CHAR16)(uint8_t)*s;
        if (n >= 120) {
            buf[n] = 0;
            ST->ConOut->OutputString(ST->ConOut, buf);
            n = 0;
        }
    }
    buf[n] = 0;
    ST->ConOut->OutputString(ST->ConOut, buf);
}

static void print_num(uint64_t v) {
    char buf[24];
    int i = 23;
    buf[i] = 0;
    do {
        buf[--i] = (char)('0' + v % 10);
        v /= 10;
    } while (v);
    print(buf + i);
}

/* Something went wrong before the kernel runs: say what, then back to the firmware. */
static EFI_STATUS fail(const char *what, EFI_STATUS st) {
    ST->ConOut->SetAttribute(ST->ConOut, 0x0C);             /* light red */
    print("\nTerminal OS cannot start: ");
    print(what);
    if (st) {
        print(" (UEFI error ");
        print_num(st & ~EFI_ERR);
        print(")");
    }
    ST->ConOut->SetAttribute(ST->ConOut, 0x07);
    print("\nPress a key to go back to the firmware.\n");
    uint64_t index;
    BS->WaitForEvent(1, &ST->ConIn->WaitForKey, &index);
    EFI_INPUT_KEY key;
    ST->ConIn->ReadKeyStroke(ST->ConIn, &key);
    return st ? st : EFI_LOAD_ERROR;
}

static int guid_eq(const EFI_GUID *a, const EFI_GUID *b) { return memcmp(a, b, sizeof(*a)) == 0; }

/* ---- Memory ------------------------------------------------------------------------- */

static EFI_MEMORY_DESCRIPTOR *map;
static uint64_t map_capacity, map_size, map_key, desc_size;
static uint32_t desc_version;

static EFI_STATUS get_map(void) {
    for (;;) {
        map_size = map_capacity;
        EFI_STATUS st = BS->GetMemoryMap(&map_size, map, &map_key, &desc_size, &desc_version);
        if (st != EFI_BUFFER_TOO_SMALL) return st;
        if (map) BS->FreePool(map);
        map_capacity = map_size + 32 * (desc_size ? desc_size : 48);
        map = 0;
        st = BS->AllocatePool(EfiLoaderData, map_capacity, (void **)&map);
        if (EFI_ERROR(st)) return st;
    }
}

#define DESC(i) ((EFI_MEMORY_DESCRIPTOR *)((uint8_t *)map + (i) * desc_size))
#define DESC_COUNT (map_size / desc_size)

/* Free memory for the hand-over (the RAM disk, the boot information, ...):
 * above 2 GiB and below 4 GiB if there is any (out of the way of the kernel
 * heap and of user programs at 1-2 GiB), else between 16 MiB and 1 GiB.
 * Below 4 GiB either way: the multiboot2 module addresses are 32-bit. */
static void *alloc_high(uint64_t bytes) {
    uint64_t pages = (bytes + 4095) / 4096, need = pages * 4096;
    if (EFI_ERROR(get_map())) return 0;
    static const uint64_t windows[2][2] = { { 2048 * MIB, 4096 * MIB }, { 16 * MIB, 1024 * MIB } };
    for (int w = 0; w < 2; w++) {
        uint64_t best = 0;
        for (uint64_t i = 0; i < DESC_COUNT; i++) {
            EFI_MEMORY_DESCRIPTOR *d = DESC(i);
            if (d->Type != EfiConventionalMemory) continue;
            uint64_t start = d->PhysicalStart, end = start + d->NumberOfPages * 4096;
            if (start < windows[w][0]) start = windows[w][0];
            if (end > windows[w][1]) end = windows[w][1];
            if (end <= start || end - start < need) continue;
            uint64_t at = (end - need) & ~0xFFFull;
            if (at >= start && at > best) best = at;
        }
        if (best) {
            EFI_PHYSICAL_ADDRESS addr = best;
            if (!EFI_ERROR(BS->AllocatePages(AllocateAddress, EfiLoaderData, pages, &addr))) {
                memset((void *)addr, 0, need);
                return (void *)addr;
            }
        }
    }
    return 0;
}

/* Is all of [start, end) memory the kernel may overwrite once the firmware is gone? */
static int free_after_exit(uint64_t start, uint64_t end) {
    if (EFI_ERROR(get_map())) return 0;
    uint64_t at = start;
    while (at < end) {
        int found = 0;
        for (uint64_t i = 0; i < DESC_COUNT; i++) {
            EFI_MEMORY_DESCRIPTOR *d = DESC(i);
            uint64_t s = d->PhysicalStart, e = s + d->NumberOfPages * 4096;
            if (at < s || at >= e) continue;
            uint32_t t = d->Type;
            if (t != EfiConventionalMemory && t != EfiBootServicesCode && t != EfiBootServicesData &&
                t != EfiLoaderCode && t != EfiLoaderData)
                return 0;
            at = e;
            found = 1;
            break;
        }
        if (!found) return 0;
    }
    return 1;
}

/* ---- Files ---------------------------------------------------------------------------- */

static EFI_FILE_PROTOCOL *root;

static uint8_t *read_file(const CHAR16 *path, uint64_t *size) {
    EFI_FILE_PROTOCOL *f;
    if (EFI_ERROR(root->Open(root, &f, path, EFI_FILE_MODE_READ, 0))) return 0;
    uint64_t len = 0;
    uint8_t *buf = 0;
    if (!EFI_ERROR(f->SetPosition(f, ~0ull)) && !EFI_ERROR(f->GetPosition(f, &len)) &&
        !EFI_ERROR(f->SetPosition(f, 0)) && len &&
        !EFI_ERROR(BS->AllocatePool(EfiLoaderData, len, (void **)&buf))) {
        uint64_t got = len;
        if (EFI_ERROR(f->Read(f, &got, buf)) || got != len) {
            BS->FreePool(buf);
            buf = 0;
        }
    }
    f->Close(f);
    *size = len;
    return buf;
}

/* ---- The kernel image ------------------------------------------------------------------- */

typedef struct {
    uint8_t ident[16];
    uint16_t type, machine;
    uint32_t version;
    uint64_t entry, phoff, shoff;
    uint32_t flags;
    uint16_t ehsize, phentsize, phnum, shentsize, shnum, shstrndx;
} elf64_ehdr_t;

typedef struct {
    uint32_t type, flags;
    uint64_t offset, vaddr, paddr, filesz, memsz, align;
} elf64_phdr_t;

typedef struct {
    uint8_t *image;          /* every loadable segment laid out, BSS zeroed */
    uint64_t base, size;     /* where it goes (physical) */
    uint64_t entry64;
} kernel_t;

static const char *load_kernel(const uint8_t *file, uint64_t len, kernel_t *k) {
    const elf64_ehdr_t *eh = (const elf64_ehdr_t *)file;
    if (len < sizeof(*eh) || memcmp(eh->ident, "\x7f" "ELF", 4) || eh->ident[4] != 2 || eh->machine != 0x3E)
        return "KERNEL.BIN is not a 64-bit ELF file";
    uint64_t lo = ~0ull, hi = 0;
    for (int i = 0; i < eh->phnum; i++) {
        const elf64_phdr_t *ph = (const elf64_phdr_t *)(file + eh->phoff + (uint64_t)i * eh->phentsize);
        if (ph->type != 1 || !ph->memsz) continue;
        if (ph->offset + ph->filesz > len) return "KERNEL.BIN is cut short";
        if (ph->paddr < lo) lo = ph->paddr;
        if (ph->paddr + ph->memsz > hi) hi = ph->paddr + ph->memsz;
    }
    if (lo < MIB || hi > 16 * MIB || hi <= lo) return "KERNEL.BIN has an unexpected layout";
    /* The 64-bit entry: "TERMOS64" and its address, in the boot header. */
    k->entry64 = 0;
    for (uint64_t off = 0; off + 16 <= len && off < 64 * 1024; off += 8)
        if (!memcmp(file + off, "TERMOS64", 8)) {
            memcpy(&k->entry64, file + off + 8, 8);
            break;
        }
    if (!k->entry64) return "KERNEL.BIN has no 64-bit entry (too old?)";
    k->base = lo;
    k->size = hi - lo;
    k->image = alloc_high(k->size);
    if (!k->image) return "no memory for the kernel";
    for (int i = 0; i < eh->phnum; i++) {
        const elf64_phdr_t *ph = (const elf64_phdr_t *)(file + eh->phoff + (uint64_t)i * eh->phentsize);
        if (ph->type == 1 && ph->filesz) memcpy(k->image + (ph->paddr - lo), file + ph->offset, ph->filesz);
    }
    return 0;
}

/* ---- The multiboot2 information ----------------------------------------------------------- */

static uint8_t *info;
static uint32_t info_len, info_cap;

static void *tag(uint32_t type, uint32_t size) {
    info_len = (info_len + 7) & ~7u;
    if (info_len + size + 8 > info_cap) return 0;
    uint32_t *t = (uint32_t *)(info + info_len);
    t[0] = type;
    t[1] = size;
    info_len += size;
    return t;
}

static void tag_module(const void *start, uint64_t len, const char *name) {
    size_t n = cstrlen(name) + 1;
    uint32_t *t = tag(3, (uint32_t)(16 + n));
    if (!t) return;
    t[2] = (uint32_t)(uint64_t)start;
    t[3] = (uint32_t)((uint64_t)start + len);
    memcpy(t + 4, name, n);
}

static uint32_t mb2_type(uint32_t efi_type) {
    switch (efi_type) {
    case EfiConventionalMemory: case EfiLoaderCode:
    case EfiBootServicesCode: case EfiBootServicesData:
        return 1;                                          /* free for the kernel */
    case EfiACPIReclaimMemory: return 3;
    case EfiACPIMemoryNVS:     return 4;
    case EfiUnusableMemory:    return 5;
    default:                   return 2;                   /* EfiLoaderData: what we hand over */
    }
}

/* The memory map tags, from the final map (after ExitBootServices: no calls). */
static void tag_memory_map(void) {
    uint64_t count = DESC_COUNT;
    uint32_t *t = tag(6, (uint32_t)(16 + count * 24));
    if (t) {
        t[2] = 24;
        t[3] = 0;
        uint8_t *e = (uint8_t *)(t + 4);
        uint32_t used = 0;
        for (uint64_t i = 0; i < count; i++) {
            EFI_MEMORY_DESCRIPTOR *d = DESC(i);
            uint64_t base = d->PhysicalStart, length = d->NumberOfPages * 4096;
            uint32_t type = mb2_type(d->Type);
            if (used) {                                     /* merge with the previous one */
                uint8_t *p = e + (used - 1) * 24;
                if (*(uint32_t *)(p + 16) == type && *(uint64_t *)p + *(uint64_t *)(p + 8) == base) {
                    *(uint64_t *)(p + 8) += length;
                    continue;
                }
            }
            uint8_t *p = e + used * 24;
            *(uint64_t *)p = base;
            *(uint64_t *)(p + 8) = length;
            *(uint32_t *)(p + 16) = type;
            *(uint32_t *)(p + 20) = 0;
            used++;
        }
        t[1] = 16 + used * 24;
        info_len = (uint32_t)((uint8_t *)t - info) + t[1];
    }
    /* The firmware's own map too (type 17), for the kernel's runtime-services calls. */
    uint32_t *f = tag(17, (uint32_t)(16 + map_size));
    if (f) {
        f[2] = (uint32_t)desc_size;
        f[3] = desc_version;
        memcpy(f + 4, map, map_size);
    }
}

static void tag_end(void) {
    tag(0, 8);
    ((uint32_t *)info)[0] = (info_len + 7) & ~7u;
    ((uint32_t *)info)[1] = 0;
}

/* ---- Where we were started from ---------------------------------------------------------- */

static void hex_byte(char *out, uint8_t b) {
    out[0] = "0123456789abcdef"[b >> 4];
    out[1] = "0123456789abcdef"[b & 15];
}

/* "live" when started from a USB stick (or other removable disk), and
 * "bootpart=<partition GUID>" of the partition the files are on. */
static void boot_device(EFI_LOADED_IMAGE_PROTOCOL *li, char *cmdline, size_t cap) {
    static const EFI_GUID dp_guid = DEVICE_PATH_GUID, bio_guid = BLOCK_IO_GUID;
    size_t n = 7;
    memcpy(cmdline, "termos ", 7);                          /* who started the kernel */
    int live = 0;
    EFI_DEVICE_PATH *dp = 0;
    EFI_BLOCK_IO_PROTOCOL *bio = 0;
    if (!EFI_ERROR(BS->HandleProtocol(li->DeviceHandle, &bio_guid, (void **)&bio)) && bio->Media &&
        bio->Media->RemovableMedia)
        live = 1;
    if (!EFI_ERROR(BS->HandleProtocol(li->DeviceHandle, &dp_guid, (void **)&dp))) {
        for (int guard = 0; dp && dp->Type != DP_END && guard < 64; guard++) {
            uint16_t len = (uint16_t)(dp->Length[0] | dp->Length[1] << 8);
            if (len < 4) break;
            if (dp->Type == DP_MESSAGING && dp->SubType == DP_MSG_USB) live = 1;
            if (dp->Type == DP_MEDIA && dp->SubType == DP_MEDIA_HD) {
                EFI_HD_DEVICE_PATH *hd = (EFI_HD_DEVICE_PATH *)dp;
                if (hd->SignatureType == 2 && n + 48 < cap) {   /* GPT: the partition's GUID */
                    const char *key = "bootpart=";
                    memcpy(cmdline + n, key, 9);
                    n += 9;
                    const uint8_t *g = hd->Signature;
                    static const int order[16] = { 3, 2, 1, 0, 5, 4, 7, 6, 8, 9, 10, 11, 12, 13, 14, 15 };
                    for (int i = 0; i < 16; i++) {
                        if (i == 4 || i == 6 || i == 8 || i == 10) cmdline[n++] = '-';
                        hex_byte(cmdline + n, g[order[i]]);
                        n += 2;
                    }
                    cmdline[n++] = ' ';
                }
            }
            dp = (EFI_DEVICE_PATH *)((uint8_t *)dp + len);
        }
    }
    if (live && n + 6 < cap) {
        memcpy(cmdline + n, "live ", 5);
        n += 5;
    }
    cmdline[n] = 0;
}

/* ---- The jump ---------------------------------------------------------------------------- */

/* Copied above 16 MiB and called with rcx = &params (Microsoft x64 ABI):
 *   cli; cr3 = params.cr3; cr0.WP = 0; copy the kernel; edi = info, esi = magic; jmp entry */
static const uint8_t trampoline[] = {
    0xFA,                               /* cli                     */
    0x48, 0x8B, 0x41, 0x28,             /* mov rax, [rcx+40] (cr3) */
    0x0F, 0x22, 0xD8,                   /* mov cr3, rax            */
    0x0F, 0x20, 0xC0,                   /* mov rax, cr0            */
    0x48, 0x25, 0xFF, 0xFF, 0xFE, 0xFF, /* and rax, ~(1 << 16)     */
    0x0F, 0x22, 0xC0,                   /* mov cr0, rax            */
    0xFC,                               /* cld                     */
    0x48, 0x8B, 0x39,                   /* mov rdi, [rcx]    (dst) */
    0x48, 0x8B, 0x71, 0x08,             /* mov rsi, [rcx+8]  (src) */
    0x48, 0x8B, 0x41, 0x18,             /* mov rax, [rcx+24] (entry) */
    0x4C, 0x8B, 0x41, 0x20,             /* mov r8,  [rcx+32] (info) */
    0x48, 0x8B, 0x49, 0x10,             /* mov rcx, [rcx+16] (len) */
    0xF3, 0xA4,                         /* rep movsb               */
    0x4C, 0x89, 0xC7,                   /* mov rdi, r8             */
    0xBE, 0x89, 0x62, 0xD7, 0x36,       /* mov esi, 0x36d76289     */
    0xFF, 0xE0,                         /* jmp rax                 */
};

typedef struct { uint64_t dst, src, len, entry, info, cr3; } jump_params_t;

/* Identity map of the first 4 GiB with 2 MiB pages: writable, executable. */
static uint64_t make_page_tables(void) {
    uint64_t *t = alloc_high(6 * 4096);
    if (!t) return 0;
    uint64_t *pml4 = t, *pdpt = t + 512, *pd = t + 1024;
    pml4[0] = (uint64_t)pdpt | 3;
    for (int i = 0; i < 4; i++) pdpt[i] = (uint64_t)(pd + 512 * i) | 3;
    for (uint64_t i = 0; i < 2048; i++) pd[i] = (i * 2 * MIB) | 0x83;
    return (uint64_t)pml4;
}

/* ---- Entry --------------------------------------------------------------------------------- */

EFI_STATUS EFIAPI efi_main(EFI_HANDLE image, EFI_SYSTEM_TABLE *st) {
    ST = st;
    BS = st->BootServices;
    self = image;
    BS->SetWatchdogTimer(0, 0, 0, 0);
    ST->ConOut->ClearScreen(ST->ConOut);
    ST->ConOut->SetAttribute(ST->ConOut, 0x0D);             /* light magenta */
    print("Terminal OS\n");
    ST->ConOut->SetAttribute(ST->ConOut, 0x07);

    static const EFI_GUID li_guid = LOADED_IMAGE_GUID, fs_guid = SIMPLE_FS_GUID, gop_guid = GOP_GUID;
    static const EFI_GUID acpi20 = ACPI20_GUID, acpi10 = ACPI10_GUID;
    EFI_LOADED_IMAGE_PROTOCOL *li;
    EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *fs;
    EFI_STATUS s = BS->HandleProtocol(image, &li_guid, (void **)&li);
    if (EFI_ERROR(s)) return fail("no loaded-image information", s);
    s = BS->HandleProtocol(li->DeviceHandle, &fs_guid, (void **)&fs);
    if (EFI_ERROR(s)) return fail("cannot read the partition it was started from", s);
    s = fs->OpenVolume(fs, &root);
    if (EFI_ERROR(s)) return fail("cannot open the partition", s);

    /* The files. */
    uint64_t kernel_len, gz_len, efi_len;
    uint8_t *kernel_file = read_file(KERNEL_DIR L"KERNEL.BIN", &kernel_len);
    if (!kernel_file) return fail("\\EFI\\TERMINALOS\\KERNEL.BIN is missing", 0);
    uint8_t *gz = read_file(KERNEL_DIR L"RAMDISK.GZ", &gz_len);
    if (!gz) return fail("\\EFI\\TERMINALOS\\RAMDISK.GZ is missing", 0);
    uint8_t *efi_file = read_file(KERNEL_DIR L"TERMOS.EFI", &efi_len);   /* for the installer */

    kernel_t k;
    const char *err = load_kernel(kernel_file, kernel_len, &k);
    if (err) return fail(err, 0);
    /* The kernel goes at 1 MiB, its page tables at 4-6 MiB. */
    if (!free_after_exit(k.base, (k.base + k.size > 6 * MIB ? k.base + k.size : 6 * MIB)))
        return fail("the firmware keeps memory at 1-6 MiB, where the kernel goes", 0);

    print("Unpacking the system files...\n");
    uint32_t rd_len = gzip_size(gz, gz_len);
    uint8_t *ramdisk = rd_len ? alloc_high(rd_len) : 0;
    if (!ramdisk) return fail("no memory for the RAM disk", 0);
    if (gunzip(gz, gz_len, ramdisk, rd_len) != 0) return fail("RAMDISK.GZ is damaged", 0);

    /* The installer's copies of the three files (they go onto the SSD). */
    uint8_t *inst_kernel = alloc_high(kernel_len), *inst_gz = alloc_high(gz_len);
    uint8_t *inst_efi = efi_file ? alloc_high(efi_len) : 0;
    if (inst_kernel) memcpy(inst_kernel, kernel_file, kernel_len);
    if (inst_gz) memcpy(inst_gz, gz, gz_len);
    if (inst_efi) memcpy(inst_efi, efi_file, efi_len);
    BS->FreePool(kernel_file);
    BS->FreePool(gz);
    if (efi_file) BS->FreePool(efi_file);

    /* Options: S within a second boots on one core. */
    char cmdline[160];
    boot_device(li, cmdline, sizeof(cmdline) - 8);
    print("Press S for safe mode (one CPU core)...\n");
    for (int i = 0; i < 10; i++) {
        EFI_INPUT_KEY key;
        if (!EFI_ERROR(ST->ConIn->ReadKeyStroke(ST->ConIn, &key)) && (key.UnicodeChar | 0x20) == 's') {
            size_t n = cstrlen(cmdline);
            memcpy(cmdline + n, "nosmp", 6);
            print("Safe mode.\n");
            break;
        }
        BS->Stall(100000);
    }

    /* The boot information. */
    info_cap = 64 * 1024;
    info = alloc_high(info_cap);
    uint64_t cr3 = make_page_tables();
    uint8_t *tramp = alloc_high(4096);
    if (!info || !cr3 || !tramp) return fail("no memory for the hand-over", 0);
    info_len = 8;
    size_t cl = cstrlen(cmdline) + 1;
    uint8_t *t = tag(1, (uint32_t)(8 + cl));
    if (t) memcpy(t + 8, cmdline, cl);
    tag_module(ramdisk, rd_len, "ramdisk");
    if (inst_kernel) tag_module(inst_kernel, kernel_len, "install-kernel");
    if (inst_gz) tag_module(inst_gz, gz_len, "install-ramdisk");
    if (inst_efi) tag_module(inst_efi, efi_len, "install-loader");

    EFI_GRAPHICS_OUTPUT_PROTOCOL *gop;
    if (!EFI_ERROR(BS->LocateProtocol(&gop_guid, 0, (void **)&gop)) && gop->Mode && gop->Mode->Info &&
        gop->Mode->Info->PixelFormat != PixelBltOnly) {
        EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *mi = gop->Mode->Info;
        uint8_t *fb = tag(8, 38);
        if (fb) {
            *(uint64_t *)(fb + 8) = gop->Mode->FrameBufferBase;
            *(uint32_t *)(fb + 16) = mi->PixelsPerScanLine * 4;
            *(uint32_t *)(fb + 20) = mi->HorizontalResolution;
            *(uint32_t *)(fb + 24) = mi->VerticalResolution;
            fb[28] = 32;
            fb[29] = 1;                                     /* RGB */
            uint8_t rpos = 16, gpos = 8, bpos = 0;          /* blue, green, red in memory order */
            if (mi->PixelFormat == PixelRedGreenBlueReserved8BitPerColor) { rpos = 0; bpos = 16; }
            else if (mi->PixelFormat == PixelBitMask) {
                rpos = (uint8_t)__builtin_ctz(mi->RedMask);
                gpos = (uint8_t)__builtin_ctz(mi->GreenMask);
                bpos = (uint8_t)__builtin_ctz(mi->BlueMask);
            }
            fb[32] = rpos; fb[33] = 8; fb[34] = gpos; fb[35] = 8; fb[36] = bpos; fb[37] = 8;
        }
    }

    /* ACPI's root pointer: the 2.0 one (with the XSDT) if there is one. */
    int have_rsdp = 0;
    for (uint64_t i = 0; i < ST->NumberOfTableEntries && !have_rsdp; i++) {
        EFI_CONFIGURATION_TABLE *c = &ST->ConfigurationTable[i];
        if (guid_eq(&c->VendorGuid, &acpi20)) {
            uint8_t *r = tag(15, 8 + 36);
            if (r) memcpy(r + 8, c->VendorTable, 36);
            have_rsdp = 1;
        }
    }
    for (uint64_t i = 0; i < ST->NumberOfTableEntries && !have_rsdp; i++) {
        EFI_CONFIGURATION_TABLE *c = &ST->ConfigurationTable[i];
        if (guid_eq(&c->VendorGuid, &acpi10)) {
            uint8_t *r = tag(14, 8 + 20);
            if (r) memcpy(r + 8, c->VendorTable, 20);
            have_rsdp = 1;
        }
    }
    uint64_t *et = tag(12, 16);                             /* the EFI system table */
    if (et) et[1] = (uint64_t)ST;

    jump_params_t *jp = (jump_params_t *)(tramp + 256);
    memcpy(tramp, trampoline, sizeof(trampoline));
    jp->dst = k.base;
    jp->src = (uint64_t)k.image;
    jp->len = k.size;
    jp->entry = k.entry64;
    jp->info = (uint64_t)info;
    jp->cr3 = cr3;

    print("Starting...\n");
    /* Leave the firmware. The map must be fresh: retry if it changed. */
    uint32_t keep = info_len;
    for (int attempt = 0;; attempt++) {
        s = get_map();
        if (EFI_ERROR(s)) return fail("cannot read the memory map", s);
        s = BS->ExitBootServices(image, map_key);
        if (!EFI_ERROR(s)) break;
        if (attempt == 3) return fail("the firmware would not hand over", s);
    }
    /* No firmware calls from here on. */
    info_len = keep;
    tag_memory_map();
    tag_end();
    ((void (EFIAPI *)(jump_params_t *))tramp)(jp);
    for (;;) __asm__ volatile("hlt");
}
