// about - About this computer, in a window (and the desktop's own test app)
#include <stdio.h>
#include <os.h>
#include <ui.h>

int main(void) {
    int S = ui_scale();
    ui_window w;
    if (ui_open(&w, "About", 340 * S, 170 * S) < 0) {
        printf("about: the desktop is not running (type windowserver)\n");
        return 1;
    }
    struct os_uname u;
    struct os_meminfo m;
    uname(&u);
    meminfo(&m);
    char line[96];
    int bx = 340 * S - 96 * S, by = 170 * S - 44 * S, bw = 80 * S, bh = 30 * S, pressed = 0;
    printf("about: OK button at %d,%d\n", w.x + bx + bw / 2, w.y + by + bh / 2);
    for (;;) {
        ui_fill(&w.c, 0, 0, w.c.w, w.c.h, UI_WINDOW);
        ui_circle(&w.c, 50 * S, 54 * S, 30 * S, UI_ACCENT);
        ui_text(&w.c, 50 * S - 8 * S, 54 * S - 16 * S, "i", UI_WHITE, 2 * S);
        ui_text(&w.c, 100 * S, 26 * S, u.sysname, UI_TEXT, S);
        snprintf(line, sizeof(line), "Version %s", u.release);
        ui_text(&w.c, 100 * S, 48 * S, line, UI_DIM, S);
        snprintf(line, sizeof(line), "%lu MiB of memory", m.ram >> 20);
        ui_text(&w.c, 100 * S, 70 * S, line, UI_DIM, S);
        ui_button(&w.c, bx, by, bw, bh, "OK", S, pressed);
        ui_present(&w);
        struct ws_event e;
        ui_wait(&w, &e, 0);
        if (e.type == WS_EV_CLOSE) return 0;
        if (e.type == WS_EV_DOWN && ui_inside(e.x, e.y, bx, by, bw, bh)) pressed = 1;
        if (e.type == WS_EV_UP && pressed) {
            if (ui_inside(e.x, e.y, bx, by, bw, bh)) {
                printf("about: OK clicked\n");
                return 0;
            }
            pressed = 0;
        }
        if (e.type == WS_EV_KEY && (e.key == '\n' || e.key == '\r')) return 0;
    }
}
