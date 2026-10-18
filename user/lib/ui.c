// ui.c - drawing, widgets, and windows from the window server (see ui.h)
//
// Text comes from UIFONT.BIN (scripts/gen-uifont.py): glyphs pre-rendered
// with anti-aliasing at each scale, blended onto the pixels. Shapes have
// anti-aliased round corners (4x4 samples per edge pixel). Without the font
// file, the console's 8x16 font is used.
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <image.h>
#include <os.h>
#include <ui.h>

extern const unsigned char font8x16[256][16];

/* Appearance settings from the window server (WS_PREF_*). */
static unsigned prefs;
int ui_pref(int bit) { return (prefs & (unsigned)bit) != 0; }

/* ---- Pixels -------------------------------------------------------------- */

static inline unsigned div255(unsigned v) { return (v + 1 + (v >> 8)) >> 8; }

/* `color` over `dst` with coverage a (0-255). */
static inline unsigned mix(unsigned dst, unsigned color, unsigned a) {
    if (a >= 255) return color;
    unsigned na = 255 - a;
    unsigned r = div255(((color >> 16) & 255) * a + ((dst >> 16) & 255) * na);
    unsigned g = div255(((color >> 8) & 255) * a + ((dst >> 8) & 255) * na);
    unsigned b = div255((color & 255) * a + (dst & 255) * na);
    return r << 16 | g << 8 | b;
}

static inline int inside_clip(const ui_canvas *c, int x, int y) {
    return x >= c->cx0 && y >= c->cy0 && x < c->cx1 && y < c->cy1;
}

/* What was drawn on (clipped), for ui_present: only that is shown again. */
static inline void touch(ui_canvas *c, int x0, int y0, int x1, int y1) {
    if (x1 <= x0 || y1 <= y0) return;
    if (c->dx1 <= c->dx0) {
        c->dx0 = x0; c->dy0 = y0; c->dx1 = x1; c->dy1 = y1;
        return;
    }
    if (x0 < c->dx0) c->dx0 = x0;
    if (y0 < c->dy0) c->dy0 = y0;
    if (x1 > c->dx1) c->dx1 = x1;
    if (y1 > c->dy1) c->dy1 = y1;
}

static inline void blend(ui_canvas *c, int x, int y, unsigned color, unsigned a) {
    if (a && inside_clip(c, x, y)) {
        touch(c, x, y, x + 1, y + 1);
        unsigned *p = c->px + (long)y * c->stride + x;
        *p = mix(*p, color, a);
    }
}

/* ---- Canvas -------------------------------------------------------------- */

void ui_canvas_init(ui_canvas *c, unsigned *px, int w, int h, int stride) {
    c->px = px;
    c->w = w;
    c->h = h;
    c->stride = stride;
    c->dx0 = c->dy0 = c->dx1 = c->dy1 = 0;
    ui_noclip(c);
}

void ui_noclip(ui_canvas *c) {
    c->cx0 = c->cy0 = 0;
    c->cx1 = c->w;
    c->cy1 = c->h;
}

void ui_clip(ui_canvas *c, int x, int y, int w, int h) {
    c->cx0 = x < 0 ? 0 : x;
    c->cy0 = y < 0 ? 0 : y;
    c->cx1 = x + w > c->w ? c->w : x + w;
    c->cy1 = y + h > c->h ? c->h : y + h;
}

void ui_fill_a(ui_canvas *c, int x, int y, int w, int h, unsigned color, int alpha) {
    int x0 = x < c->cx0 ? c->cx0 : x, y0 = y < c->cy0 ? c->cy0 : y;
    int x1 = x + w > c->cx1 ? c->cx1 : x + w, y1 = y + h > c->cy1 ? c->cy1 : y + h;
    touch(c, x0, y0, x1, y1);
    for (int j = y0; j < y1; j++) {
        unsigned *row = c->px + (long)j * c->stride;
        if (alpha >= 255)
            for (int i = x0; i < x1; i++) row[i] = color;
        else
            for (int i = x0; i < x1; i++) row[i] = mix(row[i], color, (unsigned)alpha);
    }
}

void ui_fill(ui_canvas *c, int x, int y, int w, int h, unsigned color) { ui_fill_a(c, x, y, w, h, color, 255); }

void ui_blend_pixel(ui_canvas *c, int x, int y, unsigned color, int alpha) { blend(c, x, y, color, (unsigned)alpha); }

void ui_frame(ui_canvas *c, int x, int y, int w, int h, unsigned color) {
    ui_fill(c, x, y, w, 1, color);
    ui_fill(c, x, y + h - 1, w, 1, color);
    ui_fill(c, x, y, 1, h, color);
    ui_fill(c, x + w - 1, y, 1, h, color);
}

/* How much of pixel (i, j) of a corner square lies inside the quarter circle
 * of radius r centred at (r, r): 0-255, from 4x4 samples. */
static unsigned corner_cover(int i, int j, int r) {
    int R = 8 * r, inside = 0;
    for (int sy = 0; sy < 4; sy++) {
        int dy = R - (8 * j + 2 * sy + 1);
        for (int sx = 0; sx < 4; sx++) {
            int dx = R - (8 * i + 2 * sx + 1);
            if (dx <= 0 && dy <= 0) inside++;
            else if ((dx > 0 ? dx * dx : 0) + (dy > 0 ? dy * dy : 0) <= R * R) inside++;
        }
    }
    return (unsigned)(inside * 255 / 16);
}

/* Coverage of pixel (i, j) of a w x h rectangle with round `corners`
 * (UI_TL | UI_TR | UI_BL | UI_BR) of radius r. */
