// paint [file.bmp] - Paint: draw with the mouse or touchpad. Twelve colours,
// three brush sizes, an eraser; Save writes a BMP picture (open it in Files).
//
// Built from views (view.h): a toolbar (the colours and brushes are small
// custom views, the rest buttons and a label) and the canvas, a custom view
// over the picture's own pixels: a stroke paints the picture and copies just
// the dabs to the window; a redraw of the view copies the whole picture.
#include <stdio.h>
#include <string.h>
#include <os.h>
#include <view.h>

static const unsigned colors[] = { 0x1E1E1E, 0x8A8A8E, 0xFF3B30, 0xFF9500, 0xFFCC00, 0x34C759,
                                   0x00C7BE, 0x30B0FF, 0x007AFF, 0x5856D6, 0xAF52DE, 0xFF2D55 };
#define COLORS 12

static ui_window win;
static int S, color = 8, brush = 1, eraser, down, lx, ly;
static char path[128] = "drawing.bmp";
static view_t *palette, *brushes, *eraser_btn, *status, *canvas;
static ui_canvas pic;                                  /* the picture */

static int brush_radius(void) { return (brush == 0 ? 2 : brush == 1 ? 5 : 12) * S; }

/* ---- The toolbar ------------------------------------------------------------- */

static void draw_palette(view_t *v, ui_canvas *c) {
    ui_fill(c, v->x, v->y, v->w, v->h, ui_color(UI_C_TOOLBAR));
    int x = v->x, y = v->y + v->h / 2;
    for (int i = 0; i < COLORS; i++, x += 26 * S) {
        if (i == color && !eraser) ui_circle(c, x + 12 * S, y, 12 * S, 0x707078);
        ui_circle(c, x + 12 * S, y, 10 * S, colors[i]);
    }
}

static int palette_event(view_t *v, const struct ws_event *e) {
    if (e->type != WS_EV_DOWN) return 0;
    int i = e->x / (26 * S);
    if (i < 0 || i >= COLORS) return 0;
    color = i;
    eraser = 0;
    v_set_flags(eraser_btn, 0);
    v_dirty(v);
    return 1;
}

static void draw_brushes(view_t *v, ui_canvas *c) {
    ui_fill(c, v->x, v->y, v->w, v->h, ui_color(UI_C_TOOLBAR));
    int x = v->x, y = v->y + v->h / 2;
    for (int b = 0; b < 3; b++, x += 30 * S) {
        if (b == brush) ui_round(c, x, y - 13 * S, 26 * S, 26 * S, 6 * S, ui_color(UI_C_CONTROL_OFF));
        ui_circle(c, x + 13 * S, y, (b == 0 ? 2 : b == 1 ? 4 : 8) * S, ui_color(UI_C_LABEL));
    }
}

static int brushes_event(view_t *v, const struct ws_event *e) {
    if (e->type != WS_EV_DOWN) return 0;
    int b = e->x / (30 * S);
    if (b < 0 || b > 2) return 0;
    brush = b;
    v_dirty(v);
    return 1;
}

static void say(const char *s) { v_set_text(status, s); }

static void toggle_eraser(view_t *b) {
    eraser = !eraser;
    v_set_flags(b, eraser ? V_TOGGLED : 0);
    v_dirty(palette);
}

static void clear(view_t *b) {
    (void)b;
    ui_fill(&pic, 0, 0, pic.w, pic.h, UI_WHITE);
    v_dirty(canvas);
    say("");
}

static void put32(unsigned char *p, unsigned v) { p[0] = (unsigned char)v; p[1] = (unsigned char)(v >> 8); p[2] = (unsigned char)(v >> 16); p[3] = (unsigned char)(v >> 24); }

static void save(view_t *btn) {
    (void)btn;
    int w = pic.w, h = pic.h, row = (w * 3 + 3) & ~3;
    unsigned size = 54 + (unsigned)row * (unsigned)h;
    unsigned char *bmp = mmap(size, PROT_READ | PROT_WRITE, -1);
    if (!bmp) {
        say("Out of memory");
        return;
    }
    memset(bmp, 0, 54);
    bmp[0] = 'B';
    bmp[1] = 'M';
    put32(bmp + 2, size);
    put32(bmp + 10, 54);
    put32(bmp + 14, 40);
    put32(bmp + 18, (unsigned)w);
    put32(bmp + 22, (unsigned)h);
    bmp[26] = 1;
    bmp[28] = 24;
    for (int y = 0; y < h; y++) {                     /* bottom row first */
        const unsigned *src = pic.px + (long)(h - 1 - y) * pic.stride;
        unsigned char *dst = bmp + 54 + (long)y * row;
        for (int x = 0; x < w; x++) {
            dst[x * 3] = (unsigned char)src[x];
            dst[x * 3 + 1] = (unsigned char)(src[x] >> 8);
            dst[x * 3 + 2] = (unsigned char)(src[x] >> 16);
        }
    }
    int fd = open(path, OPEN_WRITE);
    long n = fd >= 0 ? write(fd, bmp, size) : fd;
    if (fd >= 0) close(fd);
    munmap(bmp);
    char s[160];
    snprintf(s, sizeof(s), n == (long)size ? "Saved %s" : "Cannot save %s", path);
    say(s);
}

