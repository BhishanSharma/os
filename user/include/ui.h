// ui.h - drawing and windows for desktop programs (a small AppKit)
//
//   ui_window w;
//   if (ui_open(&w, "Hello", 320, 200) < 0) return 1;     // no desktop running
//   ui_fill(&w.c, 0, 0, 320, 200, UI_WINDOW);
//   ui_text(&w.c, 10, 10, "Hello", UI_TEXT, w.scale);
//   ui_present(&w);
//   for (;;) { struct ws_event e; ui_wait(&w, &e, 0); if (e.type == WS_EV_CLOSE) return 0; }
#ifndef UI_H
#define UI_H

#include <ws.h>

/* Colours, 0x00RRGGBB. Semantic tokens (the macOS way): a token says what a colour
 * is FOR, and resolves for the current appearance (light, dark, Increase Contrast).
 * Ask when you draw and never keep the value: the look can change while the program
 * runs (WS_EV_PREFS), so redraw on that event. */
enum {
    UI_C_LABEL,          /* primary text */
    UI_C_SECONDARY,      /* secondary text, column headers */
    UI_C_TERTIARY,       /* placeholders, disabled text */
    UI_C_WINDOW,         /* window background */
    UI_C_TOOLBAR,        /* toolbar: the same as the title bar above it */
    UI_C_CONTENT,        /* lists, text areas, documents */
    UI_C_SIDEBAR,        /* sidebars */
    UI_C_SEPARATOR,      /* hairlines between areas */
    UI_C_ACCENT,         /* selection, default buttons, focus */
    UI_C_ROW_ALT,        /* the striped rows of a table */
    UI_C_FIELD,          /* text field background */
    UI_C_CONTROL,        /* a button's face */
    UI_C_CONTROL_EDGE,   /* a control's outline */
    UI_C_CONTROL_OFF,    /* a disabled control's face */
    UI_C_COUNT
};
unsigned ui_color(int token);
#define UI_WINDOW   ui_color(UI_C_WINDOW)
#define UI_TEXT     ui_color(UI_C_LABEL)
#define UI_DIM      ui_color(UI_C_SECONDARY)
#define UI_ACCENT   ui_color(UI_C_ACCENT)
#define UI_BORDER   ui_color(UI_C_CONTROL_EDGE)
#define UI_WHITE    0xFFFFFF   /* literal white (text on the accent, a paper canvas) */

/* Pixels to draw on, with a clip rectangle. */
typedef struct {
    unsigned *px;
    int w, h, stride;
    int cx0, cy0, cx1, cy1;        /* drawing stays inside [cx0, cx1) x [cy0, cy1) */
} ui_canvas;

void ui_canvas_init(ui_canvas *c, unsigned *px, int w, int h, int stride);
void ui_clip(ui_canvas *c, int x, int y, int w, int h);   /* inside the canvas */
void ui_noclip(ui_canvas *c);

void ui_fill(ui_canvas *c, int x, int y, int w, int h, unsigned color);
void ui_fill_a(ui_canvas *c, int x, int y, int w, int h, unsigned color, int alpha);   /* alpha 0-255 */
void ui_blend_pixel(ui_canvas *c, int x, int y, unsigned color, int alpha);
void ui_frame(ui_canvas *c, int x, int y, int w, int h, unsigned color);

/* Anti-aliased round corners; `corners` picks which are round. */
#define UI_TL 1
#define UI_TR 2
#define UI_BL 4
#define UI_BR 8
#define UI_ALL 15
void ui_round(ui_canvas *c, int x, int y, int w, int h, int r, unsigned color);
void ui_round_ex(ui_canvas *c, int x, int y, int w, int h, int r, unsigned color, int alpha, int corners);
void ui_circle(ui_canvas *c, int cx, int cy, int r, unsigned color);
void ui_shadow(ui_canvas *c, int x, int y, int w, int h, int r, int size, int alpha);   /* soft, around the rect */
void ui_shadow_ex(ui_canvas *c, int x, int y, int w, int h, int r, int size, int alpha, int outside);
void ui_copy(ui_canvas *c, int x, int y, const unsigned *src, int w, int h, int stride);
void ui_copy_round(ui_canvas *c, int x, int y, const unsigned *src, int w, int h, int stride, int r, int corners);

/* Radius tokens (points; multiply by the window's scale): the macOS scale. */
#define UI_R_XSMALL 2
#define UI_R_SMALL  4
#define UI_R_MEDIUM 6
#define UI_R_LARGE  8
#define UI_R_XXL    16
#define UI_R_FULL   999       /* a capsule: the radius is clamped to half the height */

