#include "../lib/ports.h"

// Keyboard controller ports
#define KB_DATA_PORT    0x60
#define KB_STATUS_PORT  0x64
#define KB_COMMAND_PORT 0x64

// Status register bits
#define KB_STATUS_OUTPUT_FULL  0x01
#define KB_STATUS_INPUT_FULL   0x02

// Commands
#define KB_CMD_READ_CONFIG     0x20
#define KB_CMD_WRITE_CONFIG    0x60
#define KB_CMD_DISABLE_MOUSE   0xA7
#define KB_CMD_DISABLE_KB      0xAD
#define KB_CMD_PULSE_RESET     0xFE

extern uint8_t inb(uint16_t port);
extern void outb(uint16_t port, uint8_t val);

// Wait for keyboard controller to be ready
static void kb_wait_input() {
    uint32_t timeout = 100000;
    while (timeout--) {
        if (!(inb(KB_STATUS_PORT) & KB_STATUS_INPUT_FULL)) {
            return;
        }
    }
}

static void kb_wait_output() {
    uint32_t timeout = 100000;
    while (timeout--) {
        if (inb(KB_STATUS_PORT) & KB_STATUS_OUTPUT_FULL) {
            return;
        }
    }
}

void reboot() {
    // Disable interrupts
    asm volatile("cli");

    // Method 1: Keyboard controller reset (most reliable)
    kb_wait_input();
    outb(KB_COMMAND_PORT, KB_CMD_DISABLE_KB);
    
    kb_wait_input();
    outb(KB_COMMAND_PORT, KB_CMD_DISABLE_MOUSE);
    
    kb_wait_input();
    outb(KB_COMMAND_PORT, KB_CMD_PULSE_RESET);  // Reset CPU
    
    // Wait a bit
    for (volatile int i = 0; i < 1000000; i++);

    // Method 2: ACPI reset (if method 1 fails)
    outb(0xCF9, 0x02);  // Clear reset bit
    outb(0xCF9, 0x06);  // Set reset bit + full reset
    
    for (volatile int i = 0; i < 1000000; i++);

    // Method 3: Triple fault (last resort)
    struct {
        uint16_t limit;
        uint64_t base;
    } __attribute__((packed)) null_idt = {0, 0};
    
    asm volatile(
        "lidt %0\n"
        "int $0x03\n"  // Trigger interrupt with null IDT
        :
        : "m"(null_idt)
    );

    // Fallback: halt
    while(1) {
        asm volatile("hlt");
    }
}