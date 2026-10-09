#ifndef MULTIBOOT2_H
#define MULTIBOOT2_H
#include <stdint.h>
#include "lib/fbcon.h"

/* Physical address of the multiboot2 information structure, saved from EBX by
 * the boot code (main.asm). It lives in memory the kernel later reuses (heap,
 * page tables), so read what you need from it early in kernel_main. */
extern uint32_t multiboot_info;

/* Find the framebuffer tag. Returns 0 and fills `out` for an RGB framebuffer,
 * -1 if there is none (e.g. GRUB left the machine in VGA text mode). */
int mb2_get_framebuffer(fb_info_t *out);

typedef struct {
    uint64_t base, length;
} mb2_region_t;

/* Copy up to `max` usable-RAM regions from the memory map (tag 6). Returns the
 * number copied, or -1 if the bootloader gave no memory map. */
int mb2_get_memory_map(mb2_region_t *out, int max);

/* First boot module (grub.cfg `module2`) whose command line is `name`. Returns
 * 0 and its physical range [start, end), or -1 if there is no such module. */
int mb2_get_module(const char *name, uint64_t *start, uint64_t *end);

#endif
