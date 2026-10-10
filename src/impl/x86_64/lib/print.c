#include "lib/print.h"
#include "ports.h"
#include <stdarg.h>
#include "lib/string.h"
#include "lib/serial.h"
#include "lib/fbcon.h"

#define VGA_CTRL_REGISTER 0x3D4
#define VGA_DATA_REGISTER 0x3D5
#define VIDEO_MEMORY 0xB8000

// Scrollback buffer configuration
#define EARLY_SCROLLBACK_LINES 50   // Small buffer for early boot
#define MAX_SCROLLBACK_LINES 2000   // After heap initialization
#define SCROLLBACK_CELLS (2000 * 80) // Fallback budget on a small heap: 2000 lines at 80 columns

// Screen grid: 80x25 in VGA text mode; on a framebuffer, whatever fits the screen
// (print_use_shadow_buffer). Read through these macros everywhere.
#define MAX_COLS 256
#define MAX_ROWS 100
static size_t num_cols = 80;
static size_t num_rows = 25;
#define VISIBLE_ROWS ((int)num_rows)
#define VISIBLE_COLS ((int)num_cols)
#define NUM_COLS num_cols
#define NUM_ROWS num_rows

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


struct Char {
    uint8_t character;
    uint8_t color;
};

// Small static buffer for early boot
static struct Char early_buffer[EARLY_SCROLLBACK_LINES * MAX_COLS];

// Pointer to current scrollback buffer (starts with early_buffer)
// Scrollback lines are num_cols cells wide; SB(line, col) addresses one cell.
static struct Char *scrollback_buffer = early_buffer;
#define SB(line, c) scrollback_buffer[(size_t)(line) * num_cols + (size_t)(c)]
static int scrollback_capacity = EARLY_SCROLLBACK_LINES;
static int scrollback_write_line = 0;
static int scrollback_view_offset = 0;
static int scrollback_total_lines = 0;
static int scrollback_expanded = 0;

struct Char* buffer = (struct Char*) 0xb8000;
size_t col = 0;
size_t row = 0;
uint8_t color = PRINT_COLOR_WHITE | (PRINT_COLOR_BLUE << 4);

/* Framebuffer mode: the 80x25 grid lives in RAM (`shadow`) and print_flush()
 * draws the cells that changed since the last flush. `drawn` is what is on
 * screen; `drawn_cursor` is the cell index the cursor was drawn in. */
static struct Char shadow[MAX_ROWS * MAX_COLS];
static struct Char drawn[MAX_ROWS * MAX_COLS];
static int drawn_valid = 0;
static int drawn_cursor = -1;
static int cursor_hidden = 0;
static int flush_deferred = 0;   // >0 while print_str/kprintf run: redraw once at the end

/* Rows reserved at the top for the status bar (print_reserve_status_line).
 * `buffer` and num_rows describe only the text area below them; `screen` is
 * the whole grid, status rows included. */
static struct Char *screen = (struct Char *)0xb8000;
static int status_rows = 0;

/* Quiet boot: while muted, output goes only to serial and the boot log. The
 * boot log keeps everything printed until print_bootlog_stop() (`dmesg`). */
static int console_muted = 0;
#define BOOTLOG_SIZE 16384
static char bootlog[BOOTLOG_SIZE];
static size_t bootlog_len = 0;
static int bootlog_on = 1;

void print_batch_begin(void) {
    flush_deferred++;
}

void print_batch_end(void) {
    if (flush_deferred > 0) flush_deferred--;
    if (!flush_deferred) print_flush();
}

void print_hide_cursor(void) {
    cursor_hidden = 1;
}

void print_use_shadow_buffer(size_t cols, size_t rows) {
    num_cols = cols < 40 ? 40 : (cols > MAX_COLS ? MAX_COLS : cols);
    num_rows = rows < 10 ? 10 : (rows > MAX_ROWS ? MAX_ROWS : rows);
    for (int i = 0; i < VISIBLE_ROWS * VISIBLE_COLS; i++)
        shadow[i] = (struct Char){ .character = ' ', .color = color };
    buffer = shadow;
    screen = shadow;
}

volatile uint16_t* print_text_cells(void) {
    return (volatile uint16_t*)buffer;
}