static unsigned rect_cover(int i, int j, int w, int h, int r, int corners) {
    int ci = -1, cj = -1;
    if (i < r) ci = i;
    else if (i >= w - r) ci = w - 1 - i;
    if (j < r) cj = j;
    else if (j >= h - r) cj = h - 1 - j;
    if (ci < 0 || cj < 0) return 255;
    int which = (j < r ? (i < r ? UI_TL : UI_TR) : (i < r ? UI_BL : UI_BR));
    if (!(corners & which)) return 255;
    return corner_cover(ci, cj, r);
}

void ui_round_ex(ui_canvas *c, int x, int y, int w, int h, int r, unsigned color, int alpha, int corners) {
    if (w <= 0 || h <= 0) return;
    if (r * 2 > h) r = h / 2;
    if (r * 2 > w) r = w / 2;
    int y0 = y < c->cy0 ? c->cy0 : y, y1 = y + h > c->cy1 ? c->cy1 : y + h;
    int x0 = x < c->cx0 ? c->cx0 : x, x1 = x + w > c->cx1 ? c->cx1 : x + w;
    touch(c, x0, y0, x1, y1);
    for (int py = y0; py < y1; py++) {
        int j = py - y;
        unsigned *row = c->px + (long)py * c->stride;
        int edge = j < r || j >= h - r;
        for (int px = x0; px < x1; px++) {
            int i = px - x;
            if (!edge && px == x + r && x + w - r > px) {    /* the middle: no corners */
                int end = x + w - r < x1 ? x + w - r : x1;
                for (; px < end; px++) row[px] = alpha >= 255 ? color : mix(row[px], color, (unsigned)alpha);
                px--;
                continue;
            }
            unsigned a = (edge && (i < r || i >= w - r)) ? rect_cover(i, j, w, h, r, corners) : 255;
            if (a) row[px] = mix(row[px], color, a * (unsigned)alpha / 255);
        }
    }
}

void ui_round(ui_canvas *c, int x, int y, int w, int h, int r, unsigned color) {
    ui_round_ex(c, x, y, w, h, r, color, 255, UI_ALL);
}

void ui_circle(ui_canvas *c, int cx, int cy, int r, unsigned color) {
    ui_round(c, cx - r, cy - r, 2 * r, 2 * r, r, color);
}

void ui_copy(ui_canvas *c, int x, int y, const unsigned *src, int w, int h, int stride) {
    int x0 = x < c->cx0 ? c->cx0 : x, y0 = y < c->cy0 ? c->cy0 : y;
    int x1 = x + w > c->cx1 ? c->cx1 : x + w, y1 = y + h > c->cy1 ? c->cy1 : y + h;
    if (x1 <= x0) return;
    touch(c, x0, y0, x1, y1);
    for (int j = y0; j < y1; j++)
        memcpy(c->px + (long)j * c->stride + x0, src + (long)(j - y) * stride + (x0 - x),
               (size_t)(x1 - x0) * 4);
}

/* Like ui_copy, with round corners: corner pixels are blended over what is
 * already there (draw the background first). */
void ui_copy_round(ui_canvas *c, int x, int y, const unsigned *src, int w, int h, int stride, int r, int corners) {
    if (r * 2 > h) r = h / 2;
    if (r * 2 > w) r = w / 2;
    int x0 = x < c->cx0 ? c->cx0 : x, y0 = y < c->cy0 ? c->cy0 : y;
    int x1 = x + w > c->cx1 ? c->cx1 : x + w, y1 = y + h > c->cy1 ? c->cy1 : y + h;
    if (x1 <= x0) return;
    touch(c, x0, y0, x1, y1);
    for (int py = y0; py < y1; py++) {
        int j = py - y;
        unsigned *dst = c->px + (long)py * c->stride;
        const unsigned *from = src + (long)j * stride - x;
        if (j >= r && j < h - r) {
            memcpy(dst + x0, from + x0, (size_t)(x1 - x0) * 4);
            continue;
        }
        for (int px = x0; px < x1; px++) {
            int i = px - x;
            unsigned a = (i < r || i >= w - r) ? rect_cover(i, j, w, h, r, corners) : 255;
            dst[px] = mix(dst[px], from[px], a);
        }
    }
}

static int isqrt(int v) {
    if (v <= 0) return 0;
    int r = 0, bit = 1 << 30;
    while (bit > v) bit >>= 2;
    while (bit) {
        if (v >= r + bit) {
            v -= r + bit;
            r = (r >> 1) + bit;
        } else {
            r >>= 1;
        }
        bit >>= 2;
    }
    return r;
}

/* A soft shadow of a w x h rectangle with corners of radius r: darkest well
 * inside its edge, fading out `size` pixels beyond it. The middle (which the
 * window covers) is skipped. With `outside` only: for see-through things. */
void ui_shadow_ex(ui_canvas *c, int x, int y, int w, int h, int r, int size, int alpha, int outside) {
    int x0 = x - size, y0 = y - size, x1 = x + w + size, y1 = y + h + size;
    if (x0 < c->cx0) x0 = c->cx0;
    if (y0 < c->cy0) y0 = c->cy0;
    if (x1 > c->cx1) x1 = c->cx1;
    if (y1 > c->cy1) y1 = c->cy1;
    touch(c, x0, y0, x1, y1);
    for (int py = y0; py < y1; py++) {
        unsigned *row = c->px + (long)py * c->stride;
        int qy = y + r - py;
        if (py - (y + h - 1 - r) > qy) qy = py - (y + h - 1 - r);
        if (qy < 0) qy = 0;
        int half = size / 2, inner = py >= y + half && py < y + h - half;
        for (int px = x0; px < x1; px++) {
            if (inner && px == x + r + half && x + w - r - half > px) {   /* skip the middle */
                px = x + w - r - half - 1;
                continue;
            }
            int qx = x + r - px;
            if (px - (x + w - 1 - r) > qx) qx = px - (x + w - 1 - r);
            if (qx < 0) qx = 0;
            /* Distance outside the edge in quarter pixels; inside it counts as
               negative (down to -size/2, the darkest). */
            int d = isqrt((qx * qx + qy * qy) * 16) - r * 4;
            if (d <= 0 && outside) continue;
            if (d <= 0) {
                int ix = px - x < x + w - 1 - px ? px - x : x + w - 1 - px;
                int iy = py - y < y + h - 1 - py ? py - y : y + h - 1 - py;
                int in = ix < iy ? ix : iy;
                if (in > r && qx == 0 && qy == 0) d = -in * 4;
            }
            int span = size * 6;                                   /* -size/2 .. size */
            int f = size * 4 - d;
            if (f <= 0) continue;
            if (f > span) f = span;
            unsigned a = (unsigned)(alpha * f / span * f / span);
            if (a) row[px] = mix(row[px], 0x000000, a);
        }
    }
}

