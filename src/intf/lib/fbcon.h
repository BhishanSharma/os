#ifndef FBCON_H
#define FBCON_H
#include <stdint.h>

/* Linear framebuffer as reported by the bootloader (multiboot2 tag 8). */
typedef struct {
    uint64_t addr;
    uint32_t pitch, width, height;
    uint8_t  bpp;
    uint8_t  red_pos, red_size, green_pos, green_size, blue_pos, blue_size;
} fb_info_t;

/* Text console drawn into a framebuffer: a grid of VGA-style cells (CP437
 * character + attribute byte) rendered with an 8x16 font. The font is scaled by
 * the largest whole number that still fits 80x25 characters, and the grid fills
 * the rest of the screen (e.g. 100x37 at 800x600, 120x33 at 1920x1080). */
#define FBCON_MAX_COLS 256
#define FBCON_MAX_ROWS 100

/* Grid size fbcon_init() will use for this framebuffer; needs no mapping. */
void fbcon_grid_size(const fb_info_t *fb, uint32_t *cols, uint32_t *rows);

/* The framebuffer must already be mapped. Returns 0 on success, -1 for an
 * unsupported pixel format or a screen too small for a usable grid. */
int  fbcon_init(const fb_info_t *fb);
int  fbcon_active(void);

/* Draw one cell; `cursor` draws an underline cursor in it. */
void fbcon_draw_cell(int col, int row, uint8_t ch, uint8_t attr, int cursor);

#endif
