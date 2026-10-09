#include "core/multiboot2.h"
#include <stdint.h>

#define MB2_TAG_END         0
#define MB2_TAG_MMAP        6
#define MB2_TAG_FRAMEBUFFER 8
#define MB2_FB_TYPE_RGB     1
#define MB2_MEMORY_AVAILABLE 1

/* First tag of `type`, or 0. Tags follow an 8-byte header and are 8-byte aligned. */
static const uint8_t *find_tag(uint32_t want) {
    if (!multiboot_info) return 0;
    const uint8_t *base = (const uint8_t *)(uintptr_t)multiboot_info;
    uint32_t total = *(const uint32_t *)base;
    uint32_t off = 8;
    while (off + 8 <= total) {
        const uint8_t *tag = base + off;
        uint32_t type = *(const uint32_t *)tag, size = *(const uint32_t *)(tag + 4);
        if (type == MB2_TAG_END || size < 8) break;
        if (type == want) return tag;
        off += (size + 7) & ~7u;
    }
    return 0;
}

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