void ui_shadow(ui_canvas *c, int x, int y, int w, int h, int r, int size, int alpha) {
    ui_shadow_ex(c, x, y, w, h, r, size, alpha, 0);
}

/* ---- Text ---------------------------------------------------------------- */

typedef struct __attribute__((packed)) {
    unsigned char style, scale;
    unsigned short px, line_h, baseline;
    unsigned int glyphs;
} face_t;

typedef struct __attribute__((packed)) {
    unsigned short advance;               /* 1/64 pixel */
    short x0, y0;
    unsigned char w, h;
    unsigned int bitmap;
} glyph_t;

static const unsigned char *font;
static int font_faces, font_tried;

/* A file of the system (RAM disk: /SYS when a disk is mounted at /), mapped
 * read-only; 0 if missing or without the 4-byte `magic`. */
static const unsigned char *system_file(const char *name, const char *magic) {
    char path[64];
    for (int i = 0; i < 2; i++) {
        snprintf(path, sizeof(path), "%s/%s", i ? "" : "/SYS", name);
        struct os_stat st;
        if (stat(path, &st) != 0 || st.size < 8) continue;
        int fd = open(path, OPEN_READ);
        if (fd < 0) continue;
        const unsigned char *data = mmap(st.size, PROT_READ, fd);
        close(fd);
        if (data && memcmp(data, magic, 4) == 0) return data;
    }
    return 0;
}

static void load_font(void) {
    font_tried = 1;
    font = system_file("UIFONT.BIN", "UIF1");
    if (font) font_faces = *(const int *)(font + 4);
}

/* The face for `style` at `scale`, or the largest smaller one; 0: none. */
static const face_t *find_face(int style, int scale) {
    if (!font_tried) load_font();
    if (!font) return 0;
    const face_t *best = 0;
    for (int i = 0; i < font_faces; i++) {
        const face_t *f = (const face_t *)(font + 8 + i * sizeof(face_t));
        if (f->style != style) continue;
        if (f->scale == scale) return f;
        if (f->scale < scale && (!best || f->scale > best->scale)) best = f;
    }
    if (!best && style != UI_REGULAR) return find_face(UI_REGULAR, scale);
    return best;
}

static const glyph_t *glyph(const face_t *f, unsigned char ch) {
    if (ch < 32 || ch > 127) ch = '?';
    return (const glyph_t *)(font + f->glyphs + (ch - 32) * sizeof(glyph_t));
}

/* One glyph with its pen at 1/64 pixel x64, line box top y. */
static void draw_glyph(ui_canvas *c, const face_t *f, const glyph_t *g, long x64, int y, unsigned color) {
    if (!g->w) return;
    int gx = (int)((x64 + 32) >> 6) + g->x0, gy = y + g->y0;
    if (gx >= c->cx1 || gy >= c->cy1 || gx + g->w <= c->cx0 || gy + g->h <= c->cy0) return;
    touch(c, gx < c->cx0 ? c->cx0 : gx, gy < c->cy0 ? c->cy0 : gy,
          gx + g->w > c->cx1 ? c->cx1 : gx + g->w, gy + g->h > c->cy1 ? c->cy1 : gy + g->h);
    const unsigned char *bits = font + g->bitmap;
    for (int j = 0; j < g->h; j++) {
        int py = gy + j;
        if (py < c->cy0 || py >= c->cy1) continue;
        unsigned *row = c->px + (long)py * c->stride;
        for (int i = 0; i < g->w; i++) {
            unsigned a = bits[j * g->w + i];
            int px = gx + i;
            if (a && px >= c->cx0 && px < c->cx1) row[px] = mix(row[px], color, a);
        }
    }
}

static int bitmap_char(ui_canvas *c, int x, int y, unsigned char ch, unsigned color, int scale) {
    if (x >= c->cx1 || y >= c->cy1 || x + 8 * scale <= c->cx0 || y + 16 * scale <= c->cy0) return 8 * scale;
    const unsigned char *g = font8x16[ch];
    for (int j = 0; j < 16; j++) {
        unsigned char bits = g[j];
        if (!bits) continue;
        for (int i = 0; i < 8; i++)
            if (bits & (0x80 >> i)) ui_fill(c, x + i * scale, y + j * scale, scale, scale, color);
    }
    return 8 * scale;
}

/* Tracking in 1/64 pixel, from the macOS type spec: body text -0.43 px at 17
 * (-0.025 em), large titles -1.05 px at 34 (-0.031 em). The monospaced face
 * (terminal cells) is not tracked. */
#ifndef UI_TRACKING
#define UI_TRACKING 1
#endif
static int tracking64(const face_t *f) {
    if (!UI_TRACKING || f->style == UI_MONO) return 0;
    int em1000 = f->px < 20 ? 253 : 309;
    return -(int)(((long)f->px * 64 * em1000 + 5000) / 10000);
}