/* Appearance settings last seen from the window server (WS_PREF_*), updated by
 * ui_open, ui_present, ui_poll and ui_wait. */
#define UI_PREF_DARK     WS_PREF_DARK
#define UI_PREF_CONTRAST WS_PREF_CONTRAST
#define UI_PREF_MOTION   WS_PREF_MOTION
#define UI_PREF_TRANSP   WS_PREF_TRANSP
int ui_pref(int bit);

/* Text: anti-aliased (UIFONT.BIN), in a line 16 * scale pixels high whose top is y.
 * Tracking follows the macOS type spec (about -0.025 em below 20 px, -0.031 em
 * from 20 px up), applied to every face except the monospaced one. */
enum { UI_REGULAR, UI_BOLD, UI_MONO };
int ui_text(ui_canvas *c, int x, int y, const char *s, unsigned color, int scale);  /* -> width */
int ui_text_bold(ui_canvas *c, int x, int y, const char *s, unsigned color, int scale);
int ui_text_style(ui_canvas *c, int x, int y, const char *s, unsigned color, int scale, int style);
int ui_text_width(const char *s, int scale);
int ui_text_style_width(const char *s, int scale, int style);
int ui_char(ui_canvas *c, int x, int y, unsigned char ch, unsigned color, int scale);  /* a mono 8x16 cell */
/* Leading: 129% of the font size (22 for 17). ui_text_block wraps `s` (words and
 * '\n') into `width` pixels, one leading apart; -> its height. c = 0: measure only. */
int ui_leading(int scale);
int ui_text_block(ui_canvas *c, int x, int y, int width, const char *s, unsigned color, int scale, int style);
#define UI_CHAR_W(scale) (8 * (scale))
#define UI_CHAR_H(scale) (16 * (scale))

/* Icons from ICONS.BIN (folder, file, info, picture, video, gear, computer,
 * user, terminal), smoothly scaled to `size`; alpha fades. -> -1 if none. */
int ui_icon(ui_canvas *c, int x, int y, int size, const char *name, int alpha);
int ui_has_icon(const char *name);

/* Widgets: drawn on request, hit-tested with ui_inside. */

/* The focus ring: draw it BEFORE the control. It sits 2 px outside the control
 * and its radius is the control's radius + 2 px (the HIG rule), accent coloured,
 * and fully opaque under Increase Contrast. */
void ui_focus_ring(ui_canvas *c, int x, int y, int w, int h, int r, int scale);

/* Button states (ui_button_ex flags). A default button is accent filled. Large
 * buttons are capsules; the others are rounded rectangles (radius.medium). */
#define UI_BTN_PRESSED  1
#define UI_BTN_FOCUSED  2
#define UI_BTN_DEFAULT  4
#define UI_BTN_DISABLED 8
#define UI_BTN_HOVER    16
#define UI_BTN_LARGE    32
void ui_button_ex(ui_canvas *c, int x, int y, int w, int h, const char *label, int scale, int flags);
void ui_button(ui_canvas *c, int x, int y, int w, int h, const char *label, int scale, int pressed);
void ui_textfield(ui_canvas *c, int x, int y, int w, int h, const char *text, int scale, int focused);
void ui_list(ui_canvas *c, int x, int y, int w, int h, const char *const *items, int count,
             int selected, int first, int scale);
void ui_list_icons(ui_canvas *c, int x, int y, int w, int h, const char *const *items,
                   const char *const *icon_names, int count, int selected, int first, int scale);
int ui_list_rows(int h, int scale);          /* how many rows fit */
int ui_list_hit(int y, int scale);           /* row (from the list's top) under y */
static inline int ui_inside(int px, int py, int x, int y, int w, int h) {
    return px >= x && py >= y && px < x + w && py < y + h;
}

/* A window from the window server. */
typedef struct {
    int id;
    int scale;                     /* 1, or 2 on a large screen: multiply sizes by it */
    int x, y;                      /* where it opened on the screen */
    ui_canvas c;                   /* the window's content */
    struct ws_surface *s;
} ui_window;

int ui_desktop(void);                                      /* 1 if the window server runs */
int ui_scale(void);                                        /* before opening a window */
int ui_open(ui_window *w, const char *title, int width, int height);   /* -> 0, < 0 no desktop */
void ui_set_title(ui_window *w, const char *title);
void ui_set_resizable(ui_window *w);                      /* it redraws on WS_EV_RESIZE: zoom allowed */
void ui_present(ui_window *w);                            /* show what was drawn */
int ui_poll(ui_window *w, struct ws_event *e);             /* -> 1 if there was an event */
int ui_wait(ui_window *w, struct ws_event *e, int timeout_ms);   /* 0 = no limit; -> 0 on timeout */

#endif