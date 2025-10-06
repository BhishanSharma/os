#include "lib/print.h"
#include "ports.h"
#include <stdarg.h>
#include "lib/string.h"

#define VGA_CTRL_REGISTER 0x3D4
#define VGA_DATA_REGISTER 0x3D5

extern void outb(uint16_t port, uint8_t val);
static void move_cursor(void);

static color_theme_t current_theme = THEME_DEFAULT;
static uint8_t theme_fg = PRINT_COLOR_WHITE;
static uint8_t theme_bg = PRINT_COLOR_BLACK;
static uint8_t theme_accent = PRINT_COLOR_CYAN;
static uint8_t theme_error = PRINT_COLOR_LIGHT_RED;
static uint8_t theme_success = PRINT_COLOR_LIGHT_GREEN;
static uint8_t theme_warning = PRINT_COLOR_YELLOW;

const static size_t NUM_COLS = 80;
const static size_t NUM_ROWS = 25;

struct Char {
    uint8_t character;
    uint8_t color;
};

struct Char* buffer = (struct Char*) 0xb8000;
size_t col = 0;
size_t row = 0;
uint8_t color = PRINT_COLOR_WHITE | (PRINT_COLOR_BLUE << 4);

void clear_row(int row) {
    struct Char empty = {
        .character = ' ',
        .color = color,
    };
    for (size_t col = 0; col < NUM_COLS; col++) {
        buffer[col + NUM_COLS * row] = empty;
    }
}

void print_clear() {
    for (int i = 0; i < NUM_ROWS; i++) {
        clear_row(i);
    }
    col = 0;
    row = 0;
    move_cursor();
}

void print_newLine() {
    col = 0;
    if (row < (NUM_ROWS - 1)) {
        row++;
        return;
    }

    for (size_t r = 1; r < NUM_ROWS; r++) {
        for (size_t c = 0; c < NUM_COLS; c++) {
            buffer[c + NUM_COLS * (r - 1)] = buffer[c + NUM_COLS * r];
        }
    }

    clear_row(NUM_ROWS - 1);
}

void print_char(char character) {
    if (character == '\n') {
        print_newLine();
        move_cursor();
        return;
    }

    if (character == '\b') {
        if (col > 0) {
            col--;
        } else if (row > 0) {
            row--;
            col = NUM_COLS - 1;
        }
        buffer[col + NUM_COLS * row] = (struct Char){
            .character = ' ',
            .color = color,
        };
        move_cursor();
        return;
    }

    if (col >= NUM_COLS) {
        print_newLine();
    }

    buffer[col + NUM_COLS * row] = (struct Char){
        .character = (uint8_t) character,
        .color = color,
    };
    col++;
    move_cursor();
}

void print_str(const char* str) {
    for (size_t i = 0; str[i] != '\0'; i++) {
        print_char(str[i]);
    }
}

void print_set_color(uint8_t foreground, uint8_t background) {
    color = foreground | (background << 4);
}

void print_int(int value) {
    char buffer[32];
    int i = 0;

    if (value == 0) {
        print_char('0');
        return;
    }

    if (value < 0) {
        print_char('-');
        value = -value;
    }

    while (value > 0) {
        buffer[i++] = '0' + (value % 10);
        value /= 10;
    }

    for (int j = i - 1; j >= 0; j--) {
        print_char(buffer[j]);
    }
}

void print_uint(uint32_t value) {
    char buffer[32];
    int i = 0;

    if (value == 0) {
        print_char('0');
        return;
    }

    while (value > 0) {
        buffer[i++] = '0' + (value % 10);
        value /= 10;
    }

    for (int j = i - 1; j >= 0; j--) {
        print_char(buffer[j]);
    }
}

void print_uint64(uint64_t value) {
    char buffer[32];
    int i = 0;

    if (value == 0) {
        print_char('0');
        return;
    }

    while (value > 0) {
        buffer[i++] = '0' + (value % 10);
        value /= 10;
    }

    for (int j = i - 1; j >= 0; j--) {
        print_char(buffer[j]);
    }
}

void print_hex(uint32_t value) {
    char* hex_digits = "0123456789ABCDEF";
    print_str("0x");

    for (int i = 28; i >= 0; i -= 4) {
        uint8_t digit = (value >> i) & 0xF;
        print_char(hex_digits[digit]);
    }
}

void print_hex64(uint64_t value) {
    char* hex_digits = "0123456789ABCDEF";
    print_str("0x");

    for (int i = 60; i >= 0; i -= 4) {
        uint8_t digit = (value >> i) & 0xF;
        print_char(hex_digits[digit]);
    }
}

void print_bin(uint32_t value) {
    print_str("0b");
    for (int i = 31; i >= 0; i--) {
        print_char((value & (1 << i)) ? '1' : '0');
        if (i % 8 == 0 && i != 0) print_char('_');
    }
}

void print_repeat(char c, size_t count) {
    for (size_t i = 0; i < count; i++) {
        print_char(c);
    }
}

void print_line(void) {
    print_repeat('-', NUM_COLS);
}

