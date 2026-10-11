// igfx.h - Intel integrated graphics (Gen12/13 display: Tiger Lake, Alder
// Lake, Raptor Lake): the hardware cursor and page flipping on the pipe the
// firmware set up. No mode setting: the firmware's mode stays.
#ifndef IGFX_H
#define IGFX_H

#include <stdint.h>
#include "lib/fbcon.h"

/* Find the GPU and the pipe showing `fb`. 0 if both features can be used. */
int igfx_init(const fb_info_t *fb);
const char *igfx_status(void);          /* for the boot screen and `sysinfo` */

/* The hardware cursor: a picture of at most 64x64 (0xAARRGGBB, straight
 * alpha), hot spot (hx, hy). -> 0, or -1 (then draw it in software). */
int igfx_cursor_set(const uint32_t *argb, int w, int h, int hx, int hy, int x, int y);
void igfx_cursor_move(int x, int y);
void igfx_cursor_off(void);

/* Page flipping: a second buffer in RAM the size of the screen. */
uint32_t *igfx_back_buffer(void);       /* CPU address (write-combining), 0 if none */
void igfx_flip(int buffer);             /* 0 = the firmware's framebuffer, 1 = ours; at the next refresh */
int igfx_flip_done(void);               /* 1 once the last flip is on the screen */

#endif
