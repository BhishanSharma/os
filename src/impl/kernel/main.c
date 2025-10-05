#include "print.h"
#include "keyboard.h"
#include "idt.h"
#include "string.h"
#include "timer.h"
#include "memory.h"
#include "string_utils.h"
#include "paging.h"
#include "heap.h"
#include "fat32.h"
#include "ata.h"
#include "elf.h"
#include "editor.h"
#include "shell.h"

extern void irq0_stub();
extern void irq1_stub();
void pic_remap();

extern void reboot();
extern void memory_init(uint64_t mem_upper);

void kernel_main() {
    print_set_theme(THEME_CYBERPUNK);
    print_clear();

    print_line();
    print_centered("=== Welcome to Terminmal OS ===");
    print_line();

    // Initialize IDT and PIC
    idt_init();
    pic_remap();

    // Set keyboard IRQ (IRQ1) handler
    idt_set_entry(0x21, irq1_stub, 0x8E);
    idt_set_entry(0x20, irq0_stub, 0x8E);

    // Initialize keyboard and enable interrupts
    init_keyboard();
    timer_init();
    memory_init(512 * 1024);

    uint64_t kernel_start = 0x100000;
    uint64_t kernel_end   = 0x120000;
    uint64_t heap_start   = 0x200000;
    uint64_t heap_size    = 1024*1024;

    paging_init(kernel_start, kernel_end, heap_start, heap_size);
    heap_init(heap_start, heap_size);

    if (ata_init() == 0) {
        print_str("ATA disk detected\n");
    } else {
        print_str("No ATA disk found\n");
    }
    
    // After the disk read test, add:
    if (fat32_init(0) == 0) {
        print_str("FAT32 filesystem mounted\n");
    } else {
        print_str("Failed to mount FAT32\n");
    }
    
    fat32_change_directory("/");

    print_str("Boot complete!\n");

    __asm__ volatile("sti");

    shell_run();
}
