#include "core/multiboot2.h"
#include "lib/string.h"
#include <stdint.h>

#define MB2_TAG_END         0
#define MB2_TAG_MODULE      3
#define MB2_TAG_MMAP        6
#define MB2_TAG_FRAMEBUFFER 8
#define MB2_FB_TYPE_RGB     1
#define MB2_MEMORY_AVAILABLE 1

/* Next tag of `type` after `prev` (0: from the start), or 0. Tags follow an
 * 8-byte header and are 8-byte aligned. */
static const uint8_t *find_next_tag(uint32_t want, const uint8_t *prev) {
    if (!multiboot_info) return 0;
    const uint8_t *base = (const uint8_t *)(uintptr_t)multiboot_info;
    uint32_t total = *(const uint32_t *)base;
    uint32_t off = 8;
    if (prev) off = (uint32_t)(prev - base) + ((*(const uint32_t *)(prev + 4) + 7) & ~7u);
    while (off + 8 <= total) {
        const uint8_t *tag = base + off;
        uint32_t type = *(const uint32_t *)tag, size = *(const uint32_t *)(tag + 4);
        if (type == MB2_TAG_END || size < 8) break;
        if (type == want) return tag;
        off += (size + 7) & ~7u;
    }
    return 0;
}

static const uint8_t *find_tag(uint32_t want) { return find_next_tag(want, 0); }

int mb2_get_framebuffer(fb_info_t *out) {
    const uint8_t *tag = find_tag(MB2_TAG_FRAMEBUFFER);
    if (!tag || *(const uint32_t *)(tag + 4) < 38 || tag[29] != MB2_FB_TYPE_RGB) return -1;
    out->addr   = *(const uint64_t *)(tag + 8);
    out->pitch  = *(const uint32_t *)(tag + 16);
    out->width  = *(const uint32_t *)(tag + 20);
    out->height = *(const uint32_t *)(tag + 24);
    out->bpp    = tag[28];
    out->red_pos = tag[32];   out->red_size = tag[33];
    out->green_pos = tag[34]; out->green_size = tag[35];
    out->blue_pos = tag[36];  out->blue_size = tag[37];
    return 0;
}

int mb2_get_memory_map(mb2_region_t *out, int max) {
    const uint8_t *tag = find_tag(MB2_TAG_MMAP);
    if (!tag) return -1;
    uint32_t size = *(const uint32_t *)(tag + 4);
    uint32_t entry_size = *(const uint32_t *)(tag + 8);
    if (entry_size < 24) return -1;
    int n = 0;
    for (uint32_t off = 16; off + entry_size <= size && n < max; off += entry_size) {
        const uint8_t *e = tag + off;
        if (*(const uint32_t *)(e + 16) != MB2_MEMORY_AVAILABLE) continue;
        out[n].base = *(const uint64_t *)e;
        out[n].length = *(const uint64_t *)(e + 8);
        n++;
    }
    return n;
}

int mb2_get_module(const char *name, uint64_t *start, uint64_t *end) {
    for (const uint8_t *tag = find_next_tag(MB2_TAG_MODULE, 0); tag;
         tag = find_next_tag(MB2_TAG_MODULE, tag)) {
        if (*(const uint32_t *)(tag + 4) < 17) continue;
        const char *cmdline = (const char *)(tag + 16);
        int i = 0;
        while (name[i] && cmdline[i] == name[i]) i++;
        if (name[i] || (cmdline[i] && cmdline[i] != ' ')) continue;
        *start = *(const uint32_t *)(tag + 8);
        *end = *(const uint32_t *)(tag + 12);
        return *end > *start ? 0 : -1;
    }
    return -1;
}

/* The firmware's ACPI root pointer: GRUB copies it into tag 15 (ACPI 2.0+,
 * with the XSDT) or tag 14 (ACPI 1.0). */
const void *mb2_get_rsdp(void) {
    const uint8_t *tag = find_tag(15);
    if (!tag) tag = find_tag(14);
    return tag ? tag + 8 : 0;
}

/* GRUB passes the EFI system table (tag 11 for 32-bit, 12 for 64-bit EFI)
 * only when the firmware is UEFI. */
int mb2_booted_from_uefi(void) {
    return find_tag(11) != 0 || find_tag(12) != 0;
}

/* Is `word` one of the words on the kernel's command line (tag 1)? */
int mb2_cmdline_has(const char *word) {
    const uint8_t *tag = find_tag(1);
    if (!tag) return 0;
    const char *p = (const char *)(tag + 8);
    size_t n = strlen(word);
    while (*p) {
        while (*p == ' ') p++;
        const char *start = p;
        while (*p && *p != ' ') p++;
        if ((size_t)(p - start) == n && !memcmp(start, word, n)) return 1;
    }
    return 0;
}
