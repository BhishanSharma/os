// icons.c - the window server's icons (see icons.h)
#include <string.h>
#include <ui.h>
#include "icons.h"

static int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

/* Icons ------------------------------------------------------------------------
 * Drawn here, from a few strokes on a 24 x 24 grid (stroke 1.75, round caps),
 * so an icon can be any colour: the colour of its item's state. Each pixel's
 * coverage comes from its distance to the strokes (1/16 pixel, integers
 * only). Coordinates below are in half grid units (24 -> 48). */
enum { P_LINE, P_RING, P_DISC, P_RRECT, P_FRECT, P_CUT };
typedef struct { unsigned char t; short a, b, c, d, e; } prim_t;
#define GL_MAXP 44
static struct { prim_t p[GL_MAXP]; int n; } glyphs[GL_COUNT];
static int gl_cur;

static void gp(int t, int a, int b, int c, int d, int e) {
    if (glyphs[gl_cur].n == GL_MAXP) return;
    prim_t *p = &glyphs[gl_cur].p[glyphs[gl_cur].n++];
    p->t = (unsigned char)t;
    p->a = (short)a; p->b = (short)b; p->c = (short)c; p->d = (short)d; p->e = (short)e;
}
static void g_line(int x0, int y0, int x1, int y1) { gp(P_LINE, x0, y0, x1, y1, 0); }
static void g_ring(int cx, int cy, int r) { gp(P_RING, cx, cy, r, 0, 0); }
static void g_disc(int cx, int cy, int r) { gp(P_DISC, cx, cy, r, 0, 0); }
static void g_cut(int cx, int cy, int r) { gp(P_CUT, cx, cy, r, 0, 0); }
static void g_box(int cx, int cy, int hw, int hh, int r) { gp(P_RRECT, cx, cy, hw, hh, r); }
static void g_poly(const signed char *xy, int n, int closed) {
    for (int i = 0; i + 1 < n; i++) g_line(xy[2 * i], xy[2 * i + 1], xy[2 * i + 2], xy[2 * i + 3]);
    if (closed) g_line(xy[2 * (n - 1)], xy[2 * (n - 1) + 1], xy[0], xy[1]);
}

/* cos of k * 15 degrees, 1/1024; angles run clockwise on the screen */
static const short cos15[24] = { 1024, 989, 887, 724, 512, 265, 0, -265, -512, -724, -887, -989,
                                 -1024, -989, -887, -724, -512, -265, 0, 265, 512, 724, 887, 989 };
static int c15(int k) { return cos15[((k % 24) + 24) % 24]; }
static int s15(int k) { return c15(k - 6); }
static void g_arc(int cx, int cy, int rx, int ry, int k0, int k1) {
    for (int k = k0; k < k1; k++)
        g_line(cx + rx * c15(k) / 1024, cy + ry * s15(k) / 1024, cx + rx * c15(k + 1) / 1024, cy + ry * s15(k + 1) / 1024);
}
static void g_ray(int cx, int cy, int r0, int r1, int k) {
    g_line(cx + r0 * c15(k) / 1024, cy + r0 * s15(k) / 1024, cx + r1 * c15(k) / 1024, cy + r1 * s15(k) / 1024);
}

