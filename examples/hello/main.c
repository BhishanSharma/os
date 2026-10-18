// Hello: the smallest app worth installing. A window with a label and a
// button, built from views (view.h), like the built-in apps.
#include <stdio.h>
#include <view.h>

static view_t *count;

static void more(view_t *button) {
    (void)button;
    static int n;
    char text[32];
    n++;
    snprintf(text, sizeof(text), "Clicked %d time%s", n, n == 1 ? "" : "s");
    v_set_text(count, text);
}

int main(void) {
    int S = ui_scale();
    ui_window w;
    if (ui_open(&w, "Hello", 360 * S, 160 * S) < 0) return 1;
    view_t *root = v_box(0, V_COLUMN);
    root->gap = 12;
    v_label(root, "Hello from an installed app!")->style = UI_BOLD;
    count = v_label(root, "Not clicked yet");
    v_spacer(root);
    v_button(root, "Click me", more)->flags = UI_BTN_DEFAULT;
    return v_run(&w, root, 0, 0);
}
