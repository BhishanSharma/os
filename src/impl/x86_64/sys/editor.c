#include "sys/editor.h"
#include "lib/print.h"
#include "drivers/keyboard.h"
#include "lib/string.h"
#include "drivers/fat32.h"
#include "drivers/heap.h"

#define MAX_LINES 8000
#define EDIT_MAX  256        // longest line Ctrl-E can edit

static char* lines[MAX_LINES];
static int line_count = 0;
static int current_line = 0;
static int top_line = 0;               // first line shown on screen
static int use_crlf = 0;               // file had CRLF line endings: save them back
static const char* read_only_reason;   // non-null: saving would lose data
static char current_filename[256];

// Free all allocated lines
static void editor_free_lines() {
    for (int i = 0; i < line_count; i++) {
        if (lines[i]) {
            kfree(lines[i]);
            lines[i] = 0;
        }
    }
    line_count = 0;
}

static int visible_lines(void) {
    int n = (int)print_get_rows() - 5;   // header, gap, edit prompt, gap, footer
    return n < 1 ? 1 : n;
}

// Load file into editor. Lines keep their full length; a trailing CR (CRLF
// files) is stripped and remembered. Files that cannot round-trip through the
// editor (NUL bytes, more than MAX_LINES lines) open read-only.
static int editor_load_file(const char* filename) {
    use_crlf = 0;
    read_only_reason = 0;
    if (!fat32_file_exists(filename)) {
        // New file
        return 0;
    }

    uint32_t size = fat32_get_file_size(filename);
    if (size == 0) {
        return 0;
    }

    uint8_t* buffer = kmalloc(size + 1);
    if (!buffer) {
        read_only_reason = "not enough memory to load it";
        return -1;
    }

    int bytes = fat32_read_file(filename, buffer, size);
    if (bytes < 0) {
        kfree(buffer);
        read_only_reason = "it could not be read";
        return -1;
    }

    int line_start = 0;
    for (int i = 0; i <= bytes; i++) {
        if (i < bytes && buffer[i] == 0) read_only_reason = "it contains NUL (binary) bytes";
        if (i < bytes && buffer[i] != '\n') continue;
        if (i == bytes && line_start == bytes) break;   // no empty line after a final newline
        if (line_count == MAX_LINES) {
            read_only_reason = "it has too many lines";
            break;
        }

        int line_len = i - line_start;
        if (line_len > 0 && buffer[line_start + line_len - 1] == '\r') {
            line_len--;
            use_crlf = 1;
        }
        lines[line_count] = kmalloc(line_len + 1);
        if (!lines[line_count]) {
            read_only_reason = "not enough memory to load it";
            break;
        }
        for (int j = 0; j < line_len; j++) {
            char c = (char)buffer[line_start + j];
            lines[line_count][j] = c ? c : '.';     // keep strings printable
        }
        lines[line_count][line_len] = '\0';
        line_count++;
        line_start = i + 1;
    }

    kfree(buffer);
    return 0;
}

// Save file from editor
static int editor_save_file() {
    const char* eol = use_crlf ? "\r\n" : "\n";
    uint32_t eol_len = (uint32_t)strlen(eol);

    // Calculate total size
    uint32_t total_size = 0;
    for (int i = 0; i < line_count; i++) {
        total_size += strlen(lines[i]) + eol_len;
    }

    if (total_size == 0) {
        // Empty file
        if (fat32_file_exists(current_filename)) {
            return 0;
        } else {
            return fat32_create_file(current_filename);
        }
    }

    uint8_t* buffer = kmalloc(total_size);
    if (!buffer) {
        return -1;
    }

    uint32_t pos = 0;
    for (int i = 0; i < line_count; i++) {
        int len = strlen(lines[i]);
        for (int j = 0; j < len; j++) {
            buffer[pos++] = lines[i][j];
        }
        for (uint32_t j = 0; j < eol_len; j++) {
            buffer[pos++] = eol[j];
        }
    }

    // Create file if it doesn't exist
    if (!fat32_file_exists(current_filename)) {
        fat32_create_file(current_filename);
    }

    int result = fat32_write_file(current_filename, buffer, total_size);
    kfree(buffer);

    return (result > 0) ? 0 : -1;
}

// Print at most `width` characters of `s`.
static void print_clipped(const char* s, int width) {
    for (int i = 0; s[i] && i < width; i++) print_char(s[i]);
}

