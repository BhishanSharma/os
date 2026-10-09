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

#endif