int ui_text_style(ui_canvas *c, int x, int y, const char *s, unsigned color, int scale, int style) {
    const face_t *f = find_face(style, scale);
    if (!f) {
        int start = x;
        while (*s) x += bitmap_char(c, x, y, (unsigned char)*s++, color, scale);
        return x - start;
    }
    y += (16 * scale - f->line_h) / 2;            /* a smaller face: centred */
    int trk = tracking64(f);
    long x64 = (long)x * 64;
    for (; *s; s++) {
        const glyph_t *g = glyph(f, (unsigned char)*s);
        draw_glyph(c, f, g, x64, y, color);
        x64 += g->advance + (s[1] ? trk : 0);      /* none after the last letter */
    }
    return (int)((x64 + 32) >> 6) - x;
}

int ui_text_style_width(const char *s, int scale, int style) {
    const face_t *f = find_face(style, scale);
    if (!f) return (int)strlen(s) * 8 * scale;
    int trk = tracking64(f);
    long x64 = 0;
    for (; *s; s++) x64 += glyph(f, (unsigned char)*s)->advance + (s[1] ? trk : 0);
    return (int)((x64 + 32) >> 6);
}

/* Leading: 129% of the font size. */
int ui_leading(int scale) {
    const face_t *f = find_face(UI_REGULAR, scale);
    int px = f ? f->px : 16 * scale;
    return (px * 129 + 50) / 100;
}

/* Word wrap into `width` pixels ('\n' breaks a line); one leading per line. */
int ui_text_block(ui_canvas *c, int x, int y, int width, const char *s, unsigned color, int scale, int style) {
    int lead = ui_leading(scale), lines = 0;
    char line[128];
    while (*s) {
        int n = 0, last_space = -1;
        const char *p = s;
        while (*p && *p != '\n' && n < (int)sizeof(line) - 1) {
            line[n++] = *p++;
            line[n] = 0;
            if (line[n - 1] == ' ') last_space = n - 1;
            if (ui_text_style_width(line, scale, style) > width && n > 1) {   /* too long: back to a space */
                if (last_space > 0) {
                    p -= n - last_space;
                    n = last_space;
                } else {
                    p--;
                    n--;
                }
                break;
            }
        }
        line[n] = 0;
        if (c) ui_text_style(c, x, y + lines * lead + (lead - 16 * scale) / 2, line, color, scale, style);
        lines++;
        s = p;
        if (*s == '\n' || *s == ' ') s++;
    }
    return lines * lead;
}

int ui_text(ui_canvas *c, int x, int y, const char *s, unsigned color, int scale) {
    return ui_text_style(c, x, y, s, color, scale, UI_REGULAR);
}

int ui_text_bold(ui_canvas *c, int x, int y, const char *s, unsigned color, int scale) {
    return ui_text_style(c, x, y, s, color, scale, UI_BOLD);
}

int ui_text_width(const char *s, int scale) { return ui_text_style_width(s, scale, UI_REGULAR); }

/* A character cell of the monospaced font (Terminal): 8 x 16 times scale. */
int ui_char(ui_canvas *c, int x, int y, unsigned char ch, unsigned color, int scale) {
    const face_t *f = find_face(UI_MONO, scale);
    if (!f || f->scale != scale) return bitmap_char(c, x, y, ch, color, scale);
    const glyph_t *g = glyph(f, ch);
    long x64 = (long)x * 64 + ((long)8 * scale * 64 - g->advance) / 2;   /* centred in the cell */
    draw_glyph(c, f, g, x64, y, color);
    return 8 * scale;
}

/* ---- Icons (ICONS.BIN: scripts/gen-icons.py) ------------------------------ */

typedef struct __attribute__((packed)) {
    char name[16];
    struct { unsigned int size, offset; } m[3];      /* 48, 96, 192 pixels */
} icon_t;

static const unsigned char *icons;
static int icon_count, icons_tried;

static const icon_t *find_icon(const char *name) {
    if (!icons_tried) {
        icons_tried = 1;
        icons = system_file("ICONS.BIN", "ICN1");
        if (icons) icon_count = *(const int *)(icons + 4);
    }
    for (int i = 0; icons && i < icon_count; i++) {
        const icon_t *ic = (const icon_t *)(icons + 8 + i * sizeof(icon_t));
        if (!strncmp(ic->name, name, 16)) return ic;
    }
    return 0;
}

/* An icon from a picture file (an installed app's icon.png): `name` is its
 * path. Square, halved (averaged) down to 256 pixels at most; kept once read. */
#define FILE_ICONS 24
static struct { char path[128]; int s; unsigned *px; } file_icons[FILE_ICONS];
static int n_file_icons;

static const unsigned *file_icon(const char *path, int *s) {
    for (int i = 0; i < n_file_icons; i++)
        if (!strcmp(file_icons[i].path, path)) {
            *s = file_icons[i].s;
            return file_icons[i].px;
        }
    if (n_file_icons == FILE_ICONS || strlen(path) >= sizeof(file_icons[0].path)) return 0;
    int k = n_file_icons++;
    snprintf(file_icons[k].path, sizeof(file_icons[k].path), "%s", path);   /* (a failed one: px 0) */
    struct os_stat st;
    if (stat(path, &st) != 0 || st.is_dir || !st.size || st.size > 4 * 1024 * 1024) return 0;
    unsigned char *data = malloc(st.size);
    int fd = data ? open(path, OPEN_READ) : -1;
    long got = fd >= 0 ? read(fd, data, st.size) : -1;
    if (fd >= 0) close(fd);
    image_t img;
    const char *error;
    int ok = got == (long)st.size && image_decode_rgba(data, st.size, &img, &error) == 0;
    free(data);
    if (!ok) return 0;
    int n = img.width < img.height ? img.width : img.height;        /* the middle square */
    unsigned *px = malloc((size_t)n * n * 4);
    if (!px) {
        image_free(&img);
        return 0;
    }
    for (int y = 0; y < n; y++)
        memcpy(px + (long)y * n, img.pixels + (long)(y + (img.height - n) / 2) * img.width + (img.width - n) / 2, (size_t)n * 4);
    image_free(&img);
    while (n > 256) {                                  /* halve: each pixel the average of four */
        int h = n / 2;
        for (int y = 0; y < h; y++)
            for (int x = 0; x < h; x++) {
                unsigned q[4] = { px[2 * y * n + 2 * x], px[2 * y * n + 2 * x + 1], px[(2 * y + 1) * n + 2 * x], px[(2 * y + 1) * n + 2 * x + 1] };
                unsigned out = 0;
                for (int b = 0; b < 32; b += 8)
                    out |= (((q[0] >> b & 255) + (q[1] >> b & 255) + (q[2] >> b & 255) + (q[3] >> b & 255) + 2) / 4) << b;
                px[y * h + x] = out;
            }
        n = h;
    }
    file_icons[k].s = n;
    file_icons[k].px = px;
    *s = n;
    return px;
}

