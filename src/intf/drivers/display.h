// display.h - changing the screen mode, following the VM window
//
// GRUB picks the screen mode at boot. On the Bochs-style graphics adapter that
// QEMU (-vga std) and VirtualBox emulate, the mode can be changed at any time
// through its VBE "DISPI" registers; the console then gets more (or fewer)
// rows and columns. Under VirtualBox the OS also asks the VirtualBox guest
// device for the size of the VM window (what Guest Additions do), so resizing
// the window resizes the console.
#ifndef DISPLAY_H
#define DISPLAY_H

#include <stdint.h>
#include "lib/fbcon.h"

/* Before paging_init: find the graphics adapter that owns the boot
 * framebuffer. Returns how many bytes from fb->addr to identity-map (all of
 * video memory if it can be resized, so larger modes fit). */
uint64_t display_early_init(const fb_info_t *fb);

/* After the framebuffer console is up: talk to VirtualBox, if there. */
void display_init(void);

int display_can_resize(void);
const char *display_adapter_name(void);   /* "Bochs VBE (QEMU)", "VirtualBox VBE", ... */
int display_follows_window(void);         /* 1 under VirtualBox with the guest device */
void display_get_mode(uint32_t *width, uint32_t *height);

/* Switch to width x height, 32 bpp, and resize the console. 0 on success. */
int display_set_mode(uint32_t width, uint32_t height);

/* Font scale 1-4 (0 = automatic) and redraw. */
int display_set_font_scale(uint32_t scale);

/* Called often while idle: apply a new VirtualBox window size. */
void display_poll(void);

#endif