void print_centered(const char* str) {
    size_t len = 0;
    while (str[len] != '\0') len++;
    
    if (len >= NUM_COLS) {
        print_str(str);
        return;
    }
    
    size_t padding = (NUM_COLS - len) / 2;
    print_repeat(' ', padding);
    print_str(str);
    print_newLine();
}

size_t print_get_row(void) {
    return row;
}

size_t print_get_col(void) {
    return col;
}

void print_set_pos(size_t new_col, size_t new_row) {
    if (new_col < NUM_COLS) col = new_col;
    if (new_row < NUM_ROWS) row = new_row;
    move_cursor();
}

void print_at(size_t at_col, size_t at_row, const char* str) {
    size_t old_col = col;
    size_t old_row = row;
    
    print_set_pos(at_col, at_row);
    print_str(str);
    
    col = old_col;
    row = old_row;
    move_cursor();
}

void print_box(const char* title, const char* content) {
    size_t title_len = 0;
    while (title[title_len] != '\0') title_len++;
    
    size_t content_len = 0;
    while (content[content_len] != '\0') content_len++;
    
    size_t box_width = (title_len > content_len ? title_len : content_len) + 4;
    if (box_width > NUM_COLS - 2) box_width = NUM_COLS - 2;
    
    // Top border
    print_char('+');
    print_repeat('-', box_width - 2);
    print_str("+\n");
    
    // Title
    print_str("| ");
    print_str(title);
    size_t padding = box_width - title_len - 4;
    print_repeat(' ', padding);
    print_str(" |\n");
    
    // Middle border
    print_char('+');
    print_repeat('-', box_width - 2);
    print_str("+\n");
    
    // Content
    print_str("| ");
    print_str(content);
    padding = box_width - content_len - 4;
    print_repeat(' ', padding);
    print_str(" |\n");
    
    // Bottom border
    print_char('+');
    print_repeat('-', box_width - 2);
    print_str("+\n");
}

void kprintf(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);

    while (*fmt) {
        if (*fmt == '%') {
            fmt++;
            switch (*fmt) {
                case 'd': {
                    int val = va_arg(args, int);
                    print_int(val);
                    break;
                }
                case 'u': {
                    uint32_t val = va_arg(args, uint32_t);
                    print_uint(val);
                    break;
                }
                case 'l': {
                    fmt++;
                    if (*fmt == 'u') {
                        uint64_t val = va_arg(args, uint64_t);
                        print_uint64(val);
                    } else if (*fmt == 'x') {
                        uint64_t val = va_arg(args, uint64_t);
                        print_hex64(val);
                    }
                    break;
                }
                case 'x': {
                    uint32_t val = va_arg(args, uint32_t);
                    print_hex(val);
                    break;
                }
                case 'b': {
                    uint32_t val = va_arg(args, uint32_t);
                    print_bin(val);
                    break;
                }
                case 's': {
                    char* str = va_arg(args, char*);
                    print_str(str);
                    break;
                }
                case 'c': {
                    char c = (char)va_arg(args, int);
                    print_char(c);
                    break;
                }
                case '%': {
                    print_char('%');
                    break;
                }
                default: {
                    print_char('%');
                    print_char(*fmt);
                    break;
                }
            }
        } else {
            print_char(*fmt);
        }
        fmt++;
    }

    va_end(args);
}

static void move_cursor(void) {
    uint16_t pos = row * NUM_COLS + col;

    outb(VGA_CTRL_REGISTER, 0x0F);
    outb(VGA_DATA_REGISTER, (uint8_t)(pos & 0xFF));
    outb(VGA_CTRL_REGISTER, 0x0E);
    outb(VGA_DATA_REGISTER, (uint8_t)((pos >> 8) & 0xFF));
}

color_theme_t print_get_current_theme(void) {
    return current_theme;
}

