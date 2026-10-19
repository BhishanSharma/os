#include "lib/fbcon.h"
#include <stdint.h>
#include "drivers/addr.h"
#include "drivers/paging.h"
#include "drivers/heap.h"
#include "drivers/igfx.h"
#include "drivers/timer.h"
#include "sys/task.h"
#include "sys/smp.h"
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
static int flip_on;
static void cursor_blit(int x, int y, int w, int h, const uint32_t *src, int stride);
static void flip_blit(int x, int y, int w, int h, const uint32_t *src, int stride);

void fbcon_blit(int x, int y, int w, int h, const uint32_t *src, int stride) {
    if (!active) return;
    if (flip_on) {
        flip_blit(x, y, w, h, src, stride);
        return;
    }
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

static int hw_cursor;                  /* the graphics card draws it (igfx.c) */

static int cursor_on(void) { return cur_shown && shadow; }
int fbcon_cursor_active(void) { return hw_cursor || cursor_on(); }

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
static void flip_start(void);
static void flip_stop(void);

int fbcon_cursor_set(const uint32_t *argb, int w, int h, int hx, int hy, int x, int y) {
    if (!active) return -1;
    if (w <= 0) {
        if (hw_cursor) igfx_cursor_off();
        hw_cursor = 0;
        flip_stop();
        cur_shown = 0;
        if (shadow) {
            kfree(shadow);
            shadow = 0;
        }
        return 0;
    }
    if (w > CUR_MAX || h > CUR_MAX) return -1;
    if (igfx_cursor_set(argb, w, h, hx, hy, x, y) == 0) {   /* the hardware's own cursor */
        hw_cursor = 1;
        flip_start();
        return 0;
    }
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
    if (hw_cursor) {
        igfx_cursor_move(x, y);
        return;
    }
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

/* ---- Page flipping ------------------------------------------------------------
 * With the graphics card's help (igfx.c): programs draw into the shadow, and
 * fbcon_present() brings the hidden buffer up to date (what changed in this
 * frame and the one before, which it missed) and shows it at the next
 * refresh. The screen never shows a frame half drawn. */

#define MAX_RECTS 32
typedef struct { int x0, y0, x1, y1; } rect_t;
static rect_t now_rects[MAX_RECTS], last_rects[MAX_RECTS];
static int now_n, last_n, front;          /* front: 0 the firmware's buffer, 1 or 2 ours */
static const uint32_t *surface;           /* the desktop's own frame (GFX_SURFACE): no shadow copy */
static int surface_stride;
static int ram_pair;                      /* flipping between buffers 1 and 2 (with the blitter) */

static void add_rect(rect_t *list, int *n, int x0, int y0, int x1, int y1) {
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > (int)fb.width) x1 = (int)fb.width;
    if (y1 > (int)fb.height) y1 = (int)fb.height;
    if (x1 <= x0 || y1 <= y0) return;
    if (*n == MAX_RECTS) {                 /* full: one rectangle around them all */
        for (int i = 1; i < *n; i++) {
            if (list[i].x0 < list[0].x0) list[0].x0 = list[i].x0;
            if (list[i].y0 < list[0].y0) list[0].y0 = list[i].y0;
            if (list[i].x1 > list[0].x1) list[0].x1 = list[i].x1;
            if (list[i].y1 > list[0].y1) list[0].y1 = list[i].y1;
        }
        *n = 1;
    }
    list[*n].x0 = x0; list[*n].y0 = y0; list[*n].x1 = x1; list[*n].y1 = y1;
    (*n)++;
}

static void flip_start(void) {
    uint32_t *back = igfx_back_buffer();
    if (!back || !fast_format()) return;
    if (!shadow) shadow = kmalloc((uint64_t)fb.width * fb.height * 4);
    if (!shadow) return;
    memset(shadow, 0, (uint64_t)fb.width * fb.height * 4);   /* the screen is black now */
    now_n = last_n = 0;
    add_rect(last_rects, &last_n, 0, 0, (int)fb.width, (int)fb.height);   /* ours holds nothing yet */
    front = 0;
    ram_pair = igfx_blitter_ready();          /* buffers 1 and 2 take turns: both start empty */
    if (ram_pair) add_rect(now_rects, &now_n, 0, 0, (int)fb.width, (int)fb.height);
    flip_on = 1;
}

/* ---- A batch on the GPU ------------------------------------------------------
 * Presenting hands the copies to the blitter and returns; the flip happens
 * when the batch is done (the 1 kHz timer looks, or whoever waits first).
 * Until then the source must not change: the shadow (blits wait) or the
 * desktop's frame (it calls GFX_SYNC before drawing). A batch that does not
 * finish in 50 ms turns the blitter off and the CPU does its copies. */
#define MAX_PRE 64
typedef struct { struct igfx_pre g; const uint32_t *src; int stride; } pre_t;   /* + the CPU's view */
static pre_t pre[MAX_PRE];
static int npre;
static int pending = -1;                    /* the buffer to show when the batch is done */
static uint32_t pending_seq, pending_since;
static rect_t pend_rects[2 * MAX_RECTS];
static int pend_n, pend_npre;
static pre_t pend_pre[MAX_PRE];

static void copy_rect(uint8_t *dst, const rect_t *r);

static void cpu_pre(const pre_t *p, int n) {           /* window contents into the desktop's frame */
    for (int i = 0; i < n; i++)
        for (int j = 0; j < p[i].g.h; j++)
            memcpy((uint32_t *)surface + (uint64_t)(p[i].g.y + j) * surface_stride + p[i].g.x,
                   p[i].src + (uint64_t)(p[i].g.sy + j) * p[i].stride + p[i].g.sx, (size_t)p[i].g.w * 4);
}

/* `wait` 0: only if it is done (the timer). -> 1 nothing pending any more. */
static int pending_finish(int wait) {
    while (pending >= 0) {
        if (igfx_copy_done(pending_seq)) {
            igfx_copy_idle();
        } else if (!wait) {
            return 0;
        } else if (timer_ms() - pending_since > 50) {
            igfx_copy_failed();                       /* the CPU does this frame's copies */
            uint8_t *dst = (uint8_t *)igfx_buffer(pending);
            if (surface) cpu_pre(pend_pre, pend_npre);
            for (int i = 0; i < pend_n; i++) copy_rect(dst, &pend_rects[i]);
        } else {
            /* Wait without the kernel lock: holding it stalled every core
             * (and the touchpad's polling) for each frame's copy. */
            uint32_t seq = pending_seq, until = timer_ms() + 2;
            int depth = bkl_drop();
            while (!igfx_copy_done(seq) && timer_ms() < until) __asm__ volatile("pause");
            bkl_take(depth);
            if (pending >= 0 && !igfx_copy_done(pending_seq)) task_block((const void *)&pending, 1);
            continue;
        }
        igfx_flip(pending);
        pending = -1;
        task_wakeup((const void *)&pending);
    }
    return 1;
}

/* GFX_SYNC: the desktop may draw into its frame again. */
void fbcon_sync(void) {
    if (flip_on) pending_finish(1);
}

/* GFX_COPY: a window's contents (`src`, rows `stride` pixels apart; its
 * blitter address src_va) at (sx, sy), to (x, y) in the desktop's frame,
 * w x h, before the next present. -> 0, -1 (the CPU draws it). */
int fbcon_queue_copy(const uint32_t *src, int stride, uint32_t src_va, int x, int y, int w, int h, int sx, int sy) {
    if (!surface || npre == MAX_PRE || !igfx_blitter_ready()) return -1;
    if (x < 0 || y < 0 || w <= 0 || h <= 0 || x + w > (int)fb.width || y + h > (int)fb.height) return -1;
    pre_t *p = &pre[npre++];
    p->g.src_va = src_va;
    p->g.src_pitch = (uint32_t)stride * 4;
    p->g.x = x;
    p->g.y = y;
    p->g.w = w;
    p->g.h = h;
    p->g.sx = sx;
    p->g.sy = sy;
    p->src = src;
    p->stride = stride;
    return 0;
}

void fbcon_drop_copies(void) { npre = 0; }

static void flip_stop(void) {
    if (!flip_on) return;
    pending_finish(1);                     /* (the GPU may be reading the desktop's memory) */
    flip_on = 0;
    surface = 0;
    npre = 0;
    if (front) igfx_flip(0);               /* the console draws into the firmware's buffer */
    front = 0;
}

static void flip_blit(int x, int y, int w, int h, const uint32_t *src, int stride) {
    int x0 = x < 0 ? 0 : x, x1 = x + w > (int)fb.width ? (int)fb.width : x + w;
    if (x1 <= x0) return;
    if (surface) {
        if (stride == surface_stride && src == surface + (int64_t)y * stride + x) {
            add_rect(now_rects, &now_n, x, y, x + w, y + h);   /* it is already there */
            return;
        }
        pending_finish(1);
        cpu_pre(pre, npre);
        npre = 0;
        for (uint32_t j = 0; j < fb.height; j++)       /* from elsewhere: back to the shadow */
            memcpy(shadow + (uint64_t)j * fb.width, surface + (uint64_t)j * surface_stride, fb.width * 4);
        surface = 0;
        add_rect(now_rects, &now_n, 0, 0, (int)fb.width, (int)fb.height);
    }
    pending_finish(1);                                 /* the GPU may be reading the shadow */
    for (int j = 0; j < h; j++) {
        int sy = y + j;
        if (sy < 0 || (uint32_t)sy >= fb.height) continue;
        memcpy(shadow + (uint64_t)sy * fb.width + x0, src + (int64_t)j * stride + (x0 - x), (size_t)(x1 - x0) * 4);
    }
    add_rect(now_rects, &now_n, x, y, x + w, y + h);
}

static void copy_rect(uint8_t *dst, const rect_t *r) {
    const uint32_t *src = surface ? surface : shadow;
    uint64_t stride = surface ? (uint64_t)surface_stride : fb.width;
    for (int j = r->y0; j < r->y1; j++) {
        const uint32_t *from = src + (uint64_t)j * stride + r->x0;
        uint32_t *to = (uint32_t *)(dst + (uint64_t)j * fb.pitch) + r->x0;
        uint64_t n = (uint64_t)(r->x1 - r->x0);
        __asm__ volatile("rep movsl" : "+D"(to), "+S"(from), "+c"(n) : : "memory");
    }
}

static volatile int flip_waiting;

/* The desktop's frame (`px`, rows `stride` pixels apart, the screen's size)
 * is what its blits come from: the blitter copies from it directly. `pages`:
 * its physical pages. -> 0, -1 if there is no blitter or page flipping. */
int fbcon_surface(const uint32_t *px, int stride, const uint64_t *pages, int n) {
    if (!flip_on || !ram_pair || !igfx_blitter_ready()) return -1;
    pending_finish(1);
    if (igfx_copy_source(px, pages, n, (uint32_t)((uint64_t)px & 0xFFF), (uint32_t)stride * 4)) return -1;
    surface = px;
    surface_stride = stride;
    return 0;
}

/* From the 1 kHz timer: the GPU's batch is done (flip), the last flip reached the screen. */
void fbcon_flip_poll(void) {
    if (pending >= 0) pending_finish(0);
    if (flip_waiting && igfx_flip_done()) {
        flip_waiting = 0;
        task_wakeup((const void *)&flip_waiting);
    }
}

/* -> 1: queued for the next refresh, after waiting for the frame before to
 * be shown (this paces the desktop at the screen's rate); 0: nothing to do
 * (no page flipping). */
/* Until the frame before is on the screen (at most 100 ms). 1 ms at a time:
 * the timer's wake-up can come between setting flip_waiting and blocking,
 * and a 20 ms block then lost a whole 20 ms (hitches of 20-80 ms). */
static void wait_shown(void) {
    for (int tries = 0; !igfx_flip_done() && tries < 100; tries++) {
        flip_waiting = 1;
        task_block((const void *)&flip_waiting, 1);
    }
}

/* GFX_WAIT: the frame before copied, flipped and shown, so the next one is
 * drawn from the newest input and shown at the next refresh. */
void fbcon_wait_shown(void) {
    if (!flip_on) return;
    pending_finish(1);
    wait_shown();
}

int fbcon_present(void) {
    if (!flip_on) return 0;
    pending_finish(1);                              /* the frame before: copied and flipped */
    if (!now_n && !last_n) {
        npre = 0;
        return 0;
    }
    wait_shown();
    int target = ram_pair ? (front == 1 ? 2 : 1) : !front;   /* with the blitter: our two RAM buffers */
    int gpu = ram_pair && (surface ? igfx_blitter_ready() : igfx_copy_prepare(shadow) == 0);
    uint8_t *dst = target ? (uint8_t *)igfx_buffer(target) : fb_base;
    rect_t todo[2 * MAX_RECTS];                    /* this frame's and the last (the buffer is a frame behind) */
    int n = 0;
    for (int i = 0; i < now_n; i++) todo[n++] = now_rects[i];
    for (int i = 0; i < last_n; i++) todo[n++] = last_rects[i];
    memcpy(last_rects, now_rects, sizeof(rect_t) * (size_t)now_n);
    last_n = now_n;
    now_n = 0;
    struct igfx_pre g[MAX_PRE];
    for (int i = 0; i < npre; i++) g[i] = pre[i].g;
    uint32_t seq;
    if (gpu && igfx_copy_submit(surface ? surface : shadow, target, &todo[0].x0, n, g, npre, &seq) == 0) {
        pending = target;                           /* flipped when it is done */
        pending_seq = seq;
        pending_since = timer_ms();
        memcpy(pend_rects, todo, sizeof(rect_t) * (size_t)n);
        pend_n = n;
        memcpy(pend_pre, pre, sizeof(pre_t) * (size_t)npre);
        pend_npre = npre;
        npre = 0;
        front = target;
        return 1;
    }
    /* The CPU: other cores may use the kernel meanwhile (the source is the
     * shadow, or the memory of the desktop, which is waiting for this call). */
    int depth = bkl_drop();
    if (surface) cpu_pre(pre, npre);
    for (int i = 0; i < n; i++) copy_rect(dst, &todo[i]);
    bkl_take(depth);
    npre = 0;
    if (!flip_on) return 0;                         /* the desktop ended meanwhile */
    igfx_flip(target);
    front = target;
    return 1;
}
