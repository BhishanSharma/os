// shaders.cl - the GPU's programs for the desktop (docs/GPU.md).
//
// Compiled by Intel's offline compiler (ocloc, toolchain/gpu) for Gen12 (Xe-LP)
// into src/intf/drivers/gpu_shaders.h, which the kernel's driver (igfx.c) runs on the render
// engine. Each kernel takes no arguments (no payload to lay out): its
// parameters are a 64-byte block at PARAMS + 64 * job, where job is the
// thread group's z. A thread (SIMD16) handles 16 pixels of one row: x =
// group x * 16 + lane, y = group y. Memory is addressed through the driver's
// GPU page tables (A64, stateless).
//
// Pixels are 0x00RRGGBB (the desktop's frame) unless noted. The arithmetic
// is the CPU's (windowserver.c material(), draw_faded()), integer for
// integer, so the self-tests can compare results exactly.

#define PARAMS 0x3C100000UL

typedef struct {
    ulong dst, src;          // GPU addresses
    uint dst_pitch, src_pitch;   // in pixels
    uint w, h;               // the destination's size
    uint a, b, c, d;         // per kernel
    uint e, f;
} params_t;

#define LANES 16
#define KERNEL __attribute__((reqd_work_group_size(LANES, 1, 1))) __attribute__((intel_reqd_sub_group_size(LANES))) __kernel

static __global params_t *job(void) {
    return (__global params_t *)(PARAMS + 64UL * get_group_id(2));
}

static uint lerp_rgb(uint a, uint b, uint f) {   // f of 256
    if (f > 256) f = 256;
    uint r = (((a >> 16) & 255) * (256 - f) + ((b >> 16) & 255) * f) >> 8;
    uint g = (((a >> 8) & 255) * (256 - f) + ((b >> 8) & 255) * f) >> 8;
    uint bl = ((a & 255) * (256 - f) + (b & 255) * f) >> 8;
    return r << 16 | g << 8 | bl;
}

// fill: dst = a (the self-test).
KERNEL void fill(void) {
    __global params_t *p = job();
    uint x = get_group_id(0) * LANES + get_sub_group_local_id(), y = get_group_id(1);
    if (x < p->w && y < p->h) ((__global uint *)p->dst)[y * p->dst_pitch + x] = p->a;
}

// The glass's samples are 10 bits a channel (r << 20 | g << 10 | b, 0-1023):
// the blur and the tint keep a quarter level, and upscale dithers them down,
// so a gentle gradient behind the glass is not cut into bands.

// box3: one 3-tap box pass (a = 1: horizontal, 0: vertical), clamped at the edges.
KERNEL void box3(void) {
    __global params_t *p = job();
    uint x = get_group_id(0) * LANES + get_sub_group_local_id(), y = get_group_id(1);
    if (x >= p->w || y >= p->h) return;
    __global const uint *s = (__global const uint *)p->src;
    uint r = 0, g = 0, b = 0;
    for (int d = -1; d <= 1; d++) {
        int xx = p->a ? clamp((int)x + d, 0, (int)p->w - 1) : (int)x;
        int yy = p->a ? (int)y : clamp((int)y + d, 0, (int)p->h - 1);
        uint c = s[yy * p->src_pitch + xx];
        r += c >> 20 & 1023;
        g += c >> 10 & 1023;
        b += c & 1023;
    }
    ((__global uint *)p->dst)[y * p->dst_pitch + x] = (r / 3) << 20 | (g / 3) << 10 | (b / 3);
}

// satint: saturation a (percent), then tint b over it with alpha c (0-255).
KERNEL void satint(void) {
    __global params_t *p = job();
    uint x = get_group_id(0) * LANES + get_sub_group_local_id(), y = get_group_id(1);
    if (x >= p->w || y >= p->h) return;
    uint c = ((__global const uint *)p->src)[y * p->src_pitch + x];
    int sat = (int)p->a, alpha = (int)p->c;
    uint tint = p->b;
    int r = (int)(c >> 20 & 1023), g = (int)(c >> 10 & 1023), b = (int)(c & 1023);
    int gray = (r * 30 + g * 59 + b * 11) / 100;
    r = clamp(gray + (r - gray) * sat / 100, 0, 1023);
    g = clamp(gray + (g - gray) * sat / 100, 0, 1023);
    b = clamp(gray + (b - gray) * sat / 100, 0, 1023);
    r = (r * (255 - alpha) + (int)(tint >> 16 & 255) * 4 * alpha) / 255;   /* (the tint is 8-bit) */
    g = (g * (255 - alpha) + (int)(tint >> 8 & 255) * 4 * alpha) / 255;
    b = (b * (255 - alpha) + (int)(tint & 255) * 4 * alpha) / 255;
    ((__global uint *)p->dst)[y * p->dst_pitch + x] = (uint)(r << 20 | g << 10 | b);
}