#define POLY(a) a, (int)(sizeof(a) / 2)
void glyphs_init(void) {
    static const signed char tab[] = { 8, 16, 8, 12, 16, 12, 19, 16 };
    static const signed char layer_a[] = { 24, 6, 42, 16, 24, 26, 6, 16 };
    static const signed char layer_b[] = { 6, 25, 24, 35, 42, 25 };
    static const signed char layer_c[] = { 6, 33, 24, 43, 42, 33 };
    static const signed char needle[] = { 31, 17, 27, 27, 17, 31, 21, 21 };
    static const signed char hex[] = { 24, 5, 40, 14, 40, 34, 24, 43, 8, 34, 8, 14 };
    static const signed char bell[] = { 9, 35, 12, 32, 12, 22, 15, 15, 20, 11, 24, 10, 28, 11, 33, 15, 36, 22, 36, 32, 39, 35 };
    static const signed char spk[] = { 7, 19, 14, 19, 22, 12, 22, 36, 14, 29, 7, 29 };
    static const signed char chev[] = { 14, 18, 21, 24, 14, 30 };
    static const signed char beam[] = { 20, 13, 38, 9 };
    static const signed char play[] = { 33, 20, 42, 15, 42, 33, 33, 28 };
    static const signed char pulse[] = { 5, 24, 14, 24, 19, 11, 28, 37, 33, 24, 43, 24 };
    static const signed char hill[] = { 8, 34, 17, 25, 24, 32, 30, 27, 40, 35 };
    static const signed char back[] = { 17, 17, 17, 9, 39, 9, 39, 31, 31, 31 };

    gl_cur = GL_DOOR;     g_box(24, 22, 12, 17, 3); g_disc(30, 23, 2); g_line(6, 43, 42, 43);   /* a door with its knob, on the floor */
    gl_cur = GL_APPS;     g_box(15, 15, 6, 6, 3); g_box(33, 15, 6, 6, 3); g_box(15, 33, 6, 6, 3); g_box(33, 33, 6, 6, 3);
    gl_cur = GL_FOLDER;   g_box(24, 28, 18, 12, 5); g_poly(POLY(tab), 0);
    gl_cur = GL_LAYERS;   g_poly(POLY(layer_a), 1); g_poly(POLY(layer_b), 0); g_poly(POLY(layer_c), 0);
    gl_cur = GL_COMPASS;  g_ring(24, 24, 18); g_poly(POLY(needle), 1);
    gl_cur = GL_GEAR;     g_poly(POLY(hex), 1); g_ring(24, 24, 6);
    gl_cur = GL_USER;     g_ring(24, 17, 7); g_arc(24, 41, 14, 14, 12, 24);
    gl_cur = GL_SEARCH;   g_ring(21, 21, 13); g_line(31, 31, 41, 41);
    gl_cur = GL_BELL;     g_poly(POLY(bell), 0); g_line(9, 35, 39, 35); g_arc(24, 37, 4, 4, 0, 12); g_disc(24, 8, 3);
    gl_cur = GL_WIFI;     g_arc(24, 39, 9, 9, 15, 21); g_arc(24, 39, 18, 18, 15, 21); g_arc(24, 39, 27, 27, 15, 21); g_disc(24, 39, 2);
    gl_cur = GL_SOUND;    g_poly(POLY(spk), 1); g_arc(25, 24, 9, 9, 21, 27); g_arc(25, 24, 17, 17, 21, 27);
    gl_cur = GL_BATTERY;  g_box(22, 24, 18, 10, 5); g_line(42, 20, 42, 28);
    gl_cur = GL_MOON;     g_disc(24, 24, 18); g_cut(33, 16, 15);
    gl_cur = GL_SUN;      g_ring(24, 24, 7);
    for (int k = 0; k < 24; k += 3) g_ray(24, 24, 12, 18, k);
    gl_cur = GL_POWER;    g_arc(24, 26, 16, 16, 21, 39); g_line(24, 6, 24, 22);
    gl_cur = GL_TERM;     g_box(24, 24, 19, 16, 5); g_poly(POLY(chev), 0); g_line(25, 31, 33, 31);
    gl_cur = GL_DOC;      g_box(24, 24, 14, 18, 5); g_line(17, 18, 31, 18); g_line(17, 25, 31, 25); g_line(17, 32, 25, 32);
    gl_cur = GL_MUSIC;    g_disc(15, 36, 5); g_disc(33, 32, 5); g_line(20, 36, 20, 13); g_line(38, 32, 38, 9); g_poly(POLY(beam), 0);
    gl_cur = GL_GLOBE;    g_ring(24, 24, 18); g_line(6, 24, 42, 24); g_arc(24, 24, 7, 18, 0, 24);
    gl_cur = GL_VIDEO;    g_box(21, 24, 16, 12, 5); g_poly(POLY(play), 1);
    gl_cur = GL_PAINT;    g_ring(24, 24, 18); g_disc(16, 20, 2); g_disc(24, 14, 2); g_disc(32, 20, 2); g_disc(17, 31, 2);
    gl_cur = GL_PULSE;    g_poly(POLY(pulse), 0);
    gl_cur = GL_INFO;     g_ring(24, 24, 18); g_line(24, 22, 24, 34); g_disc(24, 15, 2);
    gl_cur = GL_COMPUTER; g_box(24, 21, 18, 12, 4); g_line(18, 40, 30, 40); g_line(24, 33, 24, 40);
    gl_cur = GL_IMAGE;    g_box(24, 24, 18, 15, 5); g_disc(32, 18, 3); g_poly(POLY(hill), 0);
    gl_cur = GL_MOUSE;    g_box(24, 24, 10, 17, 10); g_line(24, 7, 24, 19);
    gl_cur = GL_LOGO;     g_arc(24, 24, 18, 18, 22, 44); g_disc(37, 11, 3);                   /* the DOOR mark */
    gl_cur = GL_MIN;      g_line(10, 24, 38, 24);
    gl_cur = GL_MAX;      g_box(24, 24, 13, 13, 4);
    gl_cur = GL_RESTORE;  g_box(20, 28, 11, 11, 3); g_poly(POLY(back), 0);
    gl_cur = GL_CLOSE;    g_line(12, 12, 36, 36); g_line(36, 12, 12, 36);
}

int isq(long n) {
    if (n <= 0) return 0;
    long x = n < 4 ? n : (1L << ((64 - __builtin_clzl((unsigned long)n) + 1) / 2));
    for (;;) {
        long y = (x + n / x) / 2;
        if (y >= x) return (int)x;
        x = y;
    }
}

