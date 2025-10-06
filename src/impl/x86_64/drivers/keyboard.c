#include "drivers/keyboard.h"
#include "lib/print.h"
#include "../lib/ports.h"
#include "lib/string.h"

#define KEYBOARD_DATA_PORT 0x60
#define HISTORY_SIZE 20
#define MAX_CMD_LEN 256

static int ctrl_pressed = 0;
static int shift_pressed = 0;
static int caps_lock = 0;

static char key_buffer[256];
static int buffer_index = 0;
static int extended_scancode = 0;

// Command history
static char command_history[HISTORY_SIZE][MAX_CMD_LEN];
static int history_count = 0;
static int history_index = -1;
static int history_current = 0;

// Normal characters (unshifted)
unsigned char kbdus[128] = {
    0,  27, '1','2','3','4','5','6','7','8','9','0','-','=', '\b',
    '\t', 'q','w','e','r','t','y','u','i','o','p','[',']','\n', 0,
    'a','s','d','f','g','h','j','k','l',';','\'','`',  0,'\\','z',
    'x','c','v','b','n','m',',','.','/', 0, '*', 0, ' ',
};

// Shifted characters
unsigned char kbdus_shift[128] = {
    0,  27, '!','@','#','$','%','^','&','*','(',')','_','+', '\b',
    '\t', 'Q','W','E','R','T','Y','U','I','O','P','{','}','\n', 0,
    'A','S','D','F','G','H','J','K','L',':','"','~',  0,'|','Z',
    'X','C','V','B','N','M','<','>','?', 0, '*', 0, ' ',
};

void keyboard_handler() {
    uint8_t scancode = inb(KEYBOARD_DATA_PORT);

    // Check for extended scancode prefix (0xE0)
    if (scancode == 0xE0) {
        extended_scancode = 1;
        return;
    }

    // Handle key releases
    if (scancode & 0x80) {
        scancode &= 0x7F;  // Remove release bit
        
        // Track modifier key releases
        if (scancode == 0x1D) ctrl_pressed = 0;   // Left Ctrl
        if (scancode == 0x2A || scancode == 0x36) shift_pressed = 0;  // Shift
        return;
    }

    // Handle key presses
    if (extended_scancode) {
        // Arrow keys
        switch (scancode) {
            case 0x48: key_buffer[buffer_index++] = KEY_UP_ARROW; break;
            case 0x50: key_buffer[buffer_index++] = KEY_DOWN_ARROW; break;
            case 0x4B: key_buffer[buffer_index++] = KEY_LEFT_ARROW; break;
            case 0x4D: key_buffer[buffer_index++] = KEY_RIGHT_ARROW; break;
        }
        extended_scancode = 0;
    } else {
        // Track modifier keys
        if (scancode == 0x1D) {  // Left Ctrl
            ctrl_pressed = 1;
            return;
        }
        if (scancode == 0x2A || scancode == 0x36) {  // Shift
            shift_pressed = 1;
            return;
        }
        if (scancode == 0x3A) {  // Caps Lock
            caps_lock = !caps_lock;
            return;
        }
        
        // Handle Ctrl combinations
        if (ctrl_pressed) {
            switch (scancode) {
                case 0x10: key_buffer[buffer_index++] = KEY_CTRL_Q; break;  // Q
                case 0x1F: key_buffer[buffer_index++] = KEY_CTRL_S; break;  // S
                case 0x31: key_buffer[buffer_index++] = KEY_CTRL_N; break;  // N
                case 0x20: key_buffer[buffer_index++] = KEY_CTRL_D; break;  // D
                case 0x12: key_buffer[buffer_index++] = KEY_CTRL_E; break;  // E
                default: return;
            }
        } else {
            // Normal key - apply shift or caps lock
            char c;
            if (shift_pressed) {
                c = kbdus_shift[scancode];
            } else {
                c = kbdus[scancode];
                // Apply caps lock to letters only
                if (caps_lock && c >= 'a' && c <= 'z') {
                    c = c - 32;  // Convert to uppercase
                }
            }
            if (c) key_buffer[buffer_index++] = c;
        }
    }
    key_buffer[buffer_index] = '\0';
}

