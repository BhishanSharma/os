#include "lib/fbcon.h"
#include <stdint.h>

#define GLYPH_W 8
#define GLYPH_H 16

extern const uint8_t font8x16[256][16];

/* The 16 VGA text colours. */
static const uint8_t vga_rgb[16][3] = {
    {0x00, 0x00, 0x00}, {0x00, 0x00, 0xAA}, {0x00, 0xAA, 0x00}, {0x00, 0xAA, 0xAA},
    {0xAA, 0x00, 0x00}, {0xAA, 0x00, 0xAA}, {0xAA, 0x55, 0x00}, {0xAA, 0xAA, 0xAA},
    {0x55, 0x55, 0x55}, {0x55, 0x55, 0xFF}, {0x55, 0xFF, 0x55}, {0x55, 0xFF, 0xFF},
    {0xFF, 0x55, 0x55}, {0xFF, 0x55, 0xFF}, {0xFF, 0xFF, 0x55}, {0xFF, 0xFF, 0xFF},
};

static fb_info_t fb;
static int active;
static uint32_t scale, origin_x, origin_y, text_cols, text_rows;
static uint32_t palette[16];

static uint32_t pack(uint8_t r, uint8_t g, uint8_t b) {
    return ((uint32_t)(r >> (8 - fb.red_size)) << fb.red_pos) |
           ((uint32_t)(g >> (8 - fb.green_size)) << fb.green_pos) |
           ((uint32_t)(b >> (8 - fb.blue_size)) << fb.blue_pos);
}

static void put_pixel(uint32_t x, uint32_t y, uint32_t value) {
    uint8_t *p = (uint8_t *)(uintptr_t)fb.addr + (uint64_t)y * fb.pitch + (uint64_t)x * (fb.bpp / 8);
    if (fb.bpp == 32) *(volatile uint32_t *)p = value;
    else { p[0] = (uint8_t)value; p[1] = (uint8_t)(value >> 8); p[2] = (uint8_t)(value >> 16); }
}

static uint32_t forced_scale;   /* 0: automatic */

void fbcon_set_scale(uint32_t s) { forced_scale = s > 4 ? 4 : s; }
uint32_t fbcon_get_scale(void) { return scale; }

static uint32_t pick_scale(const fb_info_t *info) {
    if (forced_scale) return forced_scale;
    /* Normal size (x1), made larger only on big screens, where the text would
     * otherwise be more than 160 columns wide and tiny: 1920x1080 gives 120x33
     * at x2, 1280x800 gives 160x50 at x1. Never less than 80x25. */
    uint32_t s = 1;
    while (info->width / (GLYPH_W * s) > 160 && info->width / (GLYPH_W * (s + 1)) >= 80 &&
           info->height / (GLYPH_H * (s + 1)) >= 25)
        s++;
    return s;
}

void fbcon_grid_size(const fb_info_t *info, uint32_t *cols, uint32_t *rows) {
    uint32_t s = pick_scale(info);
    *cols = info->width / (GLYPH_W * s);
    *rows = info->height / (GLYPH_H * s);
    if (*cols > FBCON_MAX_COLS) *cols = FBCON_MAX_COLS;
    if (*rows > FBCON_MAX_ROWS) *rows = FBCON_MAX_ROWS;
}

int fbcon_init(const fb_info_t *info) {
    if ((info->bpp != 32 && info->bpp != 24) || !info->red_size || !info->green_size || !info->blue_size)
        return -1;
    fb = *info;
    scale = pick_scale(&fb);
    fbcon_grid_size(&fb, &text_cols, &text_rows);
    if (text_cols < 40 || text_rows < 10) return -1;
    origin_x = (fb.width - text_cols * GLYPH_W * scale) / 2;
    origin_y = (fb.height - text_rows * GLYPH_H * scale) / 2;
    for (int i = 0; i < 16; i++) palette[i] = pack(vga_rgb[i][0], vga_rgb[i][1], vga_rgb[i][2]);

    for (uint32_t y = 0; y < fb.height; y++)
        for (uint32_t x = 0; x < fb.width; x++) put_pixel(x, y, palette[0]);
    active = 1;
    return 0;
}

int fbcon_active(void) { return active; }

void fbcon_draw_cell(int col, int row, uint8_t ch, uint8_t attr, int cursor) {
    if (!active || col < 0 || (uint32_t)col >= text_cols || row < 0 || (uint32_t)row >= text_rows) return;
    uint32_t fg = palette[attr & 0x0F], bg = palette[(attr >> 4) & 0x0F];
    uint32_t x0 = origin_x + (uint32_t)col * GLYPH_W * scale;
    uint32_t y0 = origin_y + (uint32_t)row * GLYPH_H * scale;
    for (uint32_t gy = 0; gy < GLYPH_H; gy++) {
        uint8_t bits = font8x16[ch][gy];
        if (cursor && gy >= GLYPH_H - 2) bits = 0xFF;
        for (uint32_t sy = 0; sy < scale; sy++) {
            uint32_t y = y0 + gy * scale + sy;
            for (uint32_t gx = 0; gx < GLYPH_W; gx++) {
                uint32_t v = (bits & (0x80 >> gx)) ? fg : bg;
                for (uint32_t sx = 0; sx < scale; sx++) put_pixel(x0 + gx * scale + sx, y, v);
            }
        }
    }
}
