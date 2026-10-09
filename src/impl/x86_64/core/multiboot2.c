#include "core/multiboot2.h"
#include <stdint.h>

#define MB2_TAG_END         0
#define MB2_TAG_FRAMEBUFFER 8
#define MB2_FB_TYPE_RGB     1

int mb2_get_framebuffer(fb_info_t *out) {
    if (!multiboot_info) return -1;
    const uint8_t *base = (const uint8_t *)(uintptr_t)multiboot_info;
    uint32_t total = *(const uint32_t *)base;
    uint32_t off = 8;                                   /* total_size, reserved */
    while (off + 8 <= total) {
        const uint8_t *tag = base + off;
        uint32_t type = *(const uint32_t *)tag, size = *(const uint32_t *)(tag + 4);
        if (type == MB2_TAG_END || size < 8) break;
        if (type == MB2_TAG_FRAMEBUFFER && size >= 38 && tag[29] == MB2_FB_TYPE_RGB) {
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
        off += (size + 7) & ~7u;                        /* tags are 8-byte aligned */
    }
    return -1;
}