/* Coverage 0..16 of one stroke or shape at (px, py), all in 1/16 pixel. */
static int prim_cover(const prim_t *q, int px, int py, int hw) {
    long dx = px - q->a, dy = py - q->b;
    int d;
    switch (q->t) {
    case P_LINE: {
        long vx = q->c - q->a, vy = q->d - q->b, l2 = vx * vx + vy * vy, t = dx * vx + dy * vy;
        if (t < 0) t = 0;
        else if (t > l2) t = l2;
        long ex = l2 ? dx - vx * t / l2 : dx, ey = l2 ? dy - vy * t / l2 : dy;
        d = isq(ex * ex + ey * ey);
        return clampi(hw - d + 8, 0, 16);
    }
    case P_RING:
        d = isq(dx * dx + dy * dy) - q->c;
        if (d < 0) d = -d;
        return clampi(hw - d + 8, 0, 16);
    case P_DISC:
    case P_CUT:
        d = isq(dx * dx + dy * dy) - q->c;
        return clampi(8 - d, 0, 16);
    default: {                                          /* P_RRECT outline, P_FRECT filled */
        long ax = dx < 0 ? -dx : dx, ay = dy < 0 ? -dy : dy;
        long qx = ax - (q->c - q->e), qy = ay - (q->d - q->e);
        int out = isq((qx > 0 ? qx * qx : 0) + (qy > 0 ? qy * qy : 0));
        int in = (int)(qx > qy ? qx : qy);
        if (in > 0) in = 0;
        int sd = out + in - q->e;
        if (q->t == P_FRECT) return clampi(8 - sd, 0, 16);
        if (sd < 0) sd = -sd;
        return clampi(hw - sd + 8, 0, 16);
    }
    }
}

/* An icon, `size` pixels square (24 * S is the design size), in `color`.
 * `level` (GL_BATTERY): the charge in percent, drawn inside. */
void gl_draw(ui_canvas *cv, int x, int y, int size, int id, unsigned color, int alpha, int level) {
    if (id <= 0 || id >= GL_COUNT || alpha <= 0 || size <= 0) return;
    if (x >= cv->cx1 || y >= cv->cy1 || x + size <= cv->cx0 || y + size <= cv->cy0) return;
    prim_t q[GL_MAXP + 1];
    int n = glyphs[id].n;
    for (int i = 0; i < n; i++) {                       /* half grid units -> 1/16 pixel */
        const prim_t *s = &glyphs[id].p[i];
        q[i].t = s->t;
        q[i].a = (short)(s->a * size / 3); q[i].b = (short)(s->b * size / 3);
        q[i].c = (short)(s->c * size / 3); q[i].d = (short)(s->d * size / 3);
        q[i].e = (short)(s->e * size / 3);
    }
    if (id == GL_BATTERY && level >= 0) {
        int w = 28 * clampi(level, 4, 100) / 100;       /* inner width in half units */
        q[n].t = P_FRECT;
        q[n].a = (short)((8 + w / 2) * size / 3);
        q[n].b = (short)(24 * size / 3);
        q[n].c = (short)((w / 2 > 1 ? w / 2 : 1) * size / 3);
        q[n].d = (short)(6 * size / 3);
        q[n].e = (short)(2 * size / 3);
        n++;
    }
    int hw = size * 7 / 12;                             /* half the stroke: 0.875 px at 24 */
    if (hw < 6) hw = 6;
    int has_cut = 0;
    for (int i = 0; i < n; i++) if (q[i].t == P_CUT) has_cut = 1;
    int y0 = y < cv->cy0 ? cv->cy0 - y : 0, y1 = y + size > cv->cy1 ? cv->cy1 - y : size;
    int x0 = x < cv->cx0 ? cv->cx0 - x : 0, x1 = x + size > cv->cx1 ? cv->cx1 - x : size;
    for (int iy = y0; iy < y1; iy++)
        for (int ix = x0; ix < x1; ix++) {
            int px = ix * 16 + 8, py = iy * 16 + 8, cov = 0, cut = 0;
            for (int i = 0; i < n && (cov < 16 || has_cut); i++) {
                int c = prim_cover(&q[i], px, py, hw);
                if (q[i].t == P_CUT) { if (c > cut) cut = c; }
                else if (c > cov) cov = c;
            }
            if (cut) cov = cov * (16 - cut) / 16;
            if (cov) ui_blend_pixel(cv, x + ix, y + iy, color, cov * alpha / 16);
        }
}

/* The glyph for an app.cfg icon name; 0: none (a picture or a letter instead). */
int glyph_for(const char *icon) {
    static const struct { const char *name; int gl; } map[] = {
        { "folder", GL_FOLDER }, { "gear", GL_GEAR }, { "terminal", GL_TERM }, { "textedit", GL_DOC },
        { "file", GL_DOC }, { "paint", GL_PAINT }, { "activity", GL_PULSE }, { "music", GL_MUSIC },
        { "sound", GL_SOUND }, { "video", GL_VIDEO }, { "info", GL_INFO }, { "computer", GL_COMPUTER },
        { "picture", GL_IMAGE }, { "browser", GL_GLOBE }, { "user", GL_USER }, { "mouse", GL_MOUSE },
        { "wifi", GL_WIFI }, { "battery", GL_BATTERY },
    };
    if (!icon) return 0;
    for (unsigned i = 0; i < sizeof(map) / sizeof(map[0]); i++)
        if (!strcmp(icon, map[i].name)) return map[i].gl;
    return 0;
}
