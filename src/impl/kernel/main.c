#include "print.h"
#include "keyboard.h"
#include "idt.h"
#include "string.h"
#include "timer.h"
#include "memory.h"
#include "string_utils.h"
#include "paging.h"
#include "heap.h"

extern void irq0_stub();
extern void irq1_stub();
void pic_remap();

extern void reboot();
extern void memory_init(uint64_t mem_upper);

// Test allocation tracking (max 16 allocations for demo)
#define MAX_TEST_ALLOCS 16
static void* test_allocs[MAX_TEST_ALLOCS];
static uint64_t test_alloc_sizes[MAX_TEST_ALLOCS];
static int test_alloc_count = 0;

void kernel_main() {
    print_clear();
    
    print_box("System Info", "MyOS v1.0");
    print_centered("=== Welcome to MyOS ===");
    print_line();
    kprintf("Binary: %b\n", 0xFF00AA55);
    kprintf("64-bit: %lu bytes\n", heap_get_total());
    kprintf("Hex64: %lx\n", 0x123456789ABCDEF0);

    // Initialize IDT and PIC
    idt_init();
    pic_remap();

    // Set keyboard IRQ (IRQ1) handler
    idt_set_entry(0x21, irq1_stub, 0x8E);
    idt_set_entry(0x20, irq0_stub, 0x8E);

    // Initialize keyboard and enable interrupts
    init_keyboard();
    timer_init();
    memory_init(64 * 1024);

    uint64_t kernel_start = 0x100000;
    uint64_t kernel_end   = 0x120000;
    uint64_t heap_start   = 0x200000;
    uint64_t heap_size    = 1024*1024;

    paging_init(kernel_start, kernel_end, heap_start, heap_size);
    heap_init(heap_start, heap_size);

    __asm__ volatile("sti");

    char line[128];

    while (1) {
        print_str("> ");
        get_line(line, sizeof(line));

        if (strcmp(line, "help") == 0) {
            print_str("Available commands:\n");
            print_str("help    - show this message\n");
            print_str("echo    - print text\n");
            print_str("clear   - clear screen\n");
            print_str("uptime  - show uptime in seconds\n");
            print_str("reboot  - reboot the system\n");
            print_str("alloc   - allocate a frame\n");
            print_str("malloc  - test heap allocation (malloc <size>)\n");
            print_str("free    - free last malloc'd block\n");
            print_str("freeidx - free specific allocation (freeidx <n>)\n");
            print_str("meminfo - show heap memory statistics\n");
            print_str("listptr - list all test allocations\n");
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
        else if (strcmp(line, "reboot") == 0) {
            print_str("Rebooting...\n");
            reboot();
        }
        else if (strcmp(line, "status") == 0) {
            uint32_t sec = get_seconds();
            uint32_t frames = 0; // You'd get this from your frame allocator
            kprintf("Uptime: %d sec, Test allocations: %d\n", sec, test_alloc_count);
        }
        else if (strcmp(line, "alloc") == 0) {
            void* frame = alloc_frame();
            if (frame) {
                kprintf("Allocated frame at 0x%x\n", frame);
            } else {
                print_str("Out of memory!\n");
            }
        }
        else if (strncmp(line, "malloc ", 7) == 0) {
            uint32_t size = kstr_to_uint32(line + 7);
            if (size == 0) {
                print_str("Invalid size\n");
            } else if (test_alloc_count >= MAX_TEST_ALLOCS) {
                print_str("Test allocation limit reached (max 16)\n");
            } else {
                void* ptr = kmalloc(size);
                if (ptr) {
                    test_allocs[test_alloc_count] = ptr;
                    test_alloc_sizes[test_alloc_count] = size;
                    kprintf("Allocated %d bytes at 0x%x [slot %d]\n", 
                            size, ptr, test_alloc_count);
                    test_alloc_count++;
                } else {
                    print_str("kmalloc failed - out of heap memory!\n");
                }
            }
        }
        else if (strcmp(line, "free") == 0) {
            if (test_alloc_count == 0) {
                print_str("No allocations to free\n");
            } else {
                test_alloc_count--;
                kprintf("Freeing 0x%x [slot %d]\n", 
                        test_allocs[test_alloc_count], test_alloc_count);
                kfree(test_allocs[test_alloc_count]);
                test_allocs[test_alloc_count] = 0;
            }
        }
        else if (strncmp(line, "freeidx ", 8) == 0) {
            uint32_t idx = kstr_to_uint32(line + 8);
            if (idx >= test_alloc_count) {
                print_str("Invalid index\n");
            } else if (test_allocs[idx] == 0) {
                print_str("Already freed\n");
            } else {
                kprintf("Freeing 0x%x [slot %d]\n", test_allocs[idx], idx);
                kfree(test_allocs[idx]);
                test_allocs[idx] = 0;
            }
        }
        else if (strcmp(line, "listptr") == 0) {
            print_str("Test allocations:\n");
            for (int i = 0; i < test_alloc_count; i++) {
                if (test_allocs[i]) {
                    kprintf("[%d] 0x%x (%d bytes)\n", 
                            i, test_allocs[i], test_alloc_sizes[i]);
                } else {
                    kprintf("[%d] (freed)\n", i);
                }
            }
        }
        else if (strcmp(line, "meminfo") == 0) {
            uint64_t total = heap_get_total();
            uint64_t used = heap_get_used();
            uint64_t free = heap_get_free();
            uint64_t allocs = heap_get_allocations();
            
            print_str("=== Heap Memory Info ===\n");
            kprintf("Total:       %d bytes (%d KB)\n", total, total / 1024);
            kprintf("Used:        %d bytes (%d KB)\n", used, used / 1024);
            kprintf("Free:        %d bytes (%d KB)\n", free, free / 1024);
            kprintf("Allocations: %d active\n", allocs);
            kprintf("Test slots:  %d/%d used\n", test_alloc_count, MAX_TEST_ALLOCS);
        }
        else if (strncmp(line, "sleep ", 6) == 0) {
            uint32_t s = kstr_to_uint32(line + 6);
            sleep(s * 1000);
            print_str("Done sleeping\n");
        }
        else {
            kprintf("Unknown command: %s\n", line);
        }
    }
}