void enable_irq(uint8_t irq) {
    if (irq < 8)
        outb(0x21, inb(0x21) & ~(1 << irq));
    else
        outb(0xA1, inb(0xA1) & ~(1 << (irq - 8)));
}

void init_keyboard() {
    print_str("Keyboard initialized\n");
    enable_irq(1);
}

char get_char() {
    if (buffer_index == 0) return 0;
    char c = key_buffer[0];
    for (int i = 0; i < buffer_index - 1; i++)
        key_buffer[i] = key_buffer[i+1];
    buffer_index--;
    return c;
}

// Add command to history
void history_add(const char* cmd) {
    if (cmd[0] == '\0') return;  // Don't add empty commands
    
    // Check if it's the same as the last command
    if (history_count > 0 && 
        strcmp(command_history[(history_current - 1 + HISTORY_SIZE) % HISTORY_SIZE], cmd) == 0) {
        return;  // Don't add duplicates
    }
    
    // Copy command to history
    int i = 0;
    while (cmd[i] && i < MAX_CMD_LEN - 1) {
        command_history[history_current][i] = cmd[i];
        i++;
    }
    command_history[history_current][i] = '\0';
    
    // Update history pointers
    history_current = (history_current + 1) % HISTORY_SIZE;
    if (history_count < HISTORY_SIZE) {
        history_count++;
    }
    history_index = -1;  // Reset browsing position
}

// Get previous command from history
const char* history_prev() {
    if (history_count == 0) return NULL;
    
    if (history_index == -1) {
        history_index = (history_current - 1 + HISTORY_SIZE) % HISTORY_SIZE;
    } else {
        int prev = (history_index - 1 + HISTORY_SIZE) % HISTORY_SIZE;
        if (prev == history_current) return NULL;  // Reached oldest
        history_index = prev;
    }
    
    return command_history[history_index];
}

// Get next command from history
const char* history_next() {
    if (history_index == -1) return NULL;
    
    int next = (history_index + 1) % HISTORY_SIZE;
    if (next == history_current) {
        history_index = -1;
        return "";  // Return empty to clear line
    }
    
    history_index = next;
    return command_history[history_index];
}

void get_line(char* buffer, size_t max_len) {
    size_t index = 0;
    buffer[0] = '\0';

    while (1) {
        char c = get_char();
        if (!c) {
            __asm__ volatile("hlt");
            continue;
        }

        // Handle arrow keys FIRST - before any other processing
        if (c == KEY_UP_ARROW) {
            const char* prev_cmd = history_prev();
            if (prev_cmd) {
                // Clear current line
                while (index > 0) {
                    print_str("\b \b");
                    index--;
                }
                
                // Display previous command
                index = 0;
                while (prev_cmd[index] && index < max_len - 1) {
                    buffer[index] = prev_cmd[index];
                    print_char(prev_cmd[index]);
                    index++;
                }
                buffer[index] = '\0';
            }
            continue;  // CRITICAL: Skip rest of loop
        }
        
        if (c == KEY_DOWN_ARROW) {
            const char* next_cmd = history_next();
            if (next_cmd != NULL) {
                // Clear current line
                while (index > 0) {
                    print_str("\b \b");
                    index--;
                }
                
                // Display next command
                index = 0;
                while (next_cmd[index] && index < max_len - 1) {
                    buffer[index] = next_cmd[index];
                    print_char(next_cmd[index]);
                    index++;
                }
                buffer[index] = '\0';
            }
            continue;  // Skip rest of loop
        }
        
        // Filter out ALL other special keys (left/right arrows, etc.)
        if (c >= 0x80) {
            continue;  // Silently ignore
        }

        // Handle backspace
        if (c == '\b') {
            if (index > 0) {
                index--;
                print_str("\b \b");
            }
            continue;
        }

        // Handle enter
        if (c == '\n' || c == '\r') {
            buffer[index] = '\0';
            print_str("\n");
            
            // Add to history if not empty
            if (index > 0) {
                history_add(buffer);
            }
            break;
        }

        // Normal character
        if (index < max_len - 1) {
            buffer[index++] = c;
            print_char(c);
        }
    }
}