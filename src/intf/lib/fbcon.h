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
 * character + attribute byte) rendered with an 8x16 font, scaled up only on
 * big screens (more than 160 columns at x1); the grid fills the screen
 * (e.g. 100x37 at 800x600, 160x50 at 1280x800, 120x33 at 1920x1080). */
#define FBCON_MAX_COLS 256
#define FBCON_MAX_ROWS 100

/* Grid size fbcon_init() will use for this framebuffer; needs no mapping. */
void fbcon_grid_size(const fb_info_t *fb, uint32_t *cols, uint32_t *rows);

/* The framebuffer must already be mapped. Returns 0 on success, -1 for an
 * unsupported pixel format or a screen too small for a usable grid. */
int  fbcon_init(const fb_info_t *fb);
int  fbcon_active(void);

/* Font scale: 1-4, or 0 for automatic (x1, larger past 160 columns). Takes
 * effect at the next fbcon_init. */
void fbcon_set_scale(uint32_t scale);
uint32_t fbcon_get_scale(void);

/* Pixel access for programs (sys/process.c, the `gfx` system call):
 * screen size, the size of one text cell and where the grid starts. */
void fbcon_geometry(uint32_t *width, uint32_t *height, uint32_t *cell_w, uint32_t *cell_h,
                    uint32_t *grid_x, uint32_t *grid_y);
void fbcon_fill(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t rgb);   /* 0x00RRGGBB */
void fbcon_blit(int x, int y, int w, int h, const uint32_t *src, int stride);

/* Draw one cell; `cursor` draws an underline cursor in it. */
void fbcon_draw_cell(int col, int row, uint8_t ch, uint8_t attr, int cursor);

#endif
