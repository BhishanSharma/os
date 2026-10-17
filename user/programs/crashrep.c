// crashrep NAME REASON - "NAME quit unexpectedly" (the window server starts it
// when a program it opened crashes). Show Log opens Console; the kernel's
// report (which instruction, which address) is in the log, and on the boot
// stick in /USB/LOGS. Built from views (view.h): no drawing code of its own.
#include <stdio.h>
#include <string.h>
#include <os.h>
#include <view.h>

static void show_log(view_t *b) {
    (void)b;
    char *args[] = { "console", 0 };
    spawn("console", args, 0);
    v_quit(0);
}

static void ok(view_t *b) {
    (void)b;
    v_quit(0);
}

int main(int argc, char **argv) {
    const char *name = argc > 1 ? argv[1] : "A program";
    const char *why = argc > 2 ? argv[2] : "it crashed";
    printf("crashrep: %s quit unexpectedly: %s\n", name, why);
    int S = ui_scale();
    ui_window w;
    if (ui_open(&w, "Problem Report", 440 * S, 170 * S) < 0) return 1;
    char title[96], line[160];
    snprintf(title, sizeof(title), "%s quit unexpectedly.", name);
    snprintf(line, sizeof(line), "Because %s. The report is in the log.", why);

    view_t *root = v_box(0, V_ROW);                    /* the icon, then the words and buttons */
    root->gap = 16;
    v_icon(root, "info", 56);
    view_t *words = v_box(root, V_COLUMN);
    words->flex = 1;
    v_label(words, title)->style = UI_BOLD;
    v_text(words, line)->color = UI_C_SECONDARY;
    v_spacer(words);
    view_t *buttons = v_box(words, V_ROW);
    v_spacer(buttons);
    v_button(buttons, "Show Log", show_log);
    v_button(buttons, "OK", ok)->flags = UI_BTN_DEFAULT;   /* Return */
    return v_run(&w, root, 0, 0);
}
