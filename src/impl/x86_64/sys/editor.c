#include "sys/editor.h"
#include "lib/print.h"
#include "drivers/keyboard.h"
#include "lib/string.h"
#include "drivers/fat32.h"
#include "drivers/heap.h"

#define MAX_LINES 100
#define MAX_LINE_LENGTH 80

static char* lines[MAX_LINES];
static int line_count = 0;
static int current_line = 0;
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

// Load file into editor
static int editor_load_file(const char* filename) {
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
        return -1;
    }
    
    int bytes = fat32_read_file(filename, buffer, size);
    if (bytes < 0) {
        kfree(buffer);
        return -1;
    }
    
    buffer[bytes] = '\0';
    
    // Parse into lines
    int pos = 0;
    int line_start = 0;
    
    for (int i = 0; i <= bytes && line_count < MAX_LINES; i++) {
        if (buffer[i] == '\n' || buffer[i] == '\0') {
            int line_len = i - line_start;
            if (line_len > MAX_LINE_LENGTH) line_len = MAX_LINE_LENGTH;
            
            lines[line_count] = kmalloc(line_len + 1);
            if (lines[line_count]) {
                for (int j = 0; j < line_len; j++) {
                    lines[line_count][j] = buffer[line_start + j];
                }
                lines[line_count][line_len] = '\0';
                line_count++;
            }
            line_start = i + 1;
        }
    }
    
    kfree(buffer);
    return 0;
}

// Save file from editor
static int editor_save_file() {
    // Calculate total size
    uint32_t total_size = 0;
    for (int i = 0; i < line_count; i++) {
        total_size += strlen(lines[i]) + 1; // +1 for newline
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
        buffer[pos++] = '\n';
    }
    
    // Create file if it doesn't exist
    if (!fat32_file_exists(current_filename)) {
        fat32_create_file(current_filename);
    }
    
    int result = fat32_write_file(current_filename, buffer, total_size);
    kfree(buffer);
    
    return (result > 0) ? 0 : -1;
}

// Display editor screen
static void editor_display() {
    print_clear();
    
    // Header
    print_set_color(PRINT_COLOR_BLACK, PRINT_COLOR_CYAN);
    print_str(" EDIT: ");
    print_str(current_filename);
    for (int i = strlen(current_filename) + 7; i < 80; i++) {
        print_char(' ');
    }
    print_set_color(PRINT_COLOR_LIGHT_GRAY, PRINT_COLOR_BLACK);
    print_str("\n");
    
    // Display lines
    for (int i = 0; i < 20 && i < line_count; i++) {
        if (i == current_line) {
            print_set_color(PRINT_COLOR_BLACK, PRINT_COLOR_LIGHT_GRAY);
            print_str("> ");
        } else {
            print_set_color(PRINT_COLOR_LIGHT_GRAY, PRINT_COLOR_BLACK);
            print_str("  ");
        }
        
        if (lines[i]) {
            print_str(lines[i]);
        }
        print_set_color(PRINT_COLOR_LIGHT_GRAY, PRINT_COLOR_BLACK);
        print_str("\n");
    }
    
    // Footer
    print_set_pos(0, 24);
    print_set_color(PRINT_COLOR_BLACK, PRINT_COLOR_CYAN);
    print_str(" ^S Save | ^Q Quit | ^N New Line | ^D Delete Line | ^E Edit Line ");
    for (int i = 65; i < 80; i++) {
        print_char(' ');
    }
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
    
    // Main editor loop
    editor_display();
    
    while (1) {
        char c = get_char();
        if (!c) {
            __asm__ volatile("hlt");
            continue;
        }
        
        // Handle Ctrl commands
        if (c == 17) { // Ctrl-Q
            print_clear();
            print_info("Exiting editor");
            editor_free_lines();
            return;
        }
        
        if (c == 19) { // Ctrl-S
            if (editor_save_file() == 0) {
                print_clear();
                print_success("File saved");
                editor_display();
            } else {
                print_clear();
                print_error("Failed to save file");
                editor_display();
            }
            continue;
        }
        
        if (c == 14) { // Ctrl-N - New line
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
        
        if (c == 4) { // Ctrl-D - Delete line
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
        
        if (c == 5) { // Ctrl-E - Edit current line
            if (current_line < line_count) {
                print_set_pos(0, 22);
                print_set_color(PRINT_COLOR_YELLOW, PRINT_COLOR_BLACK);
                print_str("Edit line: ");
                
                char line_buffer[MAX_LINE_LENGTH];
                if (lines[current_line]) {
                    int len = strlen(lines[current_line]);
                    for (int i = 0; i < len && i < MAX_LINE_LENGTH - 1; i++) {
                        line_buffer[i] = lines[current_line][i];
                        print_char(line_buffer[i]);
                    }
                    line_buffer[len] = '\0';
                    get_line(line_buffer + len, MAX_LINE_LENGTH - len);
                } else {
                    get_line(line_buffer, MAX_LINE_LENGTH);
                }
                
                // Update line
                if (lines[current_line]) {
                    kfree(lines[current_line]);
                }
                int len = strlen(line_buffer);
                lines[current_line] = kmalloc(len + 1);
                if (lines[current_line]) {
                    for (int i = 0; i <= len; i++) {
                        lines[current_line][i] = line_buffer[i];
                    }
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