// The wallpaper's 8x8 ordered dither (windowserver.c dither16()).
__constant uchar bayer8[64] = {
    0, 32, 8, 40, 2, 34, 10, 42, 48, 16, 56, 24, 50, 18, 58, 26, 12, 44, 4, 36, 14, 46, 6, 38,
    60, 28, 52, 20, 62, 30, 54, 22, 3, 35, 11, 43, 1, 33, 9, 41, 51, 19, 59, 27, 49, 17, 57, 25,
    15, 47, 7, 39, 13, 45, 5, 37, 63, 31, 55, 23, 61, 29, 53, 21 };

// One channel of four 10-bit samples, bilinear (f, rf of 256), to 8 bits:
// rounded with the dither threshold t, in steps of `step` levels (4 on a
// 6-bit panel, which shows only every fourth level).
static uint up_channel(uint a0, uint a1, uint b0, uint b1, uint f, uint rf, uint t, uint step) {
    uint top = a0 * (256 - f) + a1 * f, bot = b0 * (256 - f) + b1 * f;
    uint v = (top * (256 - rf) + bot * rf) >> 10;      // 8-bit levels, in 1/256ths
    uint q = (v + t * step) / (256 * step) * step;
    return q > 255 ? 255 : q;
}

// upscale: the small src (a x b, 10-bit samples) over the w x h destination,
// bilinear, as material() does it: bleed c, rows from d (the first row drawn),
// dithered in steps of e levels.
KERNEL void upscale(void) {
    __global params_t *p = job();
    uint i = get_group_id(0) * LANES + get_sub_group_local_id(), jj = get_group_id(1);
    if (i >= p->w || jj >= p->h) return;
    int bw = (int)p->a, bh = (int)p->b, bleed = (int)p->c, j = (int)jj + (int)p->d;
    int u = ((int)i + bleed) * 64 - 96, c0 = u >> 8;
    uint f = (uint)(u & 255);
    if (c0 < 0) { c0 = 0; f = 0; }
    if (c0 >= bw - 1) { c0 = bw - 1; f = 0; }
    int v = (j + bleed) * 64 - 96, r0 = v >> 8;
    uint rf = (uint)(v & 255);
    if (r0 < 0) { r0 = 0; rf = 0; }
    if (r0 >= bh - 1) { r0 = bh - 1; rf = 0; }
    __global const uint *s = (__global const uint *)p->src;
    __global const uint *a = s + r0 * (int)p->src_pitch, *b = rf ? a + p->src_pitch : a;
    int c1 = f ? c0 + 1 : c0;
    uint a0 = a[c0], a1 = a[c1], b0 = b[c0], b1 = b[c1];
    uint t = (uint)bayer8[(j & 7) * 8 + (i & 7)] * 4 + 2, step = p->e ? p->e : 1;
    uint r = up_channel(a0 >> 20 & 1023, a1 >> 20 & 1023, b0 >> 20 & 1023, b1 >> 20 & 1023, f, rf, t, step);
    uint g = up_channel(a0 >> 10 & 1023, a1 >> 10 & 1023, b0 >> 10 & 1023, b1 >> 10 & 1023, f, rf, t, step);
    uint bl = up_channel(a0 & 1023, a1 & 1023, b0 & 1023, b1 & 1023, f, rf, t, step);
    ((__global uint *)p->dst)[jj * p->dst_pitch + i] = r << 16 | g << 8 | bl;
}

// fade: dst = lerp(src (the old picture), dst (the new), a), as draw_faded().
KERNEL void fade(void) {
    __global params_t *p = job();
    uint x = get_group_id(0) * LANES + get_sub_group_local_id(), y = get_group_id(1);
    if (x >= p->w || y >= p->h) return;
    __global uint *d = (__global uint *)p->dst + y * p->dst_pitch + x;
    *d = lerp_rgb(((__global const uint *)p->src)[y * p->src_pitch + x], *d, p->a);
}

// ---- Drawing onto the frame (gfx GFX_GPU_DRAW) --------------------------------
// The thread grid covers a region of the canvas (dst: its first pixel); the
// shape's rectangle is given from the region's corner (a, b: often negative).

static uint div255(uint v) { return (v + 1 + (v >> 8)) >> 8; }

static uint mix255(uint dst, uint color, uint a) {   // as ui.c
    if (a >= 255) return color;
    uint na = 255 - a;
    uint r = div255(((color >> 16) & 255) * a + ((dst >> 16) & 255) * na);
    uint g = div255(((color >> 8) & 255) * a + ((dst >> 8) & 255) * na);
    uint b = div255((color & 255) * a + (dst & 255) * na);
    return r << 16 | g << 8 | b;
}

