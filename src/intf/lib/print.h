#ifndef PRINT_H
#define PRINT_H

#include <stdint.h>
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

#endif