int ui_has_icon(const char *name) {
    int s;
    return name[0] == '/' ? file_icon(name, &s) != 0 : find_icon(name) != 0;
}

/* Premultiplied 0xAARRGGBB of texel (x, y). */
static inline unsigned texel(const unsigned *px, int s, int x, int y) {
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (x >= s) x = s - 1;
    if (y >= s) y = s - 1;
    unsigned p = px[y * s + x], a = p >> 24;
    return a << 24 | div255(((p >> 16) & 255) * a) << 16 | div255(((p >> 8) & 255) * a) << 8 | div255((p & 255) * a);
}

static inline unsigned lerp4(unsigned a, unsigned b, unsigned f) {   /* each byte, f of 256 */
    unsigned r = 0;
    for (int k = 0; k < 32; k += 8) {
        unsigned x = (a >> k) & 255, y = (b >> k) & 255;
        r |= ((x * (256 - f) + y * f) >> 8) << k;
    }
    return r;
}

/* The icon `name` in a size x size square at (x, y), smoothly scaled from
 * the nearest stored size; alpha 0-255 fades it. -> 0, -1 if there is none. */
int ui_icon(ui_canvas *c, int x, int y, int size, const char *name, int alpha) {
    int s;
    const unsigned *px;
    if (size <= 0) return -1;
    if (name[0] == '/') {                              /* a picture file (file_icon) */
        if (!(px = file_icon(name, &s))) return -1;
    } else {
        const icon_t *ic = find_icon(name);
        if (!ic) return -1;
        int k = 0;
        while (k < 2 && (int)ic->m[k].size < size) k++;
        s = (int)ic->m[k].size;
        px = (const unsigned *)(icons + ic->m[k].offset);
    }
    int y0 = y < c->cy0 ? c->cy0 : y, y1 = y + size > c->cy1 ? c->cy1 : y + size;
    int x0 = x < c->cx0 ? c->cx0 : x, x1 = x + size > c->cx1 ? c->cx1 : x + size;
    touch(c, x0, y0, x1, y1);
    for (int py = y0; py < y1; py++) {
        int v = ((2 * (py - y) + 1) * s * 128) / size - 128;  /* 1/256 texels */
        int ty = v >> 8;
        unsigned fy = (unsigned)(v & 255);
        unsigned *row = c->px + (long)py * c->stride;
        for (int ppx = x0; ppx < x1; ppx++) {
            int u = ((2 * (ppx - x) + 1) * s * 128) / size - 128, tx = u >> 8;
            unsigned fx = (unsigned)(u & 255);
            unsigned top = lerp4(texel(px, s, tx, ty), texel(px, s, tx + 1, ty), fx);
            unsigned bot = lerp4(texel(px, s, tx, ty + 1), texel(px, s, tx + 1, ty + 1), fx);
            unsigned p = lerp4(top, bot, fy), a = p >> 24;
            if (alpha < 255) {
                p = lerp4(0, p, (unsigned)alpha);
                a = p >> 24;
            }
            if (!a) continue;
            unsigned d = row[ppx], na = 255 - a;           /* premultiplied over */
            unsigned r = ((p >> 16) & 255) + div255(((d >> 16) & 255) * na);
            unsigned g = ((p >> 8) & 255) + div255(((d >> 8) & 255) * na);
            unsigned b = (p & 255) + div255((d & 255) * na);
            row[ppx] = (r > 255 ? 255 : r) << 16 | (g > 255 ? 255 : g) << 8 | (b > 255 ? 255 : b);
        }
    }
    return 0;
}

/* ---- Widgets ------------------------------------------------------------- */

/* Semantic colours: [Increase Contrast][Dark][token]. The light and dark values
 * match the window server's tokens (windowserver.c). */
