#include "lib/print.h"
#include "drivers/keyboard.h"
#include "core/idt.h"
#include "lib/string.h"
#include "drivers/timer.h"
#include "drivers/memory.h"
#include "lib/string_utils.h"
#include "drivers/paging.h"
#include "drivers/heap.h"
#include "drivers/fat32.h"
#include "drivers/ata.h"
#include "sys/editor.h"
#include "sys/shell.h"
#include "drivers/pic.h"
#include "drivers/rtl8139.h"
#include "core/gdt.h"
#include "lib/serial.h"
#include "net/net.h"

extern void irq0_stub();
extern void irq1_stub();

extern void memory_init(uint64_t mem_upper);
extern void irq_nic_stub();

// Exported by targets/x86_64/linker.ld: the real extent of the kernel image
// (.text, .rodata, .data, .bss and the boot stack), page aligned at the end.
extern char kernel_start[];
extern char kernel_end[];

// The heap lives right above the 1 MiB reserved for the kernel image. The
// linker script asserts that the image never grows into it.
#define HEAP_START 0x200000ULL
#define HEAP_SIZE  (1024 * 1024)

void kernel_main() {
    serial_init();      // mirror all output to COM1 from the very first line
    gdt_init();         // GDT + TSS (own stack for double faults)

    print_set_theme(THEME_CYBERPUNK);
    print_clear();

    print_line();
    print_centered("=== Welcome to Terminmal OS ===");
    print_line();

    // Initialize IDT (installs the CPU exception handlers) and PIC
    idt_init();
    pic_remap();

    // Set keyboard IRQ (IRQ1) handler
    idt_set_entry(0x21, irq1_stub, 0x8E);
    idt_set_entry(0x20, irq0_stub, 0x8E);

    // Initialize keyboard and enable interrupts
    init_keyboard();
    timer_init();
    memory_init(512 * 1024);

    paging_init((uint64_t)kernel_start, (uint64_t)kernel_end, HEAP_START, HEAP_SIZE);
    heap_init(HEAP_START, HEAP_SIZE);

    expand_scrollback();
    
    if (rtl8139_probe_init() == 0) {
        idt_set_entry(0x20 + rtl8139_get_irq(), irq_nic_stub, 0x8E);
        print_str("[NET] NIC driver installed\n");
        net_init();
    }

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