void print_set_theme(color_theme_t theme) {
    current_theme = theme;
    
    switch (theme) {
        case THEME_DRACULA:
            theme_bg = PRINT_COLOR_BLACK;
            theme_fg = PRINT_COLOR_WHITE;
            theme_accent = PRINT_COLOR_MAGENTA;
            theme_error = PRINT_COLOR_RED;
            theme_success = PRINT_COLOR_GREEN;
            theme_warning = PRINT_COLOR_YELLOW;
            break;
            
        case THEME_NORD:
            theme_bg = PRINT_COLOR_DARK_GRAY;
            theme_fg = PRINT_COLOR_LIGHT_GRAY;
            theme_accent = PRINT_COLOR_LIGHT_CYAN;
            theme_error = PRINT_COLOR_LIGHT_RED;
            theme_success = PRINT_COLOR_LIGHT_GREEN;
            theme_warning = PRINT_COLOR_YELLOW;
            break;
            
        case THEME_MONOKAI:
            theme_bg = PRINT_COLOR_BLACK;
            theme_fg = PRINT_COLOR_LIGHT_GRAY;
            theme_accent = PRINT_COLOR_LIGHT_GREEN;
            theme_error = PRINT_COLOR_PINK;
            theme_success = PRINT_COLOR_GREEN;
            theme_warning = PRINT_COLOR_YELLOW;
            break;
            
        case THEME_GRUVBOX:
            theme_bg = PRINT_COLOR_BLACK;
            theme_fg = PRINT_COLOR_LIGHT_GRAY;
            theme_accent = PRINT_COLOR_BROWN;
            theme_error = PRINT_COLOR_RED;
            theme_success = PRINT_COLOR_GREEN;
            theme_warning = PRINT_COLOR_YELLOW;
            break;
            
        case THEME_SOLARIZED:
            theme_bg = PRINT_COLOR_DARK_GRAY;
            theme_fg = PRINT_COLOR_LIGHT_GRAY;
            theme_accent = PRINT_COLOR_CYAN;
            theme_error = PRINT_COLOR_RED;
            theme_success = PRINT_COLOR_GREEN;
            theme_warning = PRINT_COLOR_YELLOW;
            break;
            
        case THEME_MATRIX:
            theme_bg = PRINT_COLOR_BLACK;
            theme_fg = PRINT_COLOR_GREEN;
            theme_accent = PRINT_COLOR_LIGHT_GREEN;
            theme_error = PRINT_COLOR_RED;
            theme_success = PRINT_COLOR_LIGHT_GREEN;
            theme_warning = PRINT_COLOR_YELLOW;
            break;
            
        case THEME_CYBERPUNK:
            theme_bg = PRINT_COLOR_BLACK;
            theme_fg = PRINT_COLOR_CYAN;
            theme_accent = PRINT_COLOR_MAGENTA;
            theme_error = PRINT_COLOR_PINK;
            theme_success = PRINT_COLOR_LIGHT_CYAN;
            theme_warning = PRINT_COLOR_YELLOW;
            break;
            
        case THEME_DEFAULT:
        default:
            theme_bg = PRINT_COLOR_BLUE;
            theme_fg = PRINT_COLOR_WHITE;
            theme_accent = PRINT_COLOR_LIGHT_CYAN;
            theme_error = PRINT_COLOR_LIGHT_RED;
            theme_success = PRINT_COLOR_LIGHT_GREEN;
            theme_warning = PRINT_COLOR_YELLOW;
            break;
    }
    
    // Apply theme globally
    print_set_color(theme_fg, theme_bg);
    color = theme_fg | (theme_bg << 4);
    
    // Clear and redraw screen with new theme
    print_clear();
}

void print_status_bar(const char* text) {
    size_t old_row = row;
    size_t old_col = col;
    uint8_t old_color = color;
    
    print_set_color(theme_bg, theme_accent);
    print_set_pos(0, 0);
    
    print_str(text);
    for (size_t i = strlen(text); i < NUM_COLS; i++) {
        print_char(' ');
    }
    
    color = old_color;
    print_set_pos(old_col, old_row);
}

void print_error(const char* text) {
    uint8_t old_color = color;
    print_set_color(theme_error, theme_bg);
    print_str("[ERROR] ");
    print_str(text);
    print_str("\n");
    color = old_color;
}

void print_success(const char* text) {
    uint8_t old_color = color;
    print_set_color(theme_success, theme_bg);
    print_str("[OK] ");
    print_str(text);
    print_str("\n");
    color = old_color;
}

void print_warning(const char* text) {
    uint8_t old_color = color;
    print_set_color(theme_warning, theme_bg);
    print_str("[WARN] ");
    print_str(text);
    print_str("\n");
    color = old_color;
}

void print_info(const char* text) {
    uint8_t old_color = color;
    print_set_color(theme_accent, theme_bg);
    print_str("[INFO] ");
    print_str(text);
    print_str("\n");
    color = old_color;
}

void print_prompt(const char* text) {
    uint8_t old_color = color;
    print_set_color(theme_accent, theme_bg);
    print_str(text);
    color = old_color;
}

// Enhanced print_box with theme colors
void print_box_themed(const char* title, const char* content) {
    size_t title_len = strlen(title);
    size_t content_len = strlen(content);
    size_t box_width = (title_len > content_len ? title_len : content_len) + 4;
    if (box_width > NUM_COLS - 2) box_width = NUM_COLS - 2;
    
    uint8_t old_color = color;
    print_set_color(theme_accent, theme_bg);
    
    // Top border
    print_char('+');
    print_repeat('-', box_width - 2);
    print_str("+\n");
    
    // Title
    print_str("| ");
    print_set_color(theme_fg, theme_bg);
    print_str(title);
    print_set_color(theme_accent, theme_bg);
    size_t padding = box_width - title_len - 4;
    print_repeat(' ', padding);
    print_str(" |\n");
    
    // Middle border
    print_char('+');
    print_repeat('-', box_width - 2);
    print_str("+\n");
    
    // Content
    print_str("| ");
    print_set_color(theme_fg, theme_bg);
    print_str(content);
    print_set_color(theme_accent, theme_bg);
    padding = box_width - content_len - 4;
    print_repeat(' ', padding);
    print_str(" |\n");
    
    // Bottom border
    print_char('+');
    print_repeat('-', box_width - 2);
    print_str("+\n");
    
    color = old_color;
}