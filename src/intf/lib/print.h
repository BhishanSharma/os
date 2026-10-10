#ifndef PRINT_H
#define PRINT_H

#include <stdint.h>
#include <stdarg.h>
#include <stddef.h>

// VGA colors
enum {
    PRINT_COLOR_BLACK = 0,
    PRINT_COLOR_BLUE = 1,
    PRINT_COLOR_GREEN = 2,
    PRINT_COLOR_CYAN = 3,
    PRINT_COLOR_RED = 4,
    PRINT_COLOR_MAGENTA = 5,
    PRINT_COLOR_BROWN = 6,
    PRINT_COLOR_LIGHT_GRAY = 7,
    PRINT_COLOR_DARK_GRAY = 8,
    PRINT_COLOR_LIGHT_BLUE = 9,
    PRINT_COLOR_LIGHT_GREEN = 10,
    PRINT_COLOR_LIGHT_CYAN = 11,
    PRINT_COLOR_LIGHT_RED = 12,
    PRINT_COLOR_PINK = 13,
    PRINT_COLOR_YELLOW = 14,
    PRINT_COLOR_WHITE = 15,
};

// Modern dark theme presets
typedef enum {
    THEME_DEFAULT,      // White on blue (classic)
    THEME_DRACULA,      // Purple/cyan on dark
    THEME_NORD,         // Blue/cyan on dark gray
    THEME_MONOKAI,      // Green/yellow on black
    THEME_GRUVBOX,      // Orange/green on dark
    THEME_SOLARIZED,    // Cyan/green on dark gray
    THEME_MATRIX,       // Green on black
    THEME_CYBERPUNK,    // Cyan/magenta on black
} color_theme_t;

void print_clear(void);
void print_char(char character);
void print_str(const char* str);
void print_set_color(uint8_t foreground, uint8_t background);
void print_int(int value);
void print_hex(uint32_t value);
void print_newLine(void);
void kprintf(const char* fmt, ...);
void vkprintf(const char* fmt, va_list args);

/* Framebuffer console support (see lib/fbcon.h). Call print_use_shadow_buffer()
 * before printing anything when the screen is a framebuffer; the text grid
 * then lives in RAM and print_flush() draws what changed. */
void print_use_shadow_buffer(size_t cols, size_t rows);   /* grid size, see fbcon_grid_size */
void print_flush(void);
/* The screen mode changed: `cols` x `total_rows` cells, status bar included
 * (framebuffer console only). Keeps the scrollback and the current line. */
void print_resize(size_t cols, size_t total_rows);
void print_hide_cursor(void);   /* framebuffer console: stop drawing the cursor (panic screen) */
/* Group many screen updates (e.g. a full-screen redraw) into one framebuffer
 * redraw at print_batch_end(). Calls nest. */
void print_batch_begin(void);
void print_batch_end(void);
/* The active grid as VGA cells (character | attribute << 8), print_get_cols() wide. */
volatile uint16_t* print_text_cells(void);

void print_uint(uint32_t value);
void print_uint64(uint64_t value);
void print_hex64(uint64_t value);
void print_bin(uint32_t value);
void print_centered(const char* str);
void print_repeat(char c, size_t count);
void print_line(void);
void print_at(size_t col, size_t row, const char* str);
void print_box(const char* title, const char* content);
size_t print_get_row(void);
size_t print_get_col(void);
void print_set_pos(size_t col, size_t row);
size_t print_get_cols(void);   /* text grid size: 80x25 in VGA text mode */
size_t print_get_rows(void);

// New theme functions
void print_set_theme(color_theme_t theme);
void print_status_bar(const char* text);
void print_error(const char* text);
void print_success(const char* text);
void print_warning(const char* text);
void print_info(const char* text);
void print_prompt(const char* text);
color_theme_t print_get_current_theme(void);

void expand_scrollback(void);
void scroll_up_lines(int lines);
void scroll_down_lines(int lines);
void scroll_to_bottom(void);
void scroll_to_top(void);
int is_at_bottom(void);
void get_scrollback_info(int* capacity, int* total_lines, int* view_offset);

/* Status bar: reserve the top screen row (call once, before print_clear), then
 * draw `left` and right-aligned `right` into it in the theme's accent colour. */
void print_reserve_status_line(void);
void print_status_line(const char *left, const char *right);

/* Quiet boot: while muted, output goes only to serial and the boot log. */
void print_set_muted(int muted);
const char *print_get_bootlog(size_t *len);   /* everything printed during boot */
void print_bootlog_stop(void);                /* stop recording (boot finished) */

typedef enum { BOOT_OK, BOOT_WARN, BOOT_FAIL } boot_state_t;
/* "  [  OK  ] Label        detail" -- shown even while muted. */
void print_boot_status(boot_state_t state, const char *label, const char *fmt, ...);
void print_accent(const char *text);      /* theme accent colour */
void print_highlight(const char *text);   /* theme success colour */
void print_shell_prompt(const char *user_host, const char *path, int root);   /* "#" for root, else "$" */
void print_set_theme_colors(void);         /* back to the theme's text colours */
void print_set_cursor_visible(int visible);
/* Text colour from the theme: 0 text, 1 accent, 2 success, 3 error, 4 warning
 * (THEME_* in sys/syscall_nums.h). */
void print_use_theme_color(int role);

#endif