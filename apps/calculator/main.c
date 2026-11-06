// calculator - Calculator: + - x / with percent, sign and memory of the last
// operation (= again repeats it). The keyboard works too: digits, . + - * /,
// Return or = for the result, Backspace, Esc to clear; Ctrl+C copies the
// result, Ctrl+V pastes a number.
//
// Built from views (view.h): the display (a large label) and a grid of buttons.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <os.h>
#include <view.h>

static view_t *display, *history;
static char entry[32] = "0";                   /* what is being typed (or the result shown) */
static double acc;                             /* the value before the operator */
static char op;                                /* + - * / or 0 */
static char last_op;                           /* = again: the same operation */
static double last_arg;
static int typing, error;                      /* typing: entry is being typed */

static void format(double v, char *out, int n) {
    if (v != v || v > 1e15 || v < -1e15) {     /* NaN, or too big to show */
        snprintf(out, (size_t)n, "Error");
        error = 1;
        return;
    }
    snprintf(out, (size_t)n, "%.10g", v);
    if (!strcmp(out, "-0")) snprintf(out, (size_t)n, "0");
}

static double value(void) { return strtod(entry, 0); }

static void show(void) {
    v_set_text(display, entry);
    char h[48] = "";
    if (op) {
        char a[24];
        format(acc, a, sizeof(a));
        snprintf(h, sizeof(h), "%s %c", a, op == '*' ? 'x' : op);
    }
    v_set_text(history, h);
}

static double apply(double a, char o, double b) {
    switch (o) {
    case '+': return a + b;
    case '-': return a - b;
    case '*': return a * b;
    case '/': return b == 0 ? __builtin_nan("") : a / b;
    }
    return b;
}

static void clear_all(void) {
    snprintf(entry, sizeof(entry), "0");
    acc = 0;
    op = last_op = 0;
    typing = error = 0;
}

static void digit(char d) {
    if (error) clear_all();
    if (!typing) {
        snprintf(entry, sizeof(entry), d == '.' ? "0." : "%c", d);
        typing = 1;
        return;
    }
    if (d == '.' && strchr(entry, '.')) return;
    size_t n = strlen(entry);
    if (n >= 16) return;
    if (!strcmp(entry, "0") && d != '.') n = 0;
    entry[n] = d;
    entry[n + 1] = 0;
}

static void operation(char o) {
    if (error) return;
    if (op && typing) {                        /* 2 + 3 x: the 2 + 3 first */
        format(apply(acc, op, value()), entry, sizeof(entry));
        if (error) return;
    }
    acc = value();
    op = o;
    typing = 0;
}

static void equals(void) {
    if (error) return;
    if (op) {
        last_op = op;
        last_arg = value();
        format(apply(acc, op, last_arg), entry, sizeof(entry));
        op = 0;
    } else if (last_op) {                      /* = again */
        format(apply(value(), last_op, last_arg), entry, sizeof(entry));
    }
    typing = 0;
}

static void backspace(void) {
    if (!typing || error) return;
    size_t n = strlen(entry);
    if (n <= 1 || (n == 2 && entry[0] == '-')) snprintf(entry, sizeof(entry), "0");
    else entry[n - 1] = 0;
}

static void press(const char *k) {
    if (!strcmp(k, "C")) clear_all();
    else if (!strcmp(k, "+/-")) {
        if (error) return;
        if (entry[0] == '-') memmove(entry, entry + 1, strlen(entry));
        else if (strcmp(entry, "0") && strlen(entry) < sizeof(entry) - 1) {
            memmove(entry + 1, entry, strlen(entry) + 1);
            entry[0] = '-';
        }
    } else if (!strcmp(k, "%")) {
        if (error) return;
        double v = value();
        format(op == '+' || op == '-' ? acc * v / 100 : v / 100, entry, sizeof(entry));
        typing = 1;
    } else if (!strcmp(k, "=")) equals();
    else if (!strcmp(k, "/") || !strcmp(k, "x") || !strcmp(k, "-") || !strcmp(k, "+"))
        operation(k[0] == 'x' ? '*' : k[0]);
    else digit(k[0]);
    show();
}

static void clicked(view_t *b) { press(b->text); }

static void copy_result(void) { ui_clip_set(entry, (int)strlen(entry)); }

static void paste_number(void) {
    char clip[64];
    ui_clip_get(clip, sizeof(clip));
    char *end;
    double v = strtod(clip, &end);
    if (end == clip) return;
    format(v, entry, sizeof(entry));
    typing = 1;
    show();
}

static int on_key(view_t *v, int k) {
    (void)v;
    char s[2] = { (char)k, 0 };
    if ((k >= '0' && k <= '9') || k == '.') press(s);
    else if (k == '+' || k == '-' || k == '/') press(s);
    else if (k == '*' || k == 'x') press("x");
    else if (k == '=' || k == '\r' || k == '\n') press("=");
    else if (k == '%') press("%");
    else if (k == 27 || k == 'c' || k == 'C') press("C");
    else if (k == '\b') { backspace(); show(); }
    else if (k == 3) copy_result();
    else if (k == 22) paste_number();
    else return 0;
    return 1;
}

int main(void) {
    int S = ui_scale();
    ui_window win;
    if (ui_open(&win, "Calculator", 260 * S, 360 * S) < 0) {
        printf("calculator: the desktop is not running\n");
        return 1;
    }
    view_t *root = v_box(0, V_COLUMN);
    root->pad = 12;
    root->gap = 8;
    root->on_key = on_key;
    history = v_label(root, "");
    history->color = UI_C_SECONDARY;
    history->align = V_END;
    display = v_label(root, "0");
    display->style = UI_BOLD;
    display->scale = 2;
    display->align = V_END;
    static const char *keys[5][4] = {
        { "C", "+/-", "%", "/" },
        { "7", "8", "9", "x" },
        { "4", "5", "6", "-" },
        { "1", "2", "3", "+" },
        { "0", ".", "=", 0 },
    };
    for (int r = 0; r < 5; r++) {
        view_t *row = v_box(root, V_ROW);
        row->flex = 1;
        row->gap = 8;
        row->pad = 0;
        row->align = V_STRETCH;
        for (int c = 0; c < 4 && keys[r][c]; c++) {
            view_t *b = v_button(row, keys[r][c], clicked);
            b->flex = r == 4 && c == 0 ? 2 : 1;     /* a wide 0 */
            if (keys[r][c][0] == '=' || (c == 3 && r < 4)) v_set_flags(b, UI_BTN_DEFAULT);
        }
    }
    show();
    return v_run(&win, root, 0, 0);
}
