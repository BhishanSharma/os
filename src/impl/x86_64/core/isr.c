#include "drivers/keyboard.h"

extern void irq1_stub();
extern void keyboard_handler();

void isr_keyboard() {
    keyboard_handler();
    // Send EOI
    __asm__ volatile("outb %0, %1" : : "a"((uint8_t)0x20), "Nd"((uint16_t)0x20));
}
