#include "lib/fbcon.h"
#include <stdint.h>
#include "drivers/addr.h"
#include "drivers/paging.h"
#include "drivers/heap.h"
#include "lib/string.h"

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
static uint8_t *fb_base;                 /* the screen, mapped write-combining */
static int active;
static uint32_t scale, origin_x, origin_y, text_cols, text_rows;
static uint32_t palette[16];

static uint32_t pack(uint8_t r, uint8_t g, uint8_t b) {
    return ((uint32_t)(r >> (8 - fb.red_size)) << fb.red_pos) |
           ((uint32_t)(g >> (8 - fb.green_size)) << fb.green_pos) |
           ((uint32_t)(b >> (8 - fb.blue_size)) << fb.blue_pos);
}

static void put_pixel(uint32_t x, uint32_t y, uint32_t value) {
    uint8_t *p = fb_base + (uint64_t)y * fb.pitch + (uint64_t)x * (fb.bpp / 8);
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
    paging_pat_init();
    fb_base = mmio_map_wc(fb.addr, (uint64_t)fb.pitch * fb.height);
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

/* ---- Pixels for programs (image viewer) ---------------------------------- */

void fbcon_geometry(uint32_t *width, uint32_t *height, uint32_t *cell_w, uint32_t *cell_h,
                    uint32_t *grid_x, uint32_t *grid_y) {
    *width = active ? fb.width : 0;
    *height = active ? fb.height : 0;
    *cell_w = GLYPH_W * scale;
    *cell_h = GLYPH_H * scale;
    *grid_x = origin_x;
    *grid_y = origin_y;
}

void fbcon_fill(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t rgb) {
    if (!active || x >= fb.width || y >= fb.height) return;
    if (w > fb.width - x) w = fb.width - x;
    if (h > fb.height - y) h = fb.height - y;
    uint32_t v = pack((uint8_t)(rgb >> 16), (uint8_t)(rgb >> 8), (uint8_t)rgb);
    for (uint32_t j = 0; j < h; j++)
        for (uint32_t i = 0; i < w; i++) put_pixel(x + i, y + j, v);
}

/* `w` x `h` pixels of 0x00RRGGBB from `src` (rows `stride` pixels apart) at
 * (x, y), clipped to the screen. */
static int cursor_on(void);
static void cursor_blit(int x, int y, int w, int h, const uint32_t *src, int stride);

void fbcon_blit(int x, int y, int w, int h, const uint32_t *src, int stride) {
    if (!active) return;
    if (cursor_on()) {
        cursor_blit(x, y, w, h, src, stride);
        return;
    }
    if (fb.bpp == 32 && fb.red_pos == 16 && fb.green_pos == 8 && fb.blue_pos == 0) {
        /* The screen holds 0x00RRGGBB too: copy whole rows (video needs the speed). */
        int x0 = x < 0 ? 0 : x, x1 = x + w > (int)fb.width ? (int)fb.width : x + w;
        if (x1 <= x0) return;
        for (int j = 0; j < h; j++) {
            int sy = y + j;
            if (sy < 0 || (uint32_t)sy >= fb.height) continue;
            const uint32_t *from = src + (int64_t)j * stride + (x0 - x);
            uint32_t *to = (uint32_t *)(fb_base + (uint64_t)sy * fb.pitch) + x0;
            uint64_t n = (uint64_t)(x1 - x0);
            __asm__ volatile("rep movsl" : "+D"(to), "+S"(from), "+c"(n) : : "memory");
        }
        return;
    }
    for (int j = 0; j < h; j++) {
        int sy = y + j;
        if (sy < 0 || (uint32_t)sy >= fb.height) continue;
        const uint32_t *row = src + (int64_t)j * stride;
        for (int i = 0; i < w; i++) {
            int sx = x + i;
            if (sx < 0 || (uint32_t)sx >= fb.width) continue;
            uint32_t c = row[i];
            put_pixel((uint32_t)sx, (uint32_t)sy, pack((uint8_t)(c >> 16), (uint8_t)(c >> 8), (uint8_t)c));
        }
    }
}

/* ---- The pointer, drawn by the kernel ---------------------------------------
 * Like a graphics card's cursor plane: the desktop hands over the pointer's
 * picture once, and the mouse driver moves it on every timer interrupt
 * (1 kHz), whatever the desktop is doing. A copy of the screen without the
 * pointer (the shadow) gives back what was under it, without reading the
 * framebuffer (slow, it is write-combining). */

#define CUR_MAX 64
static uint32_t *shadow;
static uint32_t cur_rgb[CUR_MAX * CUR_MAX];
static uint8_t cur_alpha[CUR_MAX * CUR_MAX];
static int cur_w, cur_h, cur_hx, cur_hy, cur_x, cur_y, cur_shown;
static volatile int fb_busy, cur_pending, cur_px, cur_py;

static int cursor_on(void) { return cur_shown && shadow; }
int fbcon_cursor_active(void) { return cursor_on(); }

static inline uint32_t blend_rgb(uint32_t d, uint32_t c, uint32_t a) {
    uint32_t na = 255 - a;
    uint32_t r = (((c >> 16) & 255) * a + ((d >> 16) & 255) * na + 127) / 255;
    uint32_t g = (((c >> 8) & 255) * a + ((d >> 8) & 255) * na + 127) / 255;
    uint32_t b = ((c & 255) * a + (d & 255) * na + 127) / 255;
    return r << 16 | g << 8 | b;
}

static int fast_format(void) { return fb.bpp == 32 && fb.red_pos == 16 && fb.green_pos == 8 && fb.blue_pos == 0; }

/* The screen in a rectangle, from the shadow, with the pointer on top. */
static void repaint(int x, int y, int w, int h) {
    int x0 = x < 0 ? 0 : x, y0 = y < 0 ? 0 : y;
    int x1 = x + w > (int)fb.width ? (int)fb.width : x + w, y1 = y + h > (int)fb.height ? (int)fb.height : y + h;
    if (x1 <= x0 || y1 <= y0) return;
    int cx = cur_x - cur_hx, cy = cur_y - cur_hy;
    for (int j = y0; j < y1; j++) {
        const uint32_t *row = shadow + (uint64_t)j * fb.width;
        int cj = j - cy, over = cur_shown && cj >= 0 && cj < cur_h;
        if (fast_format() && !over) {
            const uint32_t *from = row + x0;
            uint32_t *to = (uint32_t *)(fb_base + (uint64_t)j * fb.pitch) + x0;
            uint64_t n = (uint64_t)(x1 - x0);
            __asm__ volatile("rep movsl" : "+D"(to), "+S"(from), "+c"(n) : : "memory");
            continue;
        }
        for (int i = x0; i < x1; i++) {
            uint32_t c = row[i];
            int ci = i - cx;
            if (over && ci >= 0 && ci < cur_w) {
                uint32_t a = cur_alpha[cj * CUR_MAX + ci];
                if (a) c = blend_rgb(c, cur_rgb[cj * CUR_MAX + ci], a);
            }
            if (fast_format()) ((uint32_t *)(fb_base + (uint64_t)j * fb.pitch))[i] = c;
            else put_pixel((uint32_t)i, (uint32_t)j, pack((uint8_t)(c >> 16), (uint8_t)(c >> 8), (uint8_t)c));
        }
    }
}

static void cursor_blit(int x, int y, int w, int h, const uint32_t *src, int stride) {
    fb_busy = 1;
    int x0 = x < 0 ? 0 : x, x1 = x + w > (int)fb.width ? (int)fb.width : x + w;
    if (x1 > x0)
        for (int j = 0; j < h; j++) {
            int sy = y + j;
            if (sy < 0 || (uint32_t)sy >= fb.height) continue;
            memcpy(shadow + (uint64_t)sy * fb.width + x0, src + (int64_t)j * stride + (x0 - x), (size_t)(x1 - x0) * 4);
        }
    repaint(x, y, w, h);
    fb_busy = 0;
    if (cur_pending) {
        cur_pending = 0;
        fbcon_cursor_move(cur_px, cur_py);
    }
}

/* The pointer's picture: 0xAARRGGBB, at most 64 x 64, hot spot (hx, hy).
 * w = 0 takes it away (and frees the shadow). -> 0, or -1. */
int fbcon_cursor_set(const uint32_t *argb, int w, int h, int hx, int hy, int x, int y) {
    if (!active) return -1;
    if (w <= 0) {
        cur_shown = 0;
        if (shadow) {
            kfree(shadow);
            shadow = 0;
        }
        return 0;
    }
    if (w > CUR_MAX || h > CUR_MAX) return -1;
    if (!shadow) {
        shadow = kmalloc((uint64_t)fb.width * fb.height * 4);
        if (!shadow) return -1;
        memset(shadow, 0, (uint64_t)fb.width * fb.height * 4);
    }
    int ox = cur_x - cur_hx, oy = cur_y - cur_hy, ow = cur_w, oh = cur_h;
    fb_busy = 1;
    for (int j = 0; j < h; j++)
        for (int i = 0; i < w; i++) {
            uint32_t p = argb[j * w + i];
            cur_rgb[j * CUR_MAX + i] = p & 0xFFFFFF;
            cur_alpha[j * CUR_MAX + i] = (uint8_t)(p >> 24);
        }
    cur_w = w;
    cur_h = h;
    cur_hx = hx;
    cur_hy = hy;
    cur_x = x;
    cur_y = y;
    cur_shown = 1;
    if (ow) repaint(ox, oy, ow, oh);
    repaint(cur_x - cur_hx, cur_y - cur_hy, cur_w, cur_h);
    fb_busy = 0;
    return 0;
}

/* Called by the mouse driver, also from the timer interrupt. */
void fbcon_cursor_move(int x, int y) {
    if (!cursor_on() || (x == cur_x && y == cur_y)) return;
    if (fb_busy) {                         /* in the middle of a blit: right after it */
        cur_px = x;
        cur_py = y;
        cur_pending = 1;
        return;
    }
    fb_busy = 1;
    int ox = cur_x - cur_hx, oy = cur_y - cur_hy;
    cur_x = x;
    cur_y = y;
    int nx = cur_x - cur_hx, ny = cur_y - cur_hy;
    /* One rectangle around both places when they overlap, else two. */
    if (ox < nx + cur_w && nx < ox + cur_w && oy < ny + cur_h && ny < oy + cur_h) {
        int x0 = ox < nx ? ox : nx, y0 = oy < ny ? oy : ny;
        int x1 = (ox > nx ? ox : nx) + cur_w, y1 = (oy > ny ? oy : ny) + cur_h;
        repaint(x0, y0, x1 - x0, y1 - y0);
    } else {
        repaint(ox, oy, cur_w, cur_h);
        repaint(nx, ny, cur_w, cur_h);
    }
    fb_busy = 0;
}
