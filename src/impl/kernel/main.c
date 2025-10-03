#include "print.h"
#include "keyboard.h"
#include "idt.h"
#include "string.h"
#include "timer.h"

extern void irq0_stub();
extern void irq1_stub();
void pic_remap();

void kernel_main() {
    print_clear();

    // Initialize IDT and PIC
    idt_init();
    pic_remap();

    // Set keyboard IRQ (IRQ1) handler
    idt_set_entry(0x21, irq1_stub, 0x8E); // present, ring0, interrupt gate
    idt_set_entry(0x20, irq0_stub, 0x8E); // present, ring0, interrupt gate

    // Initialize keyboard and enable interrupts
    init_keyboard();
    timer_init();
    __asm__ volatile("sti");

    char line[128];

    while (1) {
        print_str("> ");           // shell prompt
        get_line(line, sizeof(line));

        if (strcmp(line, "help") == 0) {
            print_str("Available commands:\n");
            print_str("help  - show this message\n");
            print_str("echo  - print text\n");
            print_str("clear - clear screen\n");
            print_str("uptime - show uptime in seconds\n");
        } 
        else if (strncmp(line, "echo ", 5) == 0) {
            kprintf("%s\n", line + 5);
        }
        else if (strcmp(line, "clear") == 0) {
            print_clear();
        }
        else if (strcmp(line, "uptime") == 0) {
            uint32_t seconds = get_seconds();
            kprintf("Uptime: %d seconds\n", seconds);
        }
        else {
            kprintf("Unknown command: %s\n", line);
        }
    }
}
