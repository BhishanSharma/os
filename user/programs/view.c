// view <image> - show a PNG, JPEG, GIF, BMP, TGA, PSD or PPM picture on the screen.
// The picture is scaled to fit (small ones are enlarged by a whole number,
// up to 8x). Any key, or a click, closes it.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <image.h>
#include <os.h>
#include <view.h>

/* Shrink `img` into `out` (tw x th) by averaging the pixels each target
 * pixel covers (a box filter: no jagged edges, no lost detail). */
static void shrink(const image_t *img, unsigned int *out, int tw, int th) {
    for (int y = 0; y < th; y++) {
        int y0 = (int)((long)y * img->height / th), y1 = (int)((long)(y + 1) * img->height / th);
        if (y1 <= y0) y1 = y0 + 1;
        for (int x = 0; x < tw; x++) {
            int x0 = (int)((long)x * img->width / tw), x1 = (int)((long)(x + 1) * img->width / tw);
            if (x1 <= x0) x1 = x0 + 1;
            unsigned long r = 0, g = 0, b = 0, n = 0;
            for (int sy = y0; sy < y1; sy++) {
                const unsigned int *row = img->pixels + (long)sy * img->width;
                for (int sx = x0; sx < x1; sx++) {
                    unsigned int c = row[sx];
                    r += c >> 16 & 0xFF;
                    g += c >> 8 & 0xFF;
                    b += c & 0xFF;
                    n++;
                }
            }
            out[(long)y * tw + x] = (unsigned int)(r / n << 16 | g / n << 8 | b / n);
        }
    }
}

/* Enlarge by a whole number `k` (each pixel becomes a k x k block). */
static void enlarge(const image_t *img, unsigned int *out, int k) {
    int tw = img->width * k;
    for (int y = 0; y < img->height * k; y++)
        for (int x = 0; x < tw; x++)
            out[(long)y * tw + x] = img->pixels[(long)(y / k) * img->width + x / k];
}

static const unsigned int *picture;
static int pic_w, pic_h;

static void draw_picture(view_t *v, ui_canvas *c) {
    ui_fill(c, v->x, v->y, v->w, v->h, 0);
    ui_copy(c, v->x + (v->w - pic_w) / 2, v->y + (v->h - pic_h) / 2, picture, pic_w, pic_h, pic_w);
}

static int closing_key(view_t *v, int k) {             /* Esc or q closes it */
    (void)v;
    if (k != 27 && k != 'q') return 0;
    v_quit(0);
    return 1;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        printf("Usage: view <image>   (PNG, JPEG, GIF, BMP, TGA, PSD or PPM)\n");
        return 2;
    }
    struct os_gfx_info screen;
    if (gfx_info(&screen) != 0) {
        printf("view: the screen is in VGA text mode: pictures need the graphical console\n");
        return 1;
    }

    image_t img;
    const char *error;
    if (image_load(argv[1], &img, &error) != 0) {
        char title[96];
        snprintf(title, sizeof(title), "Can't open %s", strrchr(argv[1], '/') ? strrchr(argv[1], '/') + 1 : argv[1]);
        v_alert("picture", title, error);
        return 1;
    }

    // The picture area: below the status bar.
    int area_w = screen.width, area_h = screen.height - screen.top;
    int windowed = ui_desktop();                       /* on the desktop: in a window */
    if (windowed) {
        area_w = area_w * 3 / 4;
        area_h = area_h * 3 / 4;
    }
    int tw = img.width, th = img.height, zoom_num = 100;
    unsigned int *shown = img.pixels;
    unsigned int *scaled = 0;
    if (tw > area_w || th > area_h) {
        // Too big: fit the longer side, keep the shape.
        if ((long)tw * area_h > (long)th * area_w) {
            th = (int)((long)th * area_w / tw);
            tw = area_w;
        } else {
            tw = (int)((long)tw * area_h / th);
            th = area_h;
        }
        if (tw < 1) tw = 1;
        if (th < 1) th = 1;
        scaled = malloc((size_t)tw * (size_t)th * 4);
        if (!scaled) {
            printf("view: out of memory\n");
            image_free(&img);
            return 1;
        }
        shrink(&img, scaled, tw, th);
        shown = scaled;
        zoom_num = (int)((long)tw * 100 / img.width);
    } else {
        int k = area_w / tw < area_h / th ? area_w / tw : area_h / th;
        if (k > 8) k = 8;
        if (k > 1) {
            scaled = malloc((size_t)tw * k * (size_t)th * k * 4);
            if (scaled) {
                enlarge(&img, scaled, k);
                tw *= k;
                th *= k;
                shown = scaled;
                zoom_num = k * 100;
            }
        }
    }

    if (windowed) {                                    /* views (view.h): the picture is a custom view */
        ui_window w;
        const char *name = strrchr(argv[1], '/') ? strrchr(argv[1], '/') + 1 : argv[1];
        if (ui_open(&w, name, tw, th) == 0) {
            picture = shown;
            pic_w = tw;
            pic_h = th;
            view_t *root = v_box(0, V_COLUMN);
            root->pad = 0;
            root->on_key = closing_key;
            v_custom(root, draw_picture, 0);
            int r = v_run(&w, root, 0, 0);
            free(scaled);
            image_free(&img);
            return r;
        }
    }
    if (gfx_begin() != 0) {
        printf("view: cannot draw (is it running in the background?)\n");
        free(scaled);
        image_free(&img);
        return 1;
    }
    gfx_blit((area_w - tw) / 2, screen.top + (area_h - th) / 2, tw, th, shown, tw);

    // Close on a key or a click.
    struct os_mouse m;
    int had_mouse = getmouse(&m), was_down = had_mouse ? m.buttons : 0;
    while (1) {
        if (getkey()) break;
        if (had_mouse) {
            getmouse(&m);
            if (m.buttons && !was_down) break;
            was_down = m.buttons;
        }
        sleep_ms(20);
    }
    gfx_end();
    printf("%s: %dx%d %s, shown at %d%%\n", argv[1], img.width, img.height, img.format, zoom_num);
    free(scaled);
    image_free(&img);
    return 0;
}