static const unsigned color_table[2][2][UI_C_COUNT] = {
    [0][0] = {   /* light */
        [UI_C_LABEL] = 0x1D1D1F,   [UI_C_SECONDARY] = 0x6E6E73,   [UI_C_TERTIARY] = 0xA1A1A6,
        [UI_C_WINDOW] = 0xF2F2F4,  [UI_C_TOOLBAR] = 0xE9E9EB,     [UI_C_CONTENT] = 0xFFFFFF,
        [UI_C_SIDEBAR] = 0xEDEDF0, [UI_C_SEPARATOR] = 0xD8D8DC,   [UI_C_ACCENT] = 0x007AFF,
        [UI_C_ROW_ALT] = 0xF5F5F7, [UI_C_FIELD] = 0xFFFFFF,       [UI_C_CONTROL] = 0xFFFFFF,
        [UI_C_CONTROL_EDGE] = 0xD0D0D4, [UI_C_CONTROL_OFF] = 0xE8E8EC,
    },
    [0][1] = {   /* dark */
        [UI_C_LABEL] = 0xF5F5F7,   [UI_C_SECONDARY] = 0x98989D,   [UI_C_TERTIARY] = 0x636366,
        [UI_C_WINDOW] = 0x2C2C2E,  [UI_C_TOOLBAR] = 0x3A3A3C,     [UI_C_CONTENT] = 0x1E1E20,
        [UI_C_SIDEBAR] = 0x262628, [UI_C_SEPARATOR] = 0x444447,   [UI_C_ACCENT] = 0x0A84FF,
        [UI_C_ROW_ALT] = 0x262629, [UI_C_FIELD] = 0x1E1E20,       [UI_C_CONTROL] = 0x5C5C60,
        [UI_C_CONTROL_EDGE] = 0x6B6B70, [UI_C_CONTROL_OFF] = 0x3A3A3C,
    },
    [1][0] = {   /* light, Increase Contrast: darker text, accent and outlines */
        [UI_C_LABEL] = 0x000000,   [UI_C_SECONDARY] = 0x48484A,   [UI_C_TERTIARY] = 0x6E6E73,
        [UI_C_WINDOW] = 0xF2F2F4,  [UI_C_TOOLBAR] = 0xE9E9EB,     [UI_C_CONTENT] = 0xFFFFFF,
        [UI_C_SIDEBAR] = 0xEDEDF0, [UI_C_SEPARATOR] = 0x8E8E93,   [UI_C_ACCENT] = 0x0058D0,
        [UI_C_ROW_ALT] = 0xF2F2F4, [UI_C_FIELD] = 0xFFFFFF,       [UI_C_CONTROL] = 0xFFFFFF,
        [UI_C_CONTROL_EDGE] = 0x6E6E73, [UI_C_CONTROL_OFF] = 0xE0E0E4,
    },
    [1][1] = {   /* dark, Increase Contrast */
        [UI_C_LABEL] = 0xFFFFFF,   [UI_C_SECONDARY] = 0xC7C7CC,   [UI_C_TERTIARY] = 0x98989D,
        [UI_C_WINDOW] = 0x2C2C2E,  [UI_C_TOOLBAR] = 0x3A3A3C,     [UI_C_CONTENT] = 0x1E1E20,
        [UI_C_SIDEBAR] = 0x262628, [UI_C_SEPARATOR] = 0xAEAEB2,   [UI_C_ACCENT] = 0x409CFF,
        [UI_C_ROW_ALT] = 0x2C2C2E, [UI_C_FIELD] = 0x1E1E20,       [UI_C_CONTROL] = 0x5C5C60,
        [UI_C_CONTROL_EDGE] = 0xAEAEB2, [UI_C_CONTROL_OFF] = 0x3A3A3C,
    },
};

/* The accent colours of System Settings > Appearance, as macOS's:
 * [accent][Increase Contrast][Dark]. */
static const unsigned accents[WS_ACCENTS][2][2] = {
    [WS_ACCENT_BLUE]     = { { 0x007AFF, 0x0A84FF }, { 0x0058D0, 0x409CFF } },
    [WS_ACCENT_PURPLE]   = { { 0xAF52DE, 0xBF5AF2 }, { 0x8944AB, 0xDA8FFF } },
    [WS_ACCENT_PINK]     = { { 0xFF2D55, 0xFF375F }, { 0xD30F45, 0xFF6482 } },
    [WS_ACCENT_RED]      = { { 0xFF3B30, 0xFF453A }, { 0xD70015, 0xFF6961 } },
    [WS_ACCENT_ORANGE]   = { { 0xFF9500, 0xFF9F0A }, { 0xC93400, 0xFFB340 } },
    [WS_ACCENT_YELLOW]   = { { 0xFFCC00, 0xFFD60A }, { 0xB25000, 0xFFD426 } },
    [WS_ACCENT_GREEN]    = { { 0x28CD41, 0x32D74B }, { 0x248A3D, 0x30DB5B } },
    [WS_ACCENT_GRAPHITE] = { { 0x8E8E93, 0x98989D }, { 0x6C6C70, 0xAEAEB2 } },
};

unsigned ui_accent_color(int accent, int dark, int contrast) {
    if (accent < 0 || accent >= WS_ACCENTS) accent = 0;
    return accents[accent][contrast != 0][dark != 0];
}

/* The desktop's gradients (System Settings > Desktop & Dock): top, bottom.
 * Gradient 0 is the default, a different pair in Dark. */
void ui_wall_gradient(int i, int dark, unsigned *top, unsigned *bottom) {
    static const unsigned g[WS_WALL_GRADIENTS][2] = {
        { 0x234AC4, 0x9B5CC9 },   /* Terminal OS (dark: 0x0F1F5C, 0x4A2B72) */
        { 0xFF7E5F, 0xFEB47B },   /* Sunset */
        { 0x2193B0, 0x6DD5ED },   /* Ocean */
        { 0x134E5E, 0x71B280 },   /* Forest */
        { 0x2C3E50, 0xFD746C },   /* Dusk */
        { 0x0F2027, 0x2C5364 },   /* Night */
        { 0xEE9CA7, 0xFFDDE1 },   /* Rose */
        { 0x1D2B64, 0xF8CDDA },   /* Aurora */
    };
    if (i < 0 || i >= WS_WALL_GRADIENTS) i = 0;
    *top = g[i][0];
    *bottom = g[i][1];
    if (i == 0 && dark) {
        *top = 0x0F1F5C;
        *bottom = 0x4A2B72;
    }
}

unsigned ui_color(int token) {
    if (token < 0 || token >= UI_C_COUNT) return 0;
    int accent = WS_PREF_ACCENT(prefs);
    if (token == UI_C_ACCENT && accent) return ui_accent_color(accent, ui_pref(UI_PREF_DARK), ui_pref(UI_PREF_CONTRAST));
    return color_table[ui_pref(UI_PREF_CONTRAST)][ui_pref(UI_PREF_DARK)][token];
}

/* `c` darkened by pct percent (a pressed accent button). */
static unsigned shade(unsigned c, int pct) {
    unsigned r = ((c >> 16) & 255) * (100 - pct) / 100, g = ((c >> 8) & 255) * (100 - pct) / 100,
             b = (c & 255) * (100 - pct) / 100;
    return r << 16 | g << 8 | b;
}