/* Mouse pointer and selection (whole-grid cell indexes, status row included):
 * drawn with foreground and background swapped. */
static int pointer_cell = -1;
static int graphics_mode;        /* a program draws pixels: leave the text area alone */
static int select_from = -1, select_to = -1;

static uint8_t shown_color(int i) {
    uint8_t c = shadow[i].color;
    int selected = select_from >= 0 && i >= select_from && i <= select_to;
    if (selected != (i == pointer_cell)) c = (uint8_t)((c >> 4) | (c << 4));
    return c;
}

void print_flush(void) {
    if (screen != shadow || !fbcon_active()) return;
    int cursor = (!cursor_hidden && col < NUM_COLS) ? (int)((row + status_rows) * NUM_COLS + col) : -1;
    int last = graphics_mode ? status_rows * VISIBLE_COLS : (VISIBLE_ROWS + status_rows) * VISIBLE_COLS;
    for (int i = 0; i < last; i++) {
        uint8_t c = shown_color(i);
        int changed = !drawn_valid || shadow[i].character != drawn[i].character ||
                      c != drawn[i].color || i == cursor || i == drawn_cursor;
        if (!changed) continue;
        fbcon_draw_cell(i % VISIBLE_COLS, i / VISIBLE_COLS, shadow[i].character, c, i == cursor);
        drawn[i] = (struct Char){ shadow[i].character, c };
    }
    drawn_valid = 1;
    drawn_cursor = cursor;
}

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
            SB(i, j).character = ' ';
            SB(i, j).color = color;
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
    // 2000 lines if the heap has room, else the 80-column budget at this width.
    int new_lines = MAX_SCROLLBACK_LINES;
    struct Char *new_buffer = (struct Char *)kmalloc((size_t)new_lines * num_cols * sizeof(struct Char));
    if (new_buffer == NULL) {
        new_lines = (int)(SCROLLBACK_CELLS / num_cols);
        new_buffer = (struct Char *)kmalloc((size_t)new_lines * num_cols * sizeof(struct Char));
    }
    
    if (new_buffer == NULL) {
        print_warning("Failed to expand scrollback buffer - kmalloc returned NULL");
        return;
    }
    
    // Copy existing data from early buffer
    int lines_to_copy = (scrollback_total_lines < EARLY_SCROLLBACK_LINES) ? 
                        scrollback_total_lines : EARLY_SCROLLBACK_LINES;
    
    for (int i = 0; i < lines_to_copy; i++) {
        for (int j = 0; j < VISIBLE_COLS; j++) {
            new_buffer[(size_t)i * num_cols + j] = SB(i, j);
        }
    }
    
    // Initialize rest of new buffer
    for (int i = lines_to_copy; i < new_lines; i++) {
        for (int j = 0; j < VISIBLE_COLS; j++) {
            new_buffer[(size_t)i * num_cols + j].character = ' ';
            new_buffer[(size_t)i * num_cols + j].color = color;
        }
    }
    
    // Switch to new buffer
    scrollback_buffer = new_buffer;
    scrollback_capacity = new_lines;
    scrollback_expanded = 1;

    char msg[48] = "Scrollback expanded to ";
    size_t n = strlen(msg);
    char digits[8]; int d = 0;
    do { digits[d++] = (char)('0' + new_lines % 10); new_lines /= 10; } while (new_lines);
    while (d) msg[n++] = digits[--d];
    const char *tail = " lines";
    while (*tail) msg[n++] = *tail++;
    msg[n] = 0;
    print_success(msg);
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
            buffer[c + VISIBLE_COLS * display_row] = SB(buffer_line, c);
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
        SB(scrollback_write_line, c) = buffer[c + NUM_COLS * row];
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
    serial_putc(character);   // mirror everything to COM1 (survives crashes)
    if (bootlog_on && bootlog_len < BOOTLOG_SIZE) bootlog[bootlog_len++] = character;
    if (console_muted) return;
    if (scrollback_view_offset) scroll_to_bottom();   // new output: back to the live screen
    select_from = select_to = -1;                      // the selection would point at old text

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
    flush_deferred++;
    for (size_t i = 0; str[i] != '\0'; i++) {
        print_char(str[i]);
    }
    flush_deferred--;
    move_cursor();
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

