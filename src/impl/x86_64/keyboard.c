#include "keyboard.h"
#include "print.h"

#define KEYBOARD_DATA_PORT 0x60

static char key_buffer[256];
static int buffer_index = 0;

unsigned char kbdus[128] = {
    0,  27, '1','2','3','4','5','6','7','8','9','0','-','=', '\b',
    '\t', 'q','w','e','r','t','y','u','i','o','p','[',']','\n', 0,
    'a','s','d','f','g','h','j','k','l',';','\'','`',  0,'\\','z',
    'x','c','v','b','n','m',',','.','/', 0, '*', 0, ' ',
};

static inline uint8_t inb(uint16_t port) {
    uint8_t ret;
    __asm__ volatile("inb %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

static inline void outb(uint16_t port, uint8_t val) {
    __asm__ volatile ("outb %0, %1" : : "a"(val), "Nd"(port));
}

void keyboard_handler() {
    uint8_t scancode = inb(KEYBOARD_DATA_PORT);

    if (!(scancode & 0x80)) {
        char c = kbdus[scancode];
        if (c) {
            key_buffer[buffer_index++] = c;
            key_buffer[buffer_index] = '\0';
        }
    }
}

void enable_irq(uint8_t irq) {
    if (irq < 8)
        outb(0x21, inb(0x21) & ~(1 << irq));  // master PIC
    else
        outb(0xA1, inb(0xA1) & ~(1 << (irq - 8))); // slave PIC
}

void init_keyboard() {
    print_str("Keyboard initialized\n");
    enable_irq(1); // unmask IRQ1 (keyboard)
}

char get_char() {
    if (buffer_index == 0) return 0;
    char c = key_buffer[0];
    for (int i = 0; i < buffer_index - 1; i++)
        key_buffer[i] = key_buffer[i+1];
    buffer_index--;
    return c;
}

void get_line(char* buffer, size_t max_len) {
    size_t index = 0;

    while (1) {
        char c = get_char();  // get next char from keyboard buffer
        if (!c) {
            __asm__ volatile("hlt"); // halt CPU until next key
            continue;
        }

        if (c == '\b') { // backspace
            if (index > 0) {
                index--;
                print_str("\b \b"); // erase char from screen
            }
            continue;
        }

        if (c == '\n' || c == '\r') { // enter key
            buffer[index] = '\0';    // null-terminate string
            print_str("\n");          // move to next line
            break;
        }

        // normal character
        if (index < max_len - 1) {
            buffer[index++] = c;
            print_char(c); // echo typed char
        }
    }
}
