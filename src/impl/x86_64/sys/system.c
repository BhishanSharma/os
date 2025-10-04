#include "../lib/ports.h"

void reboot() {
    // Disable interrupts
    asm volatile("cli");

    // Cause a triple fault by loading a null IDT
    asm volatile(
        "lidt (%0)\n"    // Load empty IDT
        "ud2\n"          // Invalid instruction triggers #UD
        :
        : "r"(0)         // null pointer for IDT
    );

    while(1) {
        asm volatile("hlt"); // fallback halt
    }
}
