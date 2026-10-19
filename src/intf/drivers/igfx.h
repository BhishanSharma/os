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
int igfx_render_test(void);       /* GPU driver stage 1: one command on the render engine (into the log) */
const char *igfx_render_status(void);
/* GPU driver stages 2-3: shaders on the render engine (gpu/shaders.cl), each
 * checked against the CPU at boot (into the log). -> 0: the desktop may use them. */
int igfx_gpu_init(void);
int igfx_gpu_ready(void);
int igfx_panel_bpc(void);                /* the pipe's bits a colour: 6, 8, 10, 12 (8 if unknown) */
const char *igfx_gpu_status(void);
/* For gfx GFX_GPU, in the calling program's address space (`root`). -> 0 done. */
int igfx_gpu_material(uint64_t root, uint32_t *samp, uint32_t *scratch, int bw, int bh,
                      uint32_t sat, uint32_t tint, uint32_t alpha, uint32_t *out, int w, int j0, int j1, int bleed);
int igfx_gpu_fade(uint64_t root, const uint32_t *old, uint32_t *out, int w, int h, int pitch, uint32_t f);
/* gfx GFX_GPU_DRAW (sys/syscall_nums.h); ok(ptr, bytes, write): the program may use that range. */
struct os_gpu_draws;
int igfx_gpu_draw(uint64_t root, const struct os_gpu_draws *d, int (*ok)(uint64_t, uint64_t, int));
const char *igfx_blitter_status(void);
int igfx_blitter_ready(void);
/* Screen buffers in RAM: 1 and 2 (0 is the firmware's). */
uint32_t *igfx_buffer(int i);
/* A window's contents into the desktop's frame, before the frame is copied:
 * from src_va (igfx_map_user) at (sx, sy), rows src_pitch bytes apart, to
 * (x, y) in the frame, w x h. */
struct igfx_pre { uint32_t src_va, src_pitch; int x, y, w, h, sx, sy; };
/* One batch: the `pre` copies, then rectangles (x0, y0, x1, y1 each) from the
 * frame (`src`: the shadow or the desktop's surface) into buffer 1 or 2.
 * -> 0 and its number in *seq (not waited for); -1: copy with the CPU. */
int igfx_copy_submit(const uint32_t *src, int buffer, const int *rects, int n,
                     const struct igfx_pre *pre, int npre, uint32_t *seq);
int igfx_copy_done(uint32_t seq);           /* 1 once batch `seq` has finished */
void igfx_copy_idle(void);                  /* done: the GPU may sleep */
void igfx_copy_failed(void);                /* it did not finish: the blitter is turned off */
/* A program's memory [va, va + bytes) (its page tables at `root`) for the
 * blitter -> the address the blitter uses for `va`, 0 if it cannot. */
uint32_t igfx_map_user(uint64_t root, uint64_t va, uint64_t bytes);
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
