// paint - draw on the screen with the mouse.
// Left button paints, right button erases, the wheel or keys 1-7 pick the
// colour, C clears, Q (or Ctrl+C) quits.
#include <stdio.h>
#include <os.h>

static const int colors[] = { COLOR_LIGHT_RED, COLOR_YELLOW, COLOR_LIGHT_GREEN, COLOR_LIGHT_CYAN,
                              COLOR_LIGHT_BLUE, COLOR_PINK, COLOR_WHITE };
#define NCOLORS 7

static int cols, rows, current;

static void help_line(void) {
    static const char text[] = " paint  left: draw  right: erase  wheel/1-7: colour  C: clear  Q: quit  ";
    int n = 0;
    gotoxy(0, rows - 1);
    set_color(COLOR_BLACK, COLOR_LIGHT_GRAY);
    for (; text[n] && n < cols - 6; n++) putchar(text[n]);
    set_color(colors[current], colors[current]);
    printf("    ");
    set_color(COLOR_BLACK, COLOR_LIGHT_GRAY);
    for (n += 4; n < cols - 1; n++) putchar(' ');     // not the last cell: no scrolling
    reset_color();
}

static void plot(int c, int r, int erase) {
    if (r < 0 || r >= rows - 1 || c < 0 || c >= cols) return;
    gotoxy(c, r);
    if (erase) reset_color();
    else set_color(colors[current], colors[current]);
    putchar(' ');
}

/* Every cell from (c0, r0) to (c1, r1), so quick strokes have no gaps. */
static void line(int c0, int r0, int c1, int r1, int erase) {
    int dc = c1 > c0 ? c1 - c0 : c0 - c1, dr = r1 > r0 ? r1 - r0 : r0 - r1;
    int sc = c0 < c1 ? 1 : -1, sr = r0 < r1 ? 1 : -1, err = dc - dr;
    while (1) {
        plot(c0, r0, erase);
        if (c0 == c1 && r0 == r1) break;
        int e2 = 2 * err;
        if (e2 > -dr) { err -= dr; c0 += sc; }
        if (e2 < dc) { err += dc; r0 += sr; }
    }
}

static void clear_canvas(void) {
    reset_color();
    clear_screen();
    help_line();
}

int main(void) {
    struct os_mouse m;
    if (!getmouse(&m)) {
        printf("paint: no mouse found (PS/2 mouse needed)\n");
        return 1;
    }
    console_size(&cols, &rows);
    show_cursor(0);
    clear_canvas();

    int last_col = -1, last_row = -1, last_buttons = 0;
    while (1) {
        int key = getkey();
        if (key == 'q' || key == 'Q') break;
        if (key == 'c' || key == 'C') clear_canvas();
        if (key >= '1' && key < '1' + NCOLORS) {
            current = key - '1';
            help_line();
        }

        getmouse(&m);
        if (m.wheel) {
            current = ((current + (m.wheel > 0 ? 1 : -1)) % NCOLORS + NCOLORS) % NCOLORS;
            help_line();
        }
        int drawing = m.buttons & (MOUSE_BUTTON_LEFT | MOUSE_BUTTON_RIGHT);
        if (drawing && (m.col != last_col || m.row != last_row || m.buttons != last_buttons)) {
            int erase = !(m.buttons & MOUSE_BUTTON_LEFT);
            if (last_buttons & (MOUSE_BUTTON_LEFT | MOUSE_BUTTON_RIGHT))
                line(last_col, last_row, m.col, m.row, erase);
            else
                plot(m.col, m.row, erase);
        }
        last_col = m.col;
        last_row = m.row;
        last_buttons = m.buttons;
        sleep_ms(10);
    }
    reset_color();
    clear_screen();
    show_cursor(1);
    return 0;
}