/* ---- The canvas ---------------------------------------------------------------- */

static void draw_canvas(view_t *v, ui_canvas *c) {
    ui_fill(c, v->x, v->y, v->w, v->h, UI_WHITE);
    ui_copy(c, v->x, v->y, pic.px, pic.w < v->w ? pic.w : v->w, pic.h < v->h ? pic.h : v->h, pic.stride);
}

/* A dab on the picture, then the same square onto the window. */
static void dab(int x, int y) {
    int r = brush_radius();
    ui_circle(&pic, x, y, r, eraser ? UI_WHITE : colors[color]);
    int x0 = x - r - 1 < 0 ? 0 : x - r - 1, y0 = y - r - 1 < 0 ? 0 : y - r - 1;
    int x1 = x + r + 1 > pic.w ? pic.w : x + r + 1, y1 = y + r + 1 > pic.h ? pic.h : y + r + 1;
    if (x1 <= x0 || y1 <= y0) return;
    ui_copy(&win.c, canvas->x + x0, canvas->y + y0, pic.px + (long)y0 * pic.stride + x0,
            x1 - x0, y1 - y0, pic.stride);
}

/* A smooth stroke: dabs every few pixels from the last point to this one. */
static void stroke(int x, int y) {
    int dx = x - lx, dy = y - ly, d = (dx < 0 ? -dx : dx) + (dy < 0 ? -dy : dy);
    int steps = d / (brush_radius() / 2 + 1) + 1;
    for (int i = 1; i <= steps; i++) dab(lx + dx * i / steps, ly + dy * i / steps);
    lx = x;
    ly = y;
}

static int canvas_event(view_t *v, const struct ws_event *e) {
    (void)v;
    if (e->type == WS_EV_DOWN) {
        down = 1;
        lx = e->x;
        ly = e->y;
        dab(e->x, e->y);
    } else if (e->type == WS_EV_MOVE && down) {
        stroke(e->x, e->y);
    } else if (e->type == WS_EV_UP) {
        down = 0;
    } else {
        return 0;
    }
    return 1;
}

static int window_key(view_t *v, int k) {
    (void)v;
    if (k != 19) return 0;                            /* Ctrl+S */
    save(0);
    return 1;
}

int main(int argc, char **argv) {
    S = ui_scale();
    if (argc > 1) snprintf(path, sizeof(path), "%s", argv[1]);
    if (ui_open(&win, "Paint", 760 * S, 520 * S) < 0) {
        printf("paint: the desktop is not running (log in to the desktop, or type desktop)\n");
        return 1;
    }
    view_t *root = v_box(0, V_COLUMN);
    root->pad = root->gap = 0;
    root->on_key = window_key;
    view_t *bar = v_box(root, V_ROW);
    bar->pad = 10;
    bar->gap = 10;
    bar->bg = UI_C_TOOLBAR;
    bar->align = V_CENTER;
    palette = v_custom(bar, draw_palette, palette_event);
    palette->flex = 0;
    palette->want_w = COLORS * 26;
    palette->want_h = 28;
    brushes = v_custom(bar, draw_brushes, brushes_event);
    brushes->flex = 0;
    brushes->want_w = 90;
    brushes->want_h = 28;
    eraser_btn = v_button(bar, "Eraser", toggle_eraser);
    v_button(bar, "Clear", clear);
    v_button(bar, "Save", save);
    status = v_label(bar, "");
    status->color = UI_C_SECONDARY;
    status->flex = 1;
    canvas = v_custom(root, draw_canvas, canvas_event);

    v_layout(&win, root);                              /* the picture: the canvas's size */
    unsigned *px = mmap((size_t)canvas->w * canvas->h * 4, PROT_READ | PROT_WRITE, -1);
    if (!px) {
        printf("paint: out of memory\n");
        return 1;
    }
    ui_canvas_init(&pic, px, canvas->w, canvas->h, canvas->w);
    ui_fill(&pic, 0, 0, pic.w, pic.h, UI_WHITE);
    return v_run(&win, root, 0, 0);
}