/* The ring sits 2 px outside the control: its radius is the control's + 2 px. */
void ui_focus_ring(ui_canvas *c, int x, int y, int w, int h, int r, int scale) {
    int g = 2 * scale;
    ui_round_ex(c, x - g, y - g, w + 2 * g, h + 2 * g, r + g, UI_ACCENT, ui_pref(UI_PREF_CONTRAST) ? 255 : 150, UI_ALL);
}

void ui_button_ex(ui_canvas *c, int x, int y, int w, int h, const char *label, int scale, int flags) {
    int r = (flags & UI_BTN_LARGE) ? UI_R_FULL : UI_R_MEDIUM * scale;      /* capsule, or radius.medium */
    int off = flags & UI_BTN_DISABLED, down = (flags & UI_BTN_PRESSED) && !off;
    int accent = (down || ((flags & UI_BTN_DEFAULT) && !off));
    unsigned face = off ? ui_color(UI_C_CONTROL_OFF) : accent ? (down ? shade(UI_ACCENT, 20) : UI_ACCENT) : ui_color(UI_C_CONTROL);
    unsigned ink = off ? ui_color(UI_C_TERTIARY) : accent ? UI_WHITE : UI_TEXT;
    if ((flags & UI_BTN_FOCUSED) && !off) ui_focus_ring(c, x, y, w, h, r > h / 2 ? h / 2 : r, scale);
    if (!off) ui_shadow(c, x, y + scale, w, h, r > h / 2 ? h / 2 : r, 2 * scale, 40);
    ui_round(c, x, y, w, h, r, accent ? face : ui_color(UI_C_CONTROL_EDGE));
    ui_round(c, x + 1, y + 1, w - 2, h - 2, r > h / 2 ? (h - 2) / 2 : r - 1, face);
    if ((flags & UI_BTN_HOVER) && !off)                                    /* hover: +10% */
        ui_round_ex(c, x, y, w, h, r, (accent || ui_pref(UI_PREF_DARK)) ? 0xFFFFFF : 0x000000, 26, UI_ALL);
    ui_text(c, x + (w - ui_text_width(label, scale)) / 2, y + (h - 16 * scale) / 2, label, ink, scale);
}

void ui_button(ui_canvas *c, int x, int y, int w, int h, const char *label, int scale, int pressed) {
    ui_button_ex(c, x, y, w, h, label, scale, pressed ? UI_BTN_PRESSED : 0);
}

void ui_textfield(ui_canvas *c, int x, int y, int w, int h, const char *text, int scale, int focused) {
    int r = UI_R_MEDIUM * scale;                                           /* radius.medium: inputs */
    if (focused) ui_focus_ring(c, x, y, w, h, r, scale);
    ui_round(c, x, y, w, h, r, focused ? UI_ACCENT : ui_color(UI_C_CONTROL_EDGE));
    ui_round(c, x + 1, y + 1, w - 2, h - 2, r - 1, ui_color(UI_C_FIELD));
    int room = w - 16 * scale;
    const char *shown = text;                           /* the end stays in view */
    while (*shown && ui_text_width(shown, scale) > room) shown++;
    int tx = x + 8 * scale, ty = y + (h - 16 * scale) / 2;
    tx += ui_text(c, tx, ty, shown, UI_TEXT, scale);
    if (focused) ui_fill(c, tx + scale, ty + scale, scale, 14 * scale, UI_ACCENT);
}

static int row_height(int scale) { return 24 * scale; }

int ui_list_rows(int h, int scale) { return h / row_height(scale); }
int ui_list_hit(int y, int scale) { return y < 0 ? -1 : y / row_height(scale); }

void ui_list_icons(ui_canvas *c, int x, int y, int w, int h, const char *const *items,
                   const char *const *icon_names, int count, int selected, int first, int scale) {
    ui_fill(c, x, y, w, h, ui_color(UI_C_CONTENT));
    int rows = ui_list_rows(h, scale), rh = row_height(scale), is = 16 * scale;
    for (int i = 0; i < rows && first + i < count; i++) {
        int k = first + i, ry = y + i * rh, tx = x + 16 * scale;
        if (k == selected) ui_round(c, x + 4 * scale, ry + 2 * scale, w - 8 * scale, rh - 4 * scale, UI_R_SMALL * scale, UI_ACCENT);
        else if (k % 2) ui_fill(c, x, ry, w, rh, ui_color(UI_C_ROW_ALT));
        if (icon_names && icon_names[k] && ui_icon(c, tx, ry + (rh - is) / 2, is, icon_names[k], 255) == 0)
            tx += is + 8 * scale;
        ui_text(c, tx, ry + (rh - 16 * scale) / 2, items[k], k == selected ? UI_WHITE : UI_TEXT, scale);
    }
}

void ui_list(ui_canvas *c, int x, int y, int w, int h, const char *const *items, int count,
             int selected, int first, int scale) {
    ui_list_icons(c, x, y, w, h, items, 0, count, selected, first, scale);
}

/* ---- Windows ------------------------------------------------------------- */

int ui_desktop(void) {
    int p = port_lookup(WS_SERVICE);
    if (p < 0) return 0;
    port_close(p);
    return 1;
}

int ui_scale(void) {
    struct os_gfx_info g;
    if (gfx_info(&g) < 0) return 1;
    return g.cell_w >= 16 ? 2 : 1;
}

