#include "timer.h"
#include "print.h"
#include "../lib/ports.h"
#include "idt.h"
#include <stdint.h>

extern void enable_irq(uint8_t irq);

typedef struct registers {
    uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
    uint64_t rdi, rsi, rbp, rsp, rbx, rdx, rcx, rax;
    uint64_t int_no;    // Interrupt number (optional)
    uint64_t err_code;  // Error code (for some interrupts)
} registers_t;


static uint32_t tick = 0;

// Called on every timer interrupt (IRQ0)
void isr_timer(registers_t regs) {
    tick++;
}

// Initialize PIT (Programmable Interval Timer)
void timer_init() {
    uint32_t divisor = 1193182 / TIMER_FREQ;

    outb(0x43, 0x36); // Command byte
    outb(0x40, (uint8_t)(divisor & 0xFF));      // Low byte
    outb(0x40, (uint8_t)((divisor >> 8) & 0xFF)); // High byte
    enable_irq(0);
    print_str("Timer initialized\n");
}

// Get total tick count
uint32_t get_tick() {
    return tick;
}

// Get uptime in seconds
uint32_t get_seconds() {
    return tick / TIMER_FREQ;
}

// Simple busy-wait sleep
void sleep(uint32_t ms) {
    uint32_t target_ticks = tick + (ms * TIMER_FREQ) / 1000;
    while (tick < target_ticks) {
        asm volatile("hlt"); // Halt CPU until next interrupt (saves power)
    }
}
