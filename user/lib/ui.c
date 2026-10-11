// ui.c - drawing, widgets, and windows from the window server (see ui.h)
//
// Text comes from UIFONT.BIN (scripts/gen-uifont.py): glyphs pre-rendered
// with anti-aliasing at each scale, blended onto the pixels. Shapes have
// anti-aliased round corners (4x4 samples per edge pixel). Without the font
// file, the console's 8x16 font is used.
#include <string.h>
#include <stdio.h>
#include <os.h>
#include <ui.h>

extern const unsigned char font8x16[256][16];

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

static inline void blend(ui_canvas *c, int x, int y, unsigned color, unsigned a) {
    if (a && inside_clip(c, x, y)) {
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

int ui_text_style(ui_canvas *c, int x, int y, const char *s, unsigned color, int scale, int style) {
    const face_t *f = find_face(style, scale);
    if (!f) {
        int start = x;
        while (*s) x += bitmap_char(c, x, y, (unsigned char)*s++, color, scale);
        return x - start;
    }
    y += (16 * scale - f->line_h) / 2;            /* a smaller face: centred */
    long x64 = (long)x * 64;
    for (; *s; s++) {
        const glyph_t *g = glyph(f, (unsigned char)*s);
        draw_glyph(c, f, g, x64, y, color);
        x64 += g->advance;
    }
    return (int)((x64 + 32) >> 6) - x;
}

int ui_text_style_width(const char *s, int scale, int style) {
    const face_t *f = find_face(style, scale);
    if (!f) return (int)strlen(s) * 8 * scale;
    long x64 = 0;
    for (; *s; s++) x64 += glyph(f, (unsigned char)*s)->advance;
    return (int)((x64 + 32) >> 6);
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

int ui_has_icon(const char *name) { return find_icon(name) != 0; }

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
    const icon_t *ic = find_icon(name);
    if (!ic || size <= 0) return -1;
    int k = 0;
    while (k < 2 && (int)ic->m[k].size < size) k++;
    int s = (int)ic->m[k].size;
    const unsigned *px = (const unsigned *)(icons + ic->m[k].offset);
    int y0 = y < c->cy0 ? c->cy0 : y, y1 = y + size > c->cy1 ? c->cy1 : y + size;
    int x0 = x < c->cx0 ? c->cx0 : x, x1 = x + size > c->cx1 ? c->cx1 : x + size;
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

void ui_button(ui_canvas *c, int x, int y, int w, int h, const char *label, int scale, int pressed) {
    ui_shadow(c, x, y + scale, w, h, 6 * scale, 2 * scale, 40);
    ui_round(c, x, y, w, h, 6 * scale, pressed ? UI_ACCENT : 0xD0D0D4);
    ui_round(c, x + 1, y + 1, w - 2, h - 2, 6 * scale - 1, pressed ? UI_ACCENT : UI_WHITE);
    ui_text(c, x + (w - ui_text_width(label, scale)) / 2, y + (h - 16 * scale) / 2, label,
            pressed ? UI_WHITE : UI_TEXT, scale);
}

void ui_textfield(ui_canvas *c, int x, int y, int w, int h, const char *text, int scale, int focused) {
    if (focused) ui_round(c, x - 2 * scale, y - 2 * scale, w + 4 * scale, h + 4 * scale, 7 * scale, 0x9CC2FA);
    ui_round(c, x, y, w, h, 5 * scale, focused ? UI_ACCENT : UI_BORDER);
    ui_round(c, x + 1, y + 1, w - 2, h - 2, 5 * scale - 1, UI_WHITE);
    int room = w - 12 * scale;
    const char *shown = text;                           /* the end stays in view */
    while (*shown && ui_text_width(shown, scale) > room) shown++;
    int tx = x + 6 * scale, ty = y + (h - 16 * scale) / 2;
    tx += ui_text(c, tx, ty, shown, UI_TEXT, scale);
    if (focused) ui_fill(c, tx + scale, ty + scale, scale, 14 * scale, UI_ACCENT);
}

static int row_height(int scale) { return 24 * scale; }

int ui_list_rows(int h, int scale) { return h / row_height(scale); }
int ui_list_hit(int y, int scale) { return y < 0 ? -1 : y / row_height(scale); }

void ui_list_icons(ui_canvas *c, int x, int y, int w, int h, const char *const *items,
                   const char *const *icon_names, int count, int selected, int first, int scale) {
    ui_fill(c, x, y, w, h, UI_WHITE);
    int rows = ui_list_rows(h, scale), rh = row_height(scale), is = 18 * scale;
    for (int i = 0; i < rows && first + i < count; i++) {
        int k = first + i, ry = y + i * rh, tx = x + 12 * scale;
        if (k == selected) ui_round(c, x + 4 * scale, ry + scale, w - 8 * scale, rh - 2 * scale, 5 * scale, UI_ACCENT);
        else if (k % 2) ui_fill(c, x, ry, w, rh, 0xF6F6F8);
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
    w->s = shm_map(r.shm, ws_surface_size(width, height));
    if (!w->s) return SYSERR_NOMEM;
    w->id = r.id;
    w->x = r.x;
    w->y = r.y;
    w->scale = ui_scale();
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

void ui_present(ui_window *w) { w->s->damage++; }

int ui_poll(ui_window *w, struct ws_event *e) {
    struct ws_surface *s = w->s;
    if (s->ev_tail == s->ev_head) return 0;
    *e = s->ev[s->ev_tail % WS_EVENTS];
    s->ev_tail++;
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
