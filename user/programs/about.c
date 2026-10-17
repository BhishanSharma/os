// about - About this computer, in a window (and the desktop's own test app).
// Built from views (view.h).
#include <stdio.h>
#include <os.h>
#include <view.h>

static void ok(view_t *b) {
    (void)b;
    printf("about: OK clicked\n");
    v_quit(0);
}

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
    char version[48], memory[48];
    snprintf(version, sizeof(version), "Version %s", u.release);
    snprintf(memory, sizeof(memory), "%lu MiB of memory", m.ram >> 20);

    view_t *root = v_box(0, V_COLUMN);
    view_t *top = v_box(root, V_ROW);
    top->gap = 14;
    top->flex = 1;
    v_icon(top, "computer", 72);
    view_t *words = v_box(top, V_COLUMN);
    words->flex = 1;
    words->gap = 2;
    v_label(words, u.sysname);
    v_label(words, version)->color = UI_C_SECONDARY;
    v_label(words, memory)->color = UI_C_SECONDARY;
    view_t *bottom = v_box(root, V_ROW);
    v_spacer(bottom);
    view_t *button = v_button(bottom, "OK", ok);
    button->flags = UI_BTN_DEFAULT;
    button->want_w = 80;

    v_layout(&w, root);                                /* (the tests click it: where it is) */
    printf("about: OK button at %d,%d\n", w.x + button->x + button->w / 2, w.y + button->y + button->h / 2);
    return v_run(&w, root, 0, 0);
}
