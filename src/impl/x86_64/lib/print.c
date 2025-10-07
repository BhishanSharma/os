#include "lib/print.h"
#include "ports.h"
#include <stdarg.h>
#include "lib/string.h"

#define VGA_CTRL_REGISTER 0x3D4
#define VGA_DATA_REGISTER 0x3D5
#define VIDEO_MEMORY 0xB8000

// Scrollback buffer configuration
#define EARLY_SCROLLBACK_LINES 50   // Small buffer for early boot
#define MAX_SCROLLBACK_LINES 2000   // After heap initialization
#define VISIBLE_ROWS 25
#define VISIBLE_COLS 80

extern void outb(uint16_t port, uint8_t val);
static void move_cursor(void);
static void refresh_display(void);

static color_theme_t current_theme = THEME_DEFAULT;
static uint8_t theme_fg = PRINT_COLOR_WHITE;
static uint8_t theme_bg = PRINT_COLOR_BLACK;
static uint8_t theme_accent = PRINT_COLOR_CYAN;
static uint8_t theme_error = PRINT_COLOR_LIGHT_RED;
static uint8_t theme_success = PRINT_COLOR_LIGHT_GREEN;
static uint8_t theme_warning = PRINT_COLOR_YELLOW;

const static size_t NUM_COLS = VISIBLE_COLS;
const static size_t NUM_ROWS = VISIBLE_ROWS;

struct Char {
    uint8_t character;
    uint8_t color;
};

// Small static buffer for early boot
static struct Char early_buffer[EARLY_SCROLLBACK_LINES][VISIBLE_COLS];

// Pointer to current scrollback buffer (starts with early_buffer)
static struct Char (*scrollback_buffer)[VISIBLE_COLS] = early_buffer;
static int scrollback_capacity = EARLY_SCROLLBACK_LINES;
static int scrollback_write_line = 0;
static int scrollback_view_offset = 0;
static int scrollback_total_lines = 0;
static int scrollback_expanded = 0;

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

// Initialize scrollback buffer
void init_scrollback(void) {
    for (int i = 0; i < scrollback_capacity; i++) {
        for (int j = 0; j < VISIBLE_COLS; j++) {
            scrollback_buffer[i][j].character = ' ';
            scrollback_buffer[i][j].color = color;
        }
    }
    scrollback_write_line = 0;
    scrollback_view_offset = 0;
    scrollback_total_lines = 0;
}

// Expand scrollback buffer after heap is ready
// Declare kmalloc if not already declared
extern void* kmalloc(size_t size);

void expand_scrollback(void) {
    if (scrollback_expanded) {
        return;  // Already expanded
    }
    
    // Allocate larger buffer from heap
    size_t total_size = MAX_SCROLLBACK_LINES * VISIBLE_COLS * sizeof(struct Char);
    struct Char (*new_buffer)[VISIBLE_COLS] = (struct Char (*)[VISIBLE_COLS])kmalloc(total_size);
    
    if (new_buffer == NULL) {
        print_warning("Failed to expand scrollback buffer - kmalloc returned NULL");
        return;
    }
    
    // Copy existing data from early buffer
    int lines_to_copy = (scrollback_total_lines < EARLY_SCROLLBACK_LINES) ? 
                        scrollback_total_lines : EARLY_SCROLLBACK_LINES;
    
    for (int i = 0; i < lines_to_copy; i++) {
        for (int j = 0; j < VISIBLE_COLS; j++) {
            new_buffer[i][j] = scrollback_buffer[i][j];
        }
    }
    
    // Initialize rest of new buffer
    for (int i = lines_to_copy; i < MAX_SCROLLBACK_LINES; i++) {
        for (int j = 0; j < VISIBLE_COLS; j++) {
            new_buffer[i][j].character = ' ';
            new_buffer[i][j].color = color;
        }
    }
    
    // Switch to new buffer
    scrollback_buffer = new_buffer;
    scrollback_capacity = MAX_SCROLLBACK_LINES;
    scrollback_expanded = 1;
    
    print_success("Scrollback expanded to 2000 lines");
}

// Refresh the visible display from scrollback buffer
static void refresh_display(void) {
    int start_line;
    
    if (scrollback_total_lines < VISIBLE_ROWS) {
        start_line = 0;
    } else {
        start_line = scrollback_total_lines - VISIBLE_ROWS - scrollback_view_offset;
        if (start_line < 0) start_line = 0;
    }
    
    // Copy from scrollback to video memory
    for (int display_row = 0; display_row < VISIBLE_ROWS; display_row++) {
        int buffer_line = (start_line + display_row) % scrollback_capacity;
        for (int c = 0; c < VISIBLE_COLS; c++) {
            buffer[c + VISIBLE_COLS * display_row] = scrollback_buffer[buffer_line][c];
        }
    }
    
    move_cursor();
}