size_t print_get_cols(void) {
    return num_cols;
}

size_t print_get_rows(void) {
    return num_rows;
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

static void kprintf_put(char c, void *ctx) {
    (void)ctx;
    print_char(c);
}

void vkprintf(const char* fmt, va_list args) {
    flush_deferred++;
    k_vformat(kprintf_put, 0, fmt, args);
    flush_deferred--;
    move_cursor();
}

/* printf-style; see k_vformat in lib/string.h for the supported formats. */
void kprintf(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vkprintf(fmt, args);
    va_end(args);
}

static void move_cursor(void) {
    if (screen == shadow) {      /* framebuffer console: no VGA cursor registers */
        if (!flush_deferred) print_flush();
        return;
    }
    uint16_t pos = (row + status_rows) * NUM_COLS + col;

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

/* While the scrollback is shown, the live screen waits here (with the line
 * being typed, which is not in the scrollback yet) and comes back unchanged. */
static struct Char live_screen[MAX_ROWS * MAX_COLS];

static void save_live_screen(void) {
    if (scrollback_view_offset == 0)
        memcpy(live_screen, buffer, (size_t)VISIBLE_ROWS * VISIBLE_COLS * sizeof(struct Char));
}

static void restore_live_screen(void) {
    memcpy(buffer, live_screen, (size_t)VISIBLE_ROWS * VISIBLE_COLS * sizeof(struct Char));
    move_cursor();
}

// Scroll up in history (Shift+Up or Mouse Wheel Up)
void scroll_up_lines(int lines) {
    int max_scroll = scrollback_total_lines - VISIBLE_ROWS;
    if (max_scroll < 0) max_scroll = 0;
    if (max_scroll == 0) return;

    save_live_screen();
    scrollback_view_offset += lines;
    if (scrollback_view_offset > max_scroll) {
        scrollback_view_offset = max_scroll;
    }
    
    refresh_display();
}

// Scroll down in history (Shift+Down or Mouse Wheel Down)
void scroll_down_lines(int lines) {
    if (scrollback_view_offset == 0) return;
    scrollback_view_offset -= lines;
    if (scrollback_view_offset <= 0) {
        scrollback_view_offset = 0;
        restore_live_screen();
        return;
    }
    refresh_display();
}

// Check if we're viewing live content
int is_at_bottom(void) {
    return (scrollback_view_offset == 0);
}

// Jump to bottom (end of scrollback)
void scroll_to_bottom(void) {
    if (scrollback_view_offset == 0) return;
    scrollback_view_offset = 0;
    restore_live_screen();
}

// Jump to top of scrollback
void scroll_to_top(void) {
    int max_scroll = scrollback_total_lines - VISIBLE_ROWS;
    if (max_scroll < 0) max_scroll = 0;
    if (max_scroll == 0) return;
    save_live_screen();
    scrollback_view_offset = max_scroll;
    refresh_display();
}

// Get scrollback info for debugging
void get_scrollback_info(int* capacity, int* total_lines, int* view_offset) {
    if (capacity) *capacity = scrollback_capacity;
    if (total_lines) *total_lines = scrollback_total_lines;
    if (view_offset) *view_offset = scrollback_view_offset;
}

/* ---- Status bar, quiet boot, styled output ------------------------------- */

void print_reserve_status_line(void) {
    if (status_rows) return;
    screen = buffer;
    status_rows = 1;
    num_rows -= 1;
    buffer = screen + num_cols;
    if (row >= num_rows) row = num_rows - 1;
}

void print_status_line(const char *left, const char *right) {
    if (!status_rows) return;
    uint8_t bg = theme_accent;
    if (screen != shadow) bg &= 7;   // VGA text mode: bit 3 of the background means blink
    uint8_t attr = PRINT_COLOR_BLACK | (bg << 4);
    size_t left_len = strlen(left), right_len = strlen(right);
    for (size_t c = 0; c < num_cols; c++) {
        char ch = ' ';
        if (c < left_len) ch = left[c];
        else if (right_len <= num_cols && c >= num_cols - right_len && num_cols - right_len >= left_len)
            ch = right[c - (num_cols - right_len)];
        screen[c] = (struct Char){ .character = (uint8_t)ch, .color = attr };
    }
    if (!flush_deferred) print_flush();
}

void print_set_muted(int muted) {
    console_muted = muted;
    if (!muted) move_cursor();
}

const char *print_get_bootlog(size_t *len) {
    *len = bootlog_len;
    return bootlog;
}

void print_bootlog_stop(void) {
    bootlog_on = 0;
}

static void print_colored(uint8_t fg, const char *text) {
    uint8_t old_color = color;
    print_set_color(fg, theme_bg);
    print_str(text);
    color = old_color;
}

void print_accent(const char *text) {
    print_colored(theme_accent, text);
}

void print_highlight(const char *text) {
    print_colored(theme_success, text);
}

void print_shell_prompt(const char *user_host, const char *path, int root) {
    flush_deferred++;
    print_colored(root ? theme_error : theme_success, user_host);
    print_str(":");
    print_colored(theme_accent, path);
    print_str(root ? "# " : "$ ");
    flush_deferred--;
    move_cursor();
}

void print_boot_status(boot_state_t state, const char *label, const char *fmt, ...) {
    int was_muted = console_muted;
    console_muted = 0;
    flush_deferred++;

    print_str("  [");
    switch (state) {
        case BOOT_OK:   print_colored(theme_success, "  OK  "); break;
        case BOOT_WARN: print_colored(theme_warning, " WARN "); break;
        default:        print_colored(theme_error,   " FAIL "); break;
    }
    print_str("] ");
    char padded[16];
    k_snprintf(padded, sizeof(padded), "%-12s", label);
    print_colored(theme_accent, padded);

    va_list args;
    va_start(args, fmt);
    vkprintf(fmt, args);
    va_end(args);
    print_char('\n');

    flush_deferred--;
    console_muted = was_muted;
    move_cursor();
}

void print_set_theme_colors(void) {
    color = theme_fg | (theme_bg << 4);
}

void print_use_theme_color(int role) {
    uint8_t fg = role == 1 ? theme_accent : role == 2 ? theme_success : role == 3 ? theme_error
               : role == 4 ? theme_warning : theme_fg;
    color = fg | (theme_bg << 4);
}

void print_set_cursor_visible(int visible) {
    cursor_hidden = !visible;
    move_cursor();
}

/* ---- Resizing (the screen mode changed) ---------------------------------- */

extern void kfree(void *ptr);

/* New grid size: `cols` x `total_rows` cells, status bar included. The
 * scrollback is copied to the new width (long lines are cut, short ones
 * padded) and the newest lines fill the screen again. */
void print_resize(size_t cols, size_t total_rows) {
    if (screen != shadow) return;                 // VGA text mode is fixed at 80x25
    if (cols < 40) cols = 40;
    if (cols > MAX_COLS) cols = MAX_COLS;
    if (total_rows < 10) total_rows = 10;
    if (total_rows > MAX_ROWS) total_rows = MAX_ROWS;

    // The line being typed, which is not in the scrollback yet.
    struct Char current[MAX_COLS];
    size_t old_cols = num_cols;
    for (size_t c = 0; c < MAX_COLS; c++)
        current[c] = c < old_cols ? buffer[c + old_cols * row] : (struct Char){ ' ', color };

    // Scrollback at the new width, oldest line first.
    int lines = scrollback_total_lines < scrollback_capacity ? scrollback_total_lines : scrollback_capacity;
    int capacity = scrollback_capacity;
    struct Char *old = scrollback_buffer;
    struct Char *fresh = scrollback_expanded ? kmalloc((size_t)capacity * cols * sizeof(struct Char)) : 0;
    if (!fresh) {                                  // early buffer, or no memory: reuse it, drop the history
        fresh = old;
        capacity = old == early_buffer ? EARLY_SCROLLBACK_LINES
                                       : (int)((size_t)scrollback_capacity * old_cols / cols);
        lines = 0;
    } else {
        int oldest = (scrollback_write_line - lines + scrollback_capacity) % scrollback_capacity;
        for (int i = 0; i < lines; i++) {
            const struct Char *src = &old[(size_t)((oldest + i) % scrollback_capacity) * old_cols];
            for (size_t c = 0; c < cols; c++)
                fresh[(size_t)i * cols + c] = c < old_cols ? src[c] : (struct Char){ ' ', color };
        }
        kfree(old);
    }
    scrollback_buffer = fresh;
    scrollback_capacity = capacity;
    scrollback_total_lines = lines;
    scrollback_write_line = lines % capacity;
    scrollback_view_offset = 0;

    num_cols = cols;
    num_rows = total_rows - status_rows;
    buffer = screen + num_cols * status_rows;
    for (size_t i = 0; i < total_rows * num_cols; i++) shadow[i] = (struct Char){ ' ', color };

    // The newest lines, then the current one on the last used row.
    size_t shown = (size_t)lines < num_rows - 1 ? (size_t)lines : num_rows - 1;
    for (size_t r = 0; r < shown; r++) {
        int line = lines - (int)shown + (int)r;
        for (size_t c = 0; c < num_cols; c++) buffer[c + num_cols * r] = SB(line, c);
    }
    row = shown;
    for (size_t c = 0; c < num_cols; c++) buffer[c + num_cols * row] = current[c];
    if (col >= num_cols) col = num_cols - 1;

    drawn_valid = 0;
    drawn_cursor = -1;
    move_cursor();
}

/* ---- Mouse: pointer, selection, screen text ------------------------------ */

int print_pointer_supported(void) {
    return screen == shadow && fbcon_active();
}

size_t print_grid_rows(void) {
    return num_rows + status_rows;
}

void print_set_pointer(int c, int r) {
    int cell = (c >= 0 && r >= 0 && (size_t)c < num_cols && (size_t)r < num_rows + status_rows)
             ? r * (int)num_cols + c : -1;
    if (cell == pointer_cell) return;
    pointer_cell = cell;
    if (!flush_deferred) print_flush();
}

/* Cells from (c0, r0) to (c1, r1) in reading order, either way round; a
 * negative c0 clears it. */
void print_set_selection(int c0, int r0, int c1, int r1) {
    if (c0 < 0) {
        select_from = select_to = -1;
    } else {
        int a = r0 * (int)num_cols + c0, b = r1 * (int)num_cols + c1;
        select_from = a < b ? a : b;
        select_to = a < b ? b : a;
    }
    if (!flush_deferred) print_flush();
}

/* The text of the selection: one line per screen row, trailing spaces
 * dropped. Returns its length. */
size_t print_selection_text(char *out, size_t size) {
    size_t n = 0;
    if (select_from < 0 || size == 0) {
        if (size) out[0] = 0;
        return 0;
    }
    int total = (int)((num_rows + status_rows) * num_cols);
    int line_start = select_from;
    while (line_start <= select_to && line_start < total) {
        int r = line_start / (int)num_cols;
        int line_end = (r + 1) * (int)num_cols - 1;
        if (line_end > select_to) line_end = select_to;
        int last = line_end;
        while (last >= line_start && shadow[last].character == ' ') last--;
        for (int i = line_start; i <= last && n + 1 < size; i++) {
            uint8_t ch = shadow[i].character;
            out[n++] = (ch >= 32 && ch < 127) ? (char)ch : '?';
        }
        if (line_end < select_to && n + 1 < size) out[n++] = '\n';
        line_start = line_end + 1;
    }
    out[n] = 0;
    return n;
}

/* ---- Graphics mode (a program draws images) ------------------------------ */

void print_set_graphics(int on) {
    if (screen != shadow || !fbcon_active() || graphics_mode == on) return;
    graphics_mode = on;
    if (!on) {                                   // give the screen back: redraw it all
        uint32_t w, h, cw, ch, gx, gy;
        fbcon_geometry(&w, &h, &cw, &ch, &gx, &gy);
        fbcon_fill(0, 0, w, h, 0);
        drawn_valid = 0;
        drawn_cursor = -1;
    }
    print_flush();
}

int print_graphics_mode(void) {
    return graphics_mode;
}

size_t print_status_rows(void) {
    return (size_t)status_rows;
}
