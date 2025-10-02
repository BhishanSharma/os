#include "print.h"
#include <stdarg.h>

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
}

void print_newLine() {
    col = 0;
    if (row < (NUM_ROWS - 1)) {
        row++;
        return;
    }

    // Scroll everything up
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

void print_hex(uint32_t value) {
    char* hex_digits = "0123456789ABCDEF";
    print_str("0x");

    for (int i = 28; i >= 0; i -= 4) {
        uint8_t digit = (value >> i) & 0xF;
        print_char(hex_digits[digit]);
    }
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
                case 'x': {
                    uint32_t val = va_arg(args, uint32_t);
                    print_hex(val);
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
