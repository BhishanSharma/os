#include "print.h"
#include "ports.h"
#include <stdarg.h>

#define VGA_CTRL_REGISTER 0x3D4
#define VGA_DATA_REGISTER 0x3D5

extern void outb(uint16_t port, uint8_t val);
static void move_cursor(void);

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