static int isqrt_i(int v) {
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

static uint corner_cover(int i, int j, int r) {
    int R = 8 * r, inside = 0;
    for (int sy = 0; sy < 4; sy++) {
        int dy = R - (8 * j + 2 * sy + 1);
        for (int sx = 0; sx < 4; sx++) {
            int dx = R - (8 * i + 2 * sx + 1);
            if (dx <= 0 && dy <= 0) inside++;
            else if ((dx > 0 ? dx * dx : 0) + (dy > 0 ? dy * dy : 0) <= R * R) inside++;
        }
    }
    return (uint)(inside * 255 / 16);
}

static uint rect_cover(int i, int j, int w, int h, int r, int corners) {
    int ci = -1, cj = -1;
    if (i < r) ci = i;
    else if (i >= w - r) ci = w - 1 - i;
    if (j < r) cj = j;
    else if (j >= h - r) cj = h - 1 - j;
    if (ci < 0 || cj < 0) return 255;
    int which = (j < r ? (i < r ? 1 : 2) : (i < r ? 4 : 8));
    if (!(corners & which)) return 255;
    return corner_cover(ci, cj, r);
}

// A shadow's darkening, dithered: a_fx is the alpha in 1/256ths, the result
// rounded in steps of `step` levels with the 8x8 threshold t (as ui.c).
static uint shade_dith(uint c, int a_fx, uint t, uint step) {
    int v = (int)c * 256 - (int)c * a_fx / 255;        // the channel, in 1/256ths of a level
    uint q = ((uint)v + t * step) / (256 * step) * step;
    return q > 255 ? 255 : q;
}

// shadow: ui_shadow_ex. Rectangle (a, b) c x d, e = radius | size << 16,
// f = alpha | outside << 16 | dither step << 24. Its middle (under the thing)
// is left alone.
KERNEL void shadow(void) {
    __global params_t *p = job();
    int px = (int)(get_group_id(0) * LANES + get_sub_group_local_id()), py = (int)get_group_id(1);
    if (px >= (int)p->w || py >= (int)p->h) return;
    int x = (int)p->a, y = (int)p->b, w = (int)p->c, h = (int)p->d;
    int r = (int)(p->e & 0xFFFF), size = (int)(p->e >> 16), alpha = (int)(p->f & 0xFFFF), outside = (int)(p->f >> 16 & 255);
    uint step = p->f >> 24 ? p->f >> 24 : 1;
    int hs = size / 2;
    if (py >= y + hs && py < y + h - hs && px >= x + r + hs && px < x + w - r - hs) return;
    int qy = y + r - py;
    if (py - (y + h - 1 - r) > qy) qy = py - (y + h - 1 - r);
    if (qy < 0) qy = 0;
    int qx = x + r - px;
    if (px - (x + w - 1 - r) > qx) qx = px - (x + w - 1 - r);
    if (qx < 0) qx = 0;
    int d = isqrt_i((qx * qx + qy * qy) * 16) - r * 4;
    if (d <= 0 && outside) return;
    if (d <= 0) {
        int ix = px - x < x + w - 1 - px ? px - x : x + w - 1 - px;
        int iy = py - y < y + h - 1 - py ? py - y : y + h - 1 - py;
        int in = ix < iy ? ix : iy;
        if (in > r && qx == 0 && qy == 0) d = -in * 4;
    }
    int span = size * 6, f = size * 4 - d;
    if (f <= 0) return;
    if (f > span) f = span;
    int a_fx = alpha * 256 * f / span * f / span;      // 1/256ths: the faint edge gets every shade
    if (!a_fx) return;
    __global uint *o = (__global uint *)p->dst + py * p->dst_pitch + px;
    uint c = *o, t = (uint)bayer8[(py & 7) * 8 + (px & 7)] * 4 + 2;
    *o = shade_dith(c >> 16 & 255, a_fx, t, step) << 16 | shade_dith(c >> 8 & 255, a_fx, t, step) << 8 |
         shade_dith(c & 255, a_fx, t, step);
}

// over: a picture with premultiplied alpha (src, the region's pixel) over
// the canvas, except the hole [a, c) x [b, d).
KERNEL void over(void) {
    __global params_t *p = job();
    int x = (int)(get_group_id(0) * LANES + get_sub_group_local_id()), y = (int)get_group_id(1);
    if (x >= (int)p->w || y >= (int)p->h) return;
    if (x >= (int)p->a && x < (int)p->c && y >= (int)p->b && y < (int)p->d) return;
    uint v = ((__global const uint *)p->src)[y * p->src_pitch + x], a = v >> 24;
    if (!a) return;
    __global uint *o = (__global uint *)p->dst + y * p->dst_pitch + x;
    if (a == 255) {
        *o = v & 0xFFFFFF;
        return;
    }
    uint d = *o, k = 255 - a;
    uint r = ((v >> 16) & 255) + ((d >> 16) & 255) * k / 255;
    uint g = ((v >> 8) & 255) + ((d >> 8) & 255) * k / 255;
    uint bl = (v & 255) + (d & 255) * k / 255;
    *o = min(r, 255u) << 16 | min(g, 255u) << 8 | min(bl, 255u);
}

// rcopy: ui_copy_round. Rectangle (a, b) c x d, radius e, corners f; src is
// the picture's pixel at the region's corner.
KERNEL void rcopy(void) {
    __global params_t *p = job();
    int x = (int)(get_group_id(0) * LANES + get_sub_group_local_id()), y = (int)get_group_id(1);
    if (x >= (int)p->w || y >= (int)p->h) return;
    int i = x - (int)p->a, j = y - (int)p->b, w = (int)p->c, h = (int)p->d, r = (int)p->e;
    uint from = ((__global const uint *)p->src)[y * p->src_pitch + x];
    __global uint *o = (__global uint *)p->dst + y * p->dst_pitch + x;
    if (j >= r && j < h - r) {
        *o = from;
        return;
    }
    uint a = (i < r || i >= w - r) ? rect_cover(i, j, w, h, r, (int)p->f) : 255;
    *o = mix255(*o, from, a);
}

// scaled: blit_scaled. The ow x oh picture (src, src_pitch = ow) and its shape
// (e: GPU address, same layout) bilinear into (a, b) dw x dh (c = dw | dh <<
// 16), d = oh | alpha << 16; f: what was under it (region-sized), or 0.
KERNEL void scaled(void) {
    __global params_t *p = job();
    int x = (int)(get_group_id(0) * LANES + get_sub_group_local_id()), y = (int)get_group_id(1);
    if (x >= (int)p->w || y >= (int)p->h) return;
    int dw = (int)(p->c & 0xFFFF), dh = (int)(p->c >> 16), ow = (int)p->src_pitch, oh = (int)(p->d & 0xFFFF);
    uint alpha = p->d >> 16;
    int v = ((2 * (y - (int)p->b) + 1) * oh * 128) / dh - 128, ty = v >> 8;
    uint fy = (uint)(v & 255);
    if (ty < 0) { ty = 0; fy = 0; }
    if (ty >= oh - 1) { ty = oh - 1; fy = 0; }
    int u = ((2 * (x - (int)p->a) + 1) * ow * 128) / dw - 128, tx = u >> 8;
    uint fx = (uint)(u & 255);
    if (tx < 0) { tx = 0; fx = 0; }
    if (tx >= ow - 1) { tx = ow - 1; fx = 0; }
    __global const uint *c0 = (__global const uint *)p->src + ty * ow, *c1 = fy ? c0 + ow : c0;
    __global const uint *m0 = (__global const uint *)(ulong)p->e + ty * ow, *m1 = fy ? m0 + ow : m0;
    uint col, msk;
    if (!fx && !fy) {
        col = c0[tx];
        msk = m0[tx] & 255;
    } else {
        uint ta = fx ? lerp_rgb(c0[tx], c0[tx + 1], fx) : c0[tx];
        uint tb = fx ? lerp_rgb(c1[tx], c1[tx + 1], fx) : c1[tx];
        col = fy ? lerp_rgb(ta, tb, fy) : ta;
        ta = fx ? lerp_rgb(m0[tx], m0[tx + 1], fx) : m0[tx];
        tb = fx ? lerp_rgb(m1[tx], m1[tx + 1], fx) : m1[tx];
        msk = (fy ? lerp_rgb(ta, tb, fy) : ta) & 255;
    }
    uint a = msk * alpha / 255;
    if (!a) return;
    __global uint *o = (__global uint *)p->dst + y * p->dst_pitch + x;
    if (a >= 255) {
        *o = col;
        return;
    }
    uint base = (p->f && msk >= 255) ? ((__global const uint *)(ulong)p->f)[y * p->w + x] : *o;
    *o = lerp_rgb(base, col, a + (a >> 7));
}

// rfill: ui_round_ex. Rectangle (a, b) c x d, radius e, f = corners | alpha
// << 8, the colour in src.
KERNEL void rfill(void) {
    __global params_t *p = job();
    int x = (int)(get_group_id(0) * LANES + get_sub_group_local_id()), y = (int)get_group_id(1);
    if (x >= (int)p->w || y >= (int)p->h) return;
    int i = x - (int)p->a, j = y - (int)p->b, w = (int)p->c, h = (int)p->d, r = (int)p->e;
    uint alpha = p->f >> 8, color = (uint)p->src;
    uint cover = ((j < r || j >= h - r) && (i < r || i >= w - r)) ? rect_cover(i, j, w, h, r, (int)(p->f & 15)) : 255;
    uint a = cover * alpha / 255;
    if (!a) return;
    __global uint *o = (__global uint *)p->dst + y * p->dst_pitch + x;
    *o = mix255(*o, color, a);
}
