// paint [file.bmp] - Paint: draw with the mouse or touchpad. Twelve colours,
// three brush sizes, an eraser; Save writes a BMP picture (open it in Files).
#include <stdio.h>
#include <string.h>
#include <os.h>
#include <ui.h>

static const unsigned colors[] = { 0x1E1E1E, 0x8A8A8E, 0xFF3B30, 0xFF9500, 0xFFCC00, 0x34C759,
                                   0x00C7BE, 0x30B0FF, 0x007AFF, 0x5856D6, 0xAF52DE, 0xFF2D55 };
#define COLORS 12

static ui_window win;
static int S, BAR, color = 8, brush = 1, eraser, down, lx, ly;
static char path[128] = "drawing.bmp", status[64];

static int brush_radius(void) { return (brush == 0 ? 2 : brush == 1 ? 5 : 12) * S; }

static void toolbar(void) {
    ui_canvas *c = &win.c;
    ui_clip(c, 0, 0, c->w, BAR);
    ui_fill(c, 0, 0, c->w, BAR, 0xF2F2F6);
    ui_fill(c, 0, BAR - 1, c->w, 1, 0xDADAE0);
    int x = 12 * S, y = BAR / 2;
    for (int i = 0; i < COLORS; i++, x += 26 * S) {
        if (i == color && !eraser) ui_circle(c, x + 10 * S, y, 12 * S, 0x707078);
        ui_circle(c, x + 10 * S, y, 10 * S, colors[i]);
    }
    x += 10 * S;
    for (int b = 0; b < 3; b++, x += 30 * S) {
        if (b == brush) ui_round(c, x, y - 13 * S, 26 * S, 26 * S, 6 * S, 0xD8D8E0);
        ui_circle(c, x + 13 * S, y, (b == 0 ? 2 : b == 1 ? 4 : 8) * S, 0x404048);
    }
    x += 8 * S;
    ui_button(c, x, y - 14 * S, 70 * S, 28 * S, "Eraser", S, eraser);
    ui_button(c, x + 78 * S, y - 14 * S, 60 * S, 28 * S, "Clear", S, 0);
    ui_button(c, x + 146 * S, y - 14 * S, 60 * S, 28 * S, "Save", S, 0);
    if (status[0]) ui_text(c, x + 216 * S, y - 8 * S, status, UI_DIM, S);
    ui_noclip(c);
}

static void clear(void) {
    ui_fill(&win.c, 0, BAR, win.c.w, win.c.h - BAR, UI_WHITE);
}

static void dab(int x, int y) {
    ui_clip(&win.c, 0, BAR, win.c.w, win.c.h - BAR);
    ui_circle(&win.c, x, y, brush_radius(), eraser ? UI_WHITE : colors[color]);
    ui_noclip(&win.c);
}

/* A smooth stroke: dabs every few pixels from the last point to this one. */
static void stroke(int x, int y) {
    int dx = x - lx, dy = y - ly, d = (dx < 0 ? -dx : dx) + (dy < 0 ? -dy : dy);
    int steps = d / (brush_radius() / 2 + 1) + 1;
    for (int i = 1; i <= steps; i++) dab(lx + dx * i / steps, ly + dy * i / steps);
    lx = x;
    ly = y;
}

static void put32(unsigned char *p, unsigned v) { p[0] = (unsigned char)v; p[1] = (unsigned char)(v >> 8); p[2] = (unsigned char)(v >> 16); p[3] = (unsigned char)(v >> 24); }

static void save(void) {
    int w = win.c.w, h = win.c.h - BAR, row = (w * 3 + 3) & ~3;
    unsigned size = 54 + (unsigned)row * (unsigned)h;
    unsigned char *bmp = mmap(size, PROT_READ | PROT_WRITE, -1);
    if (!bmp) {
        snprintf(status, sizeof(status), "Out of memory");
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
        const unsigned *src = win.c.px + (long)(BAR + h - 1 - y) * win.c.stride;
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
    snprintf(status, sizeof(status), n == (long)size ? "Saved %s" : "Cannot save", path);
}

static void click_bar(int ex, int ey) {
    int x = 12 * S, y = BAR / 2;
    for (int i = 0; i < COLORS; i++, x += 26 * S)
        if (ui_inside(ex, ey, x - 2 * S, y - 12 * S, 24 * S, 24 * S)) {
            color = i;
            eraser = 0;
            return;
        }
    x += 10 * S;
    for (int b = 0; b < 3; b++, x += 30 * S)
        if (ui_inside(ex, ey, x, y - 13 * S, 26 * S, 26 * S)) {
            brush = b;
            return;
        }
    x += 8 * S;
    if (ui_inside(ex, ey, x, y - 14 * S, 70 * S, 28 * S)) eraser = !eraser;
    else if (ui_inside(ex, ey, x + 78 * S, y - 14 * S, 60 * S, 28 * S)) {
        clear();
        status[0] = 0;
    } else if (ui_inside(ex, ey, x + 146 * S, y - 14 * S, 60 * S, 28 * S)) save();
}

int main(int argc, char **argv) {
    S = ui_scale();
    BAR = 48 * S;
    if (argc > 1) snprintf(path, sizeof(path), "%s", argv[1]);
    if (ui_open(&win, "Paint", 760 * S, 520 * S) < 0) {
        printf("paint: the desktop is not running (log in to the desktop, or type desktop)\n");
        return 1;
    }
    clear();
    toolbar();
    ui_present(&win);
    for (;;) {
        struct ws_event e;
        ui_wait(&win, &e, 0);
        if (e.type == WS_EV_CLOSE) return 0;
        if (e.type == WS_EV_DOWN) {
            if (e.y < BAR) click_bar(e.x, e.y);
            else {
                down = 1;
                lx = e.x;
                ly = e.y;
                dab(e.x, e.y);
            }
        } else if (e.type == WS_EV_MOVE && down) {
            stroke(e.x, e.y);
        } else if (e.type == WS_EV_UP) {
            down = 0;
        } else if (e.type == WS_EV_KEY && e.key == 19) {   /* Ctrl+S */
            save();
        } else {
            continue;
        }
        toolbar();
        ui_present(&win);
    }
}