void print_clear() {
    init_scrollback();
    for (int i = 0; i < NUM_ROWS; i++) {
        clear_row(i);
    }
    col = 0;
    row = 0;
    move_cursor();
}

void print_newLine() {
    // Save current line to scrollback
    for (size_t c = 0; c < NUM_COLS; c++) {
        scrollback_buffer[scrollback_write_line][c] = buffer[c + NUM_COLS * row];
    }
    
    // Move to next line in scrollback
    scrollback_write_line = (scrollback_write_line + 1) % scrollback_capacity;
    scrollback_total_lines++;
    if (scrollback_total_lines > scrollback_capacity) {
        scrollback_total_lines = scrollback_capacity;
    }
    
    col = 0;
    
    // If viewing live content, follow the new content
    if (scrollback_view_offset == 0) {
        if (row < (NUM_ROWS - 1)) {
            row++;
        } else {
            // Scroll up the display
            for (size_t r = 1; r < NUM_ROWS; r++) {
                for (size_t c = 0; c < NUM_COLS; c++) {
                    buffer[c + NUM_COLS * (r - 1)] = buffer[c + NUM_COLS * r];
                }
            }
            clear_row(NUM_ROWS - 1);
        }
    }
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
    
    print_char('+');
    print_repeat('-', box_width - 2);
    print_str("+\n");
    
    print_str("| ");
    print_str(title);
    size_t padding = box_width - title_len - 4;
    print_repeat(' ', padding);
    print_str(" |\n");
    
    print_char('+');
    print_repeat('-', box_width - 2);
    print_str("+\n");
    
    print_str("| ");
    print_str(content);
    padding = box_width - content_len - 4;
    print_repeat(' ', padding);
    print_str(" |\n");
    
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
    
    print_set_color(theme_fg, theme_bg);
    color = theme_fg | (theme_bg << 4);
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

void print_box_themed(const char* title, const char* content) {
    size_t title_len = strlen(title);
    size_t content_len = strlen(content);
    size_t box_width = (title_len > content_len ? title_len : content_len) + 4;
    if (box_width > NUM_COLS - 2) box_width = NUM_COLS - 2;
    
    uint8_t old_color = color;
    print_set_color(theme_accent, theme_bg);
    
    print_char('+');
    print_repeat('-', box_width - 2);
    print_str("+\n");
    
    print_str("| ");
    print_set_color(theme_fg, theme_bg);
    print_str(title);
    print_set_color(theme_accent, theme_bg);
    size_t padding = box_width - title_len - 4;
    print_repeat(' ', padding);
    print_str(" |\n");
    
    print_char('+');
    print_repeat('-', box_width - 2);
    print_str("+\n");
    
    print_str("| ");
    print_set_color(theme_fg, theme_bg);
    print_str(content);
    print_set_color(theme_accent, theme_bg);
    padding = box_width - content_len - 4;
    print_repeat(' ', padding);
    print_str(" |\n");
    
    print_char('+');
    print_repeat('-', box_width - 2);
    print_str("+\n");
    
    color = old_color;
}

// Scroll up in history (Shift+Up or Mouse Wheel Up)
void scroll_up_lines(int lines) {
    int max_scroll = scrollback_total_lines - VISIBLE_ROWS;
    if (max_scroll < 0) max_scroll = 0;
    
    scrollback_view_offset += lines;
    if (scrollback_view_offset > max_scroll) {
        scrollback_view_offset = max_scroll;
    }
    
    refresh_display();
}

// Scroll down in history (Shift+Down or Mouse Wheel Down)
void scroll_down_lines(int lines) {
    scrollback_view_offset -= lines;
    if (scrollback_view_offset < 0) {
        scrollback_view_offset = 0;
    }
    
    refresh_display();
}

// Check if we're viewing live content
int is_at_bottom(void) {
    return (scrollback_view_offset == 0);
}

// Jump to bottom (end of scrollback)
void scroll_to_bottom(void) {
    scrollback_view_offset = 0;
    refresh_display();
}

// Jump to top of scrollback
void scroll_to_top(void) {
    int max_scroll = scrollback_total_lines - VISIBLE_ROWS;
    if (max_scroll < 0) max_scroll = 0;
    scrollback_view_offset = max_scroll;
    refresh_display();
}

// Get scrollback info for debugging
void get_scrollback_info(int* capacity, int* total_lines, int* view_offset) {
    if (capacity) *capacity = scrollback_capacity;
    if (total_lines) *total_lines = scrollback_total_lines;
    if (view_offset) *view_offset = scrollback_view_offset;
}