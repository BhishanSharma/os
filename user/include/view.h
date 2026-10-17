// view.h - views: a window described as a tree of components
//
// Instead of drawing the whole window after every change, a program builds a
// tree once (boxes holding labels, buttons, fields, tables...) and then only
// changes state: v_set_text(status, "Saved"). A view whose state really
// changed is marked dirty; each pass draws the dirty views and nothing else,
// and the window server is told just their rectangles (ui_present). Like a
// web page's DOM updated by React: state in, minimal redraw out.
//
//   static view_t *count;
//   static void more(view_t *b) { static int n; char t[16]; snprintf(t, 16, "%d", ++n); v_set_text(count, t); }
//   int main(void) {
//       ui_window w;
//       if (ui_open(&w, "Counter", 300 * ui_scale(), 120 * ui_scale()) < 0) return 1;
//       view_t *root = v_box(0, V_COLUMN);
//       count = v_label(root, "0");
//       v_button(root, "More", more);
//       return v_run(&w, root, 0, 0);
//   }
//
// Sizes are in points (the screen's scale is applied). Layout is a small
// flexbox: a box lays its children out in a column or a row; `flex` children
// share the space left over; the others keep their natural size. Anything
// the components cannot do, a custom view draws itself (v_custom): Paint's
// canvas, Terminal's text.
#ifndef VIEW_H
#define VIEW_H

#include <ui.h>

enum { V_BOX, V_LABEL, V_TEXT, V_BUTTON, V_ICON, V_TABLE, V_FIELD, V_SLIDER, V_CUSTOM };
enum { V_COLUMN, V_ROW };                       /* a box's direction */
enum { V_START, V_CENTER, V_END, V_STRETCH };   /* where children sit across it */

/* flags, besides UI_BTN_DEFAULT / UI_BTN_DISABLED for buttons */
#define V_SECRET   0x100                        /* a field shows dots */
#define V_INSET    0x200                        /* a table: no stripes, a rounded selection (a sidebar) */
#define V_TOGGLED  0x400                        /* a button shown held down (a tool that is on) */

#define V_MAX_COLS 8
#define V_MAX_ROWS 512

typedef struct view view_t;
/* A table's data: the text of one cell. */
typedef void (*v_cell_fn)(view_t *table, int row, int col, char *out, int max);

struct view {
    int kind;
    view_t *parent, *first, *last, *next;      /* the tree */
    int x, y, w, h;                            /* where it is (pixels, after layout) */
    int want_w, want_h;                        /* a fixed size in points (0: natural) */
    int flex;                                  /* > 0: a share of the space left in its box */
    int pad, gap, dir, align;                  /* box: padding and spacing (points), V_*, V_START... */
    int bg;                                    /* a UI_C_* colour role, or -1: its parent's background */
    int color, style, scale;                   /* text: a UI_C_* role, UI_REGULAR / UI_BOLD, size (1, 2) */
    char text[128];
    char hint[48];                             /* a field's placeholder */
    int flags, hidden, value;                  /* value: a slider's 0-100 */
    void *data;                                /* the program's */
    /* what it does */
    void (*on_click)(view_t *v);               /* buttons */
    void (*on_change)(view_t *v);              /* fields (each key), sliders */
    void (*on_enter)(view_t *v);               /* fields: Return */
    int (*on_key)(view_t *v, int key);         /* keys nothing inside took (any view) -> 1 if used */
    void (*draw)(view_t *v, ui_canvas *c);     /* custom: draw yourself into [x, y, w, h] */
    int (*event)(view_t *v, const struct ws_event *e);   /* custom: x, y relative to the view -> 1 if used */
    int focusable;                             /* custom: takes the keys when clicked */
    /* tables */
    int rows, row_h, first_row, selected, cols;
    int col_w[V_MAX_COLS];                     /* points; the last column takes the rest */
    const char *heads[V_MAX_COLS];             /* 0: no header row */
    v_cell_fn cell;
    const char *(*row_icon)(view_t *v, int row);   /* an icon before the first column */
    int (*row_header)(view_t *v, int row);         /* 1: a section heading (not selectable) */
    void (*on_select)(view_t *v, int row);
    void (*on_open)(view_t *v, int row);           /* double-click or Return */
    unsigned row_hash[V_MAX_ROWS];             /* what each visible row showed (to redraw only changes) */
    int recheck;                               /* compare the rows on the next pass */
    /* the toolkit's */
    int dirty, hot, pressed;
};

/* Building the tree (parent 0: a root). Each returns the new view. */
view_t *v_box(view_t *parent, int dir);
view_t *v_label(view_t *parent, const char *text);
view_t *v_text(view_t *parent, const char *text);                  /* wrapped over lines */
view_t *v_button(view_t *parent, const char *label, void (*on_click)(view_t *v));
view_t *v_icon(view_t *parent, const char *name, int size);        /* a named icon, size in points */
view_t *v_spacer(view_t *parent);                                  /* takes the space left */
view_t *v_table(view_t *parent, int cols, const char *const *heads, const int *widths, v_cell_fn cell);
view_t *v_field(view_t *parent, const char *text, void (*on_enter)(view_t *v));
view_t *v_slider(view_t *parent, int value, void (*on_change)(view_t *v));
view_t *v_custom(view_t *parent, void (*draw)(view_t *v, ui_canvas *c),
                 int (*event)(view_t *v, const struct ws_event *e));
void v_clear(view_t *v);                        /* remove (and free) what is inside it */

/* State. Each redraws its view only if the value really changes. */
void v_set_text(view_t *v, const char *text);
void v_set_flags(view_t *v, int flags);
void v_set_color(view_t *v, int role);
void v_set_value(view_t *v, int value);
void v_show(view_t *v, int shown);              /* hidden views take no space */
void v_focus(view_t *v);                        /* it gets the keys */
void v_table_rows(view_t *t, int rows);         /* the data may have changed: compare, redraw what did */
void v_table_select(view_t *t, int row);        /* -1: none; scrolls it into view */
void v_dirty(view_t *v);                        /* redraw it (and what is inside it) */

/* The loop: lays out, draws what changed, hands events to the views. Calls
 * `tick` every `tick_ms` (0: never). Returns when the window is closed or
 * v_quit is called; -> the exit code. */
int v_run(ui_window *w, view_t *root, int tick_ms, void (*tick)(void));
void v_quit(int code);
void v_layout(ui_window *w, view_t *root);      /* now (to know where views are before v_run) */
void v_update(void);                            /* draw what changed now (before something slow) */

/* A small window of its own: an icon, a bold line, the reason, OK. Waits until
 * it is closed (on the desktop; else it prints). E.g. a file that cannot be opened. */
void v_alert(const char *icon, const char *title, const char *message);

#endif