// Display editor screen
static void editor_display() {
    int cols = (int)print_get_cols();
    int rows = visible_lines();

    // Keep the current line on screen.
    if (current_line < top_line) top_line = current_line;
    if (current_line >= top_line + rows) top_line = current_line - rows + 1;

    print_batch_begin();        // one framebuffer redraw for the whole screen
    print_clear();

    // Header: file name, position, read-only marker
    char pos[48];
    int n = 0;
    const char* label = " line ";
    while (*label) pos[n++] = *label++;
    int values[2] = { line_count ? current_line + 1 : 0, line_count };
    for (int v = 0; v < 2; v++) {
        char digits[8]; int d = 0, x = values[v];
        do { digits[d++] = (char)('0' + x % 10); x /= 10; } while (x);
        while (d) pos[n++] = digits[--d];
        if (v == 0) pos[n++] = '/';
    }
    pos[n++] = ' ';
    pos[n] = '\0';

    print_set_color(PRINT_COLOR_BLACK, PRINT_COLOR_CYAN);
    print_str(" EDIT: ");
    print_str(current_filename);
    if (read_only_reason) print_str("  [read-only]");
    int used = 7 + (int)strlen(current_filename) + (read_only_reason ? 13 : 0);
    for (int i = used; i < cols - n; i++) {
        print_char(' ');
    }
    print_str(pos);
    print_set_color(PRINT_COLOR_LIGHT_GRAY, PRINT_COLOR_BLACK);
    print_str("\n");

    // Display lines (long lines are clipped to the screen width)
    for (int i = top_line; i < top_line + rows && i < line_count; i++) {
        if (i == current_line) {
            print_set_color(PRINT_COLOR_BLACK, PRINT_COLOR_LIGHT_GRAY);
            print_str("> ");
        } else {
            print_set_color(PRINT_COLOR_LIGHT_GRAY, PRINT_COLOR_BLACK);
            print_str("  ");
        }

        if (lines[i]) {
            print_clipped(lines[i], cols - 2);
        }
        print_set_color(PRINT_COLOR_LIGHT_GRAY, PRINT_COLOR_BLACK);
        print_str("\n");
    }

    // Footer
    const char* footer = " ^S Save | ^Q Quit | ^N New Line | ^D Delete Line | ^E Edit Line ";
    print_set_pos(0, print_get_rows() - 1);
    print_set_color(PRINT_COLOR_BLACK, PRINT_COLOR_CYAN);
    print_str(footer);
    for (int i = strlen(footer); i < cols; i++) {
        print_char(' ');
    }
    print_set_color(PRINT_COLOR_LIGHT_GRAY, PRINT_COLOR_BLACK);
    print_batch_end();
}

// One-line message above the footer, shown until the next redraw.
static void editor_message(uint8_t color, const char* a, const char* b) {
    print_set_pos(0, print_get_rows() - 3);
    print_set_color(color, PRINT_COLOR_BLACK);
    print_str(a);
    if (b) print_str(b);
    print_set_color(PRINT_COLOR_LIGHT_GRAY, PRINT_COLOR_BLACK);
}

void editor_open(const char* filename) {
    // Copy filename
    int i = 0;
    while (filename[i] && i < 255) {
        current_filename[i] = filename[i];
        i++;
    }
    current_filename[i] = '\0';

    // Load file
    editor_load_file(filename);
    current_line = 0;
    top_line = 0;

    // Main editor loop
    editor_display();
    if (read_only_reason) editor_message(PRINT_COLOR_YELLOW, "Read-only: ", read_only_reason);

    while (1) {
        int c = get_char();
        if (!c) {
            __asm__ volatile("hlt");
            continue;
        }

        // Handle Ctrl commands
        if (c == KEY_CTRL_Q) {
            print_clear();
            print_info("Exiting editor");
            editor_free_lines();
            return;
        }

        if (c == KEY_CTRL_S) {
            if (read_only_reason) {
                editor_display();
                editor_message(PRINT_COLOR_LIGHT_RED, "Not saved, the file is read-only: ", read_only_reason);
            } else if (editor_save_file() == 0) {
                editor_display();
                editor_message(PRINT_COLOR_LIGHT_GREEN, "File saved", 0);
            } else {
                editor_display();
                editor_message(PRINT_COLOR_LIGHT_RED, "Failed to save file", 0);
            }
            continue;
        }

        if (c == KEY_CTRL_N) { // New line
            if (line_count < MAX_LINES) {
                // Insert new line
                for (int i = line_count; i > current_line; i--) {
                    lines[i] = lines[i-1];
                }
                lines[current_line] = kmalloc(1);
                if (lines[current_line]) {
                    lines[current_line][0] = '\0';
                    line_count++;
                }
            }
            editor_display();
            continue;
        }

        if (c == KEY_CTRL_D) { // Delete line
            if (line_count > 0) {
                if (lines[current_line]) {
                    kfree(lines[current_line]);
                }
                for (int i = current_line; i < line_count - 1; i++) {
                    lines[i] = lines[i+1];
                }
                line_count--;
                if (current_line >= line_count && line_count > 0) {
                    current_line = line_count - 1;
                }
            }
            editor_display();
            continue;
        }

        if (c == KEY_CTRL_E) { // Edit current line (append text)
            if (current_line < line_count) {
                int len = lines[current_line] ? (int)strlen(lines[current_line]) : 0;
                if (len >= EDIT_MAX - 1) {
                    editor_message(PRINT_COLOR_YELLOW, "Line too long to edit here", 0);
                    continue;
                }
                editor_message(PRINT_COLOR_YELLOW, "Edit line: ", 0);
                print_set_color(PRINT_COLOR_YELLOW, PRINT_COLOR_BLACK);

                char line_buffer[EDIT_MAX];
                for (int i = 0; i < len; i++) {
                    line_buffer[i] = lines[current_line][i];
                    print_char(line_buffer[i]);
                }
                line_buffer[len] = '\0';
                get_line(line_buffer + len, EDIT_MAX - len);

                // Update line
                int new_len = strlen(line_buffer);
                char* updated = kmalloc(new_len + 1);
                if (updated) {
                    for (int i = 0; i <= new_len; i++) {
                        updated[i] = line_buffer[i];
                    }
                    if (lines[current_line]) kfree(lines[current_line]);
                    lines[current_line] = updated;
                }
            }
            editor_display();
            continue;
        }

        // Arrow keys
        if (c == KEY_UP_ARROW) {
            if (current_line > 0) {
                current_line--;
                editor_display();
            }
            continue;
        }

        if (c == KEY_DOWN_ARROW) {
            if (current_line < line_count - 1) {
                current_line++;
                editor_display();
            }
            continue;
        }
    }
}