int ui_open(ui_window *w, const char *title, int width, int height) {
    int server = -1;
    for (int i = 0; i < 20 && (server = port_lookup(WS_SERVICE)) < 0; i++) sleep_ms(50);
    if (server < 0) return server;
    struct ws_request q;
    memset(&q, 0, sizeof(q));
    q.op = WS_CREATE;
    q.w = width;
    q.h = height;
    q.pid = getpid();
    snprintf(q.title, sizeof(q.title), "%s", title);
    struct ws_reply r;
    int n = port_call(server, &q, sizeof(q), &r, sizeof(r), 5000);
    port_close(server);
    if (n < (int)sizeof(r)) return n < 0 ? n : SYSERR_BADCALL;
    if (r.id < 0) return r.id;
    width = r.w;                                     /* maybe smaller: it fits the screen */
    height = r.h;
    w->s = shm_map(r.shm, ws_surface_size(width, height));
    if (!w->s) return SYSERR_NOMEM;
    w->id = r.id;
    w->x = r.x;
    w->y = r.y;
    w->scale = ui_scale();
    prefs = w->s->prefs;
    ui_canvas_init(&w->c, (unsigned *)((char *)w->s + WS_HEADER), width, height, width);
    return 0;
}

void ui_set_title(ui_window *w, const char *title) {
    int server = port_lookup(WS_SERVICE);
    if (server < 0) return;
    struct ws_request q;
    memset(&q, 0, sizeof(q));
    q.op = WS_TITLE;
    q.id = w->id;
    q.pid = getpid();
    snprintf(q.title, sizeof(q.title), "%s", title);
    port_send(server, &q, sizeof(q), -1);
    port_close(server);
}

/* The frame is done: the server may show it. Returns once the server has
 * read it (by the CPU or the GPU), so the next drawing cannot be seen half
 * done; this also paces a program at the screen's rate. At most 50 ms (a
 * busy or stopped server does not stop the program). */
void ui_damage(ui_window *w, int x, int y, int width, int height) {
    ui_canvas *c = &w->c;
    int x0 = x < 0 ? 0 : x, y0 = y < 0 ? 0 : y;
    int x1 = x + width > c->w ? c->w : x + width, y1 = y + height > c->h ? c->h : y + height;
    touch(c, x0, y0, x1, y1);
}

void ui_present(ui_window *w) {
    prefs = w->s->prefs;
    ui_canvas *c = &w->c;
    struct ws_surface *s = w->s;
    if (c->dx1 <= c->dx0) {                          /* nothing through ui_: all of it (to be safe) */
        c->dx0 = c->dy0 = 0;
        c->dx1 = c->w;
        c->dy1 = c->h;
    }
    if (s->dirty_x1 <= s->dirty_x0) {                /* the server took the last: this one alone */
        s->dirty_x0 = c->dx0; s->dirty_y0 = c->dy0; s->dirty_x1 = c->dx1; s->dirty_y1 = c->dy1;
    } else {                                         /* not looked at yet: both */
        if (c->dx0 < s->dirty_x0) s->dirty_x0 = c->dx0;
        if (c->dy0 < s->dirty_y0) s->dirty_y0 = c->dy0;
        if (c->dx1 > s->dirty_x1) s->dirty_x1 = c->dx1;
        if (c->dy1 > s->dirty_y1) s->dirty_y1 = c->dy1;
    }
    c->dx0 = c->dy0 = c->dx1 = c->dy1 = 0;
    unsigned d = ++w->s->damage;
    unsigned long until = uptime_ms() + 50;
    while ((int)(w->s->released - d) < 0 && uptime_ms() < until) sleep_ms(1);
}

void ui_set_resizable(ui_window *w) {
    int server = port_lookup(WS_SERVICE);
    if (server < 0) return;
    struct ws_request q;
    memset(&q, 0, sizeof(q));
    q.op = WS_RESIZABLE;
    q.id = w->id;
    q.pid = getpid();
    port_send(server, &q, sizeof(q), -1);
    port_close(server);
}

/* WS_EV_RESIZE: a new surface of that size from the server, the canvas on it. */
static void resize(ui_window *w, int width, int height) {
    int server = port_lookup(WS_SERVICE);
    if (server < 0) return;
    struct ws_request q;
    memset(&q, 0, sizeof(q));
    q.op = WS_RESIZE;
    q.id = w->id;
    q.pid = getpid();
    q.w = width;
    q.h = height;
    struct ws_reply r;
    int n = port_call(server, &q, sizeof(q), &r, sizeof(r), 5000);
    port_close(server);
    if (n < (int)sizeof(r) || r.id < 0) return;
    struct ws_surface *s = shm_map(r.shm, ws_surface_size(width, height));
    if (!s) return;
    munmap(w->s);
    w->s = s;
    w->x = r.x;
    w->y = r.y;
    ui_canvas_init(&w->c, (unsigned *)((char *)s + WS_HEADER), width, height, width);
}

int ui_poll(ui_window *w, struct ws_event *e) {
    struct ws_surface *s = w->s;
    prefs = s->prefs;
    if (s->ev_tail == s->ev_head) return 0;
    *e = s->ev[s->ev_tail % WS_EVENTS];
    s->ev_tail++;
    /* Pointer moves with no button down: only the newest of a run counts
     * (hover). With a button down every one does (drawing, dragging). */
    while (e->type == WS_EV_MOVE && !e->buttons && s->ev_tail != s->ev_head &&
           s->ev[s->ev_tail % WS_EVENTS].type == WS_EV_MOVE && !s->ev[s->ev_tail % WS_EVENTS].buttons) {
        *e = s->ev[s->ev_tail % WS_EVENTS];
        s->ev_tail++;
    }
    if (e->type == WS_EV_RESIZE) resize(w, e->x, e->y);
    return 1;
}

/* Sleeps on the event counter: the server wakes it (a futex in the shared
 * surface) as soon as it adds an event. */
int ui_wait(ui_window *w, struct ws_event *e, int timeout_ms) {
    unsigned long until = uptime_ms() + (unsigned long)timeout_ms;
    while (!ui_poll(w, e)) {
        if (timeout_ms && uptime_ms() >= until) return 0;
        futex(FUTEX_WAIT, (volatile int *)&w->s->ev_head, (int)w->s->ev_tail);
    }
    return 1;
}