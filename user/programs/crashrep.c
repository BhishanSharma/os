// crashrep NAME REASON - "NAME quit unexpectedly" (the window server starts it
// when a program it opened crashes). Show Log opens Console; the kernel's
// report (which instruction, which address) is in the log, and on the boot
// stick in /USB/LOGS.
#include <stdio.h>
#include <string.h>
#include <os.h>
#include <ui.h>

int main(int argc, char **argv) {
    const char *name = argc > 1 ? argv[1] : "A program";
    const char *why = argc > 2 ? argv[2] : "it crashed";
    printf("crashrep: %s quit unexpectedly: %s\n", name, why);
    int S = ui_scale(), W = 440 * S, H = 170 * S;
    ui_window w;
    if (ui_open(&w, "Problem Report", W, H) < 0) return 1;
    char title[96], line[160];
    snprintf(title, sizeof(title), "%s quit unexpectedly.", name);
    snprintf(line, sizeof(line), "Because %s. The report is in the log.", why);
    int bw = 110 * S, bh = 30 * S, by = H - 46 * S, ok_x = W - bw - 16 * S, log_x = ok_x - bw - 10 * S;
    for (;;) {
        ui_fill(&w.c, 0, 0, w.c.w, w.c.h, UI_WINDOW);
        if (ui_icon(&w.c, 18 * S, 20 * S, 56 * S, "info", 255) != 0)
            ui_circle(&w.c, 46 * S, 48 * S, 26 * S, 0xFF9500);
        ui_text_bold(&w.c, 92 * S, 24 * S, title, UI_TEXT, S);
        ui_text_block(&w.c, 92 * S, 50 * S, W - 110 * S, line, UI_DIM, S, UI_REGULAR);
        ui_button(&w.c, log_x, by, bw, bh, "Show Log", S, 0);
        ui_button_ex(&w.c, ok_x, by, bw, bh, "OK", S, UI_BTN_DEFAULT);
        ui_present(&w);
        struct ws_event e;
        ui_wait(&w, &e, 0);
        if (e.type == WS_EV_CLOSE) return 0;
        if (e.type == WS_EV_KEY && (e.key == '\n' || e.key == '\r' || e.key == 27)) return 0;
        if (e.type == WS_EV_DOWN && ui_inside(e.x, e.y, ok_x, by, bw, bh)) return 0;
        if (e.type == WS_EV_DOWN && ui_inside(e.x, e.y, log_x, by, bw, bh)) {
            char *args[] = { "console", 0 };
            spawn("console", args, 0);
            return 0;
        }
    }
}
