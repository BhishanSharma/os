#include "print.h"
#include "keyboard.h"
#include "idt.h"

extern void irq1_stub();
void pic_remap();

void kernel_main() {
    // Initialize IDT and PIC
    idt_init();
    pic_remap();

    // Set keyboard IRQ (IRQ1) handler
    idt_set_entry(0x21, irq1_stub, 0x8E); // present, ring0, interrupt gate

    // Initialize keyboard and enable interrupts
    init_keyboard();
    __asm__ volatile("sti");

    char line[128];

    while (1) {
        print_str("> ");           // terminal prompt
        get_line(line, sizeof(line));
        kprintf("You typed: %s\n\n", line);
        __asm__ volatile("hlt");
    }
}
