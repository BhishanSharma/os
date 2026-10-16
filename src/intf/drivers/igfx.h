// igfx.h - Intel integrated graphics (Gen12/13 display: Tiger Lake, Alder
// Lake, Raptor Lake): the hardware cursor and page flipping on the pipe the
// firmware set up. No mode setting: the firmware's mode stays.
#ifndef IGFX_H
#define IGFX_H

#include <stdint.h>
#include "lib/fbcon.h"

/* Find the GPU and the pipe showing `fb`. 0 if both features can be used. */
int igfx_init(const fb_info_t *fb);
void igfx_engines_probe(void);   /* the render and blitter engines' state, into the system log */
int igfx_blitter_test(void);      /* step 2: one command on the blitter (into the log) -> 0 if it ran */
const char *igfx_blitter_status(void);
int igfx_blitter_ready(void);
/* Screen buffers in RAM: 1 and 2 (0 is the firmware's). */
uint32_t *igfx_buffer(int i);
/* The blitter copies rectangles (x0, y0, x1, y1 each) from the shadow (the
 * screen's size, tightly packed) into buffer 1 or 2 -> 0 when done, -1: copy
 * them with the CPU (the blitter is off or out of room). */
int igfx_copy(const uint32_t *shadow, int buffer, const int *rects, int n);
int igfx_copy_prepare(const uint32_t *shadow);
int igfx_copy_source(const void *token, const uint64_t *pages, int n, uint32_t offset, uint32_t pitch_bytes);   /* first, under the kernel lock -> 0 ready */
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
