#include "drivers/keyboard.h"
#include "lib/print.h"
#include "../lib/ports.h"
#include "lib/string.h"
#include "drivers/pic.h"
#include "sys/task.h"
#include "drivers/mouse.h"

#define KEYBOARD_DATA_PORT 0x60
#define HISTORY_SIZE 20
#define MAX_CMD_LEN 256

#define SHIFT_UP_COMBO   KEY_SCROLL_UP
#define SHIFT_DOWN_COMBO KEY_SCROLL_DOWN

static int ctrl_pressed = 0;
static int shift_pressed = 0;
static int caps_lock = 0;

static int key_buffer[256];
static int buffer_index = 0;
static int extended_scancode = 0;
volatile int keyboard_ctrl_c = 0;   // set on every Ctrl+C; whoever cares clears it

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
    // The controller also carries the mouse: a byte from it is not a key.
    if (inb(0x64) & 0x20) {
        mouse_handle_byte(inb(KEYBOARD_DATA_PORT));
        return;
    }
    keyboard_scancode(inb(KEYBOARD_DATA_PORT));
}

/* One byte of PS/2 scan code set 1, from the keyboard port or translated
 * from a USB keyboard. */
void keyboard_scancode(uint8_t scancode) {
    // Check for extended scancode prefix (0xE0)
    if (scancode == 0xE0) {
        extended_scancode = 1;
        return;
    }

    // Handle key releases
    if (scancode & 0x80) {
        scancode &= 0x7F;  // Remove release bit
        extended_scancode = 0;  // E0-prefixed releases (arrows) end here too

        // Track modifier key releases
        if (scancode == 0x1D) ctrl_pressed = 0;   // Left or right Ctrl
        if (scancode == 0x2A || scancode == 0x36) shift_pressed = 0;  // Shift
        return;
    }

    // Buffer full (nobody is reading keys): drop the key rather than overflow.
    if (buffer_index >= (int)(sizeof(key_buffer) / sizeof(key_buffer[0])) - 2) {
        extended_scancode = 0;
        return;
    }

    // Handle key presses
    if (extended_scancode) {
        // Arrow keys
        switch (scancode) {
            case 0x1D: ctrl_pressed = 1; break;   // Right Ctrl (E0 1D)
            case 0x48:
                if (shift_pressed)
                    key_buffer[buffer_index++] = SHIFT_UP_COMBO;
                else
                    key_buffer[buffer_index++] = KEY_UP_ARROW;
                break;
            case 0x50:
                if (shift_pressed)
                    key_buffer[buffer_index++] = SHIFT_DOWN_COMBO;
                else
                    key_buffer[buffer_index++] = KEY_DOWN_ARROW;
                break;
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
                case 0x2E:                                                   // C
                    key_buffer[buffer_index++] = KEY_CTRL_C;
                    keyboard_ctrl_c = 1;
                    break;
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
    else {
        outb(0xA1, inb(0xA1) & ~(1 << (irq - 8)));
        outb(0x21, inb(0x21) & ~(1 << 2));   // cascade line: slave -> master
    }
}

void init_keyboard() {
    print_str("Keyboard initialized\n");
    enable_irq(1);
}

static void (*idle_hook)(void);

void keyboard_set_idle_hook(void (*hook)(void)) {
    idle_hook = hook;
}

void keyboard_idle(void) {
    if (idle_hook) idle_hook();
    if (task_running())
        task_sleep(10);              // one tick: other tasks run, or the CPU halts
    else
        __asm__ volatile("hlt");
}

void keyboard_flush(void) {
    uint64_t flags;
    __asm__ volatile("pushfq; pop %0; cli" : "=r"(flags) :: "memory");
    buffer_index = 0;
    keyboard_ctrl_c = 0;
    if (flags & 0x200) __asm__ volatile("sti");
}

int keyboard_inject(int key) {
    uint64_t flags;
    __asm__ volatile("pushfq; pop %0; cli" : "=r"(flags) :: "memory");
    int ok = buffer_index < (int)(sizeof(key_buffer) / sizeof(key_buffer[0])) - 2;
    if (ok) key_buffer[buffer_index++] = key;
    if (flags & 0x200) __asm__ volatile("sti");
    return ok;
}

int get_char() {
    // The keyboard interrupt appends to the same buffer: keep it out meanwhile.
    uint64_t flags;
    __asm__ volatile("pushfq; pop %0; cli" : "=r"(flags) :: "memory");
    int c = 0;
    if (buffer_index > 0) {
        c = key_buffer[0];
        for (int i = 0; i < buffer_index - 1; i++)
            key_buffer[i] = key_buffer[i+1];
        buffer_index--;
    }
    if (flags & 0x200) __asm__ volatile("sti");
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
        int c = get_char();
        if (!c) {
            keyboard_idle();
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

        if (c == SHIFT_UP_COMBO) {
            scroll_up_lines(1);
            continue;
        }

        if (c == SHIFT_DOWN_COMBO) {
            scroll_down_lines(1);
            continue;
        }
        
        // Filter out ALL other special keys (left/right arrows, Ctrl combinations)
        if (c >= 0x80 || (c < 32 && c != '\b' && c != '\n' && c != '\r')) {
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