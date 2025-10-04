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
    
    print_box("System Info", "Terminmal OS v1.0");
    print_centered("=== Welcome to Terminmal OS ===");
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

    if (ata_init() == 0) {
        print_str("ATA disk detected\n");
        
        // Test: Read first sector
        uint8_t* sector_buffer = kmalloc(512);
        if (sector_buffer) {
            print_str("Testing disk read...\n");
            
            int result = disk_read_sectors(0, 1, sector_buffer);
            if (result == 0) {
                print_str("Disk read successful!\n");
                
                // Show first 16 bytes
                print_str("First 16 bytes: ");
                for (int i = 0; i < 16; i++) {
                    kprintf("%x ", sector_buffer[i]);
                }
                print_str("\n");
            } else {
                print_str("Disk read FAILED\n");
            }
            kfree(sector_buffer);
        }
    } else {
        print_str("No ATA disk found\n");
    }
    
    // After the disk read test, add:
    if (fat32_init(0) == 0) {
        print_str("FAT32 filesystem mounted\n");
    } else {
        print_str("Failed to mount FAT32\n");
    }

    print_str("Boot complete!\n");

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
            print_str("ls       - list files in root directory\n");
            print_str("cat      - display file contents\n");
            print_str("hexdump  - show hex dump of file\n");
            print_str("fileinfo - show file information\n");
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
        else if (strcmp(line, "ls") == 0) {
            fat32_file_info_t files[32];
            int count = fat32_list_directory("/", files, 32);
            
            if (count < 0) {
                print_str("Failed to read directory\n");
            } else if (count == 0) {
                print_str("Empty directory\n");
            } else {
                kprintf("Found %d files:\n", count);
                print_line();
                
                for (int i = 0; i < count; i++) {
                    if (files[i].is_directory) {
                        kprintf("[DIR]  %s\n", files[i].name);
                    } else {
                        kprintf("[FILE] %s %u bytes\n", files[i].name, files[i].size);
                    }
                }
            }
        }
        else if (strncmp(line, "cat ", 4) == 0) {
            const char* filename = line + 4;
            
            if (!fat32_file_exists(filename)) {
                kprintf("File not found: %s\n", filename);
            } else {
                uint32_t size = fat32_get_file_size(filename);
                
                if (size == 0) {
                    print_str("Empty file\n");
                } else if (size > 4096) {
                    print_str("File too large (max 4KB for display)\n");
                } else {
                    uint8_t* buffer = kmalloc(size + 1);
                    if (!buffer) {
                        print_str("Out of memory\n");
                    } else {
                        int bytes = fat32_read_file(filename, buffer, size);
                        if (bytes < 0) {
                            print_str("Failed to read file\n");
                        } else {
                            buffer[bytes] = '\0';
                            print_str("=== File Contents ===\n");
                            print_str((char*)buffer);
                            print_str("\n=== End ===\n");
                        }
                        kfree(buffer);
                    }
                }
            }
        }
        else if (strncmp(line, "hexdump ", 8) == 0) {
            const char* filename = line + 8;
            
            if (!fat32_file_exists(filename)) {
                kprintf("File not found: %s\n", filename);
            } else {
                uint32_t size = fat32_get_file_size(filename);
                uint32_t display_size = (size > 256) ? 256 : size;
                
                uint8_t* buffer = kmalloc(display_size);
                if (!buffer) {
                    print_str("Out of memory\n");
                } else {
                    int bytes = fat32_read_file(filename, buffer, display_size);
                    if (bytes < 0) {
                        print_str("Failed to read file\n");
                    } else {
                        kprintf("=== Hex Dump (first %d bytes) ===\n", bytes);
                        
                        for (int i = 0; i < bytes; i += 16) {
                            kprintf("%x: ", i);
                            
                            // Hex values
                            for (int j = 0; j < 16 && i + j < bytes; j++) {
                                kprintf("%x ", buffer[i + j]);
                            }
                            
                            print_str(" | ");
                            
                            // ASCII representation
                            for (int j = 0; j < 16 && i + j < bytes; j++) {
                                char c = buffer[i + j];
                                if (c >= 32 && c <= 126) {
                                    print_char(c);
                                } else {
                                    print_char('.');
                                }
                            }
                            
                            print_char('\n');
                        }
                    }
                    kfree(buffer);
                }
            }
        }
        else if (strncmp(line, "fileinfo ", 9) == 0) {
            const char* filename = line + 9;
            
            if (!fat32_file_exists(filename)) {
                kprintf("File not found: %s\n", filename);
            } else {
                uint32_t size = fat32_get_file_size(filename);
                kprintf("File: %s\n", filename);
                kprintf("Size: %u bytes (%u KB)\n", size, size / 1024);
            }
        }
        else if (strcmp(line, "diskinfo") == 0) {
            uint8_t* buffer = kmalloc(512);
            if (!buffer) {
                print_str("Out of memory\n");
            } else {
                // Read boot sector
                if (disk_read_sectors(0, 1, buffer) == 0) {
                    print_str("=== Boot Sector (LBA 0) ===\n");
                    
                    // Check for FAT32 signature
                    if (buffer[510] == 0x55 && buffer[511] == 0xAA) {
                        print_str("Valid boot signature found!\n");
                    } else {
                        kprintf("Invalid signature: %x %x\n", buffer[510], buffer[511]);
                    }
                    
                    // Show OEM name
                    print_str("OEM: ");
                    for (int i = 3; i < 11; i++) {
                        print_char(buffer[i]);
                    }
                    print_str("\n");
                    
                    // Show bytes per sector
                    uint16_t bytes_per_sector = *(uint16_t*)(buffer + 11);
                    kprintf("Bytes/Sector: %d\n", bytes_per_sector);
                    
                    // Show sectors per cluster
                    uint8_t sectors_per_cluster = buffer[13];
                    kprintf("Sectors/Cluster: %d\n", sectors_per_cluster);
                    
                    // Show reserved sectors
                    uint16_t reserved = *(uint16_t*)(buffer + 14);
                    kprintf("Reserved sectors: %d\n", reserved);
                    
                    // Show number of FATs
                    uint8_t num_fats = buffer[16];
                    kprintf("Number of FATs: %d\n", num_fats);
                    
                    // Check FS type
                    print_str("FS Type: ");
                    for (int i = 82; i < 90; i++) {
                        print_char(buffer[i]);
                    }
                    print_str("\n");
                    
                    // Show first 32 bytes in hex
                    print_str("\nFirst 32 bytes:\n");
                    for (int i = 0; i < 32; i++) {
                        kprintf("%x ", buffer[i]);
                        if ((i + 1) % 16 == 0) print_str("\n");
                    }
                } else {
                    print_str("Failed to read boot sector\n");
                }
                kfree(buffer);
            }
        }
        else if (strncmp(line, "readsector ", 11) == 0) {
            uint32_t lba = kstr_to_uint32(line + 11);
            uint8_t* buffer = kmalloc(512);
            
            if (!buffer) {
                print_str("Out of memory\n");
            } else {
                kprintf("Reading sector %d...\n", lba);
                
                if (disk_read_sectors(lba, 1, buffer) == 0) {
                    print_str("Success! First 64 bytes:\n");
                    for (int i = 0; i < 64; i++) {
                        kprintf("%x ", buffer[i]);
                        if ((i + 1) % 16 == 0) print_str("\n");
                    }
                } else {
                    print_str("Read failed!\n");
                }
                kfree(buffer);
            }
        }
        else if (strcmp(line, "fat32info") == 0) {
            uint8_t* buffer = kmalloc(512);
            if (!buffer) {
                print_str("Out of memory\n");
            } else {
                if (disk_read_sectors(0, 1, buffer) == 0) {
                    fat32_boot_sector_t* bs = (fat32_boot_sector_t*)buffer;
                    
                    print_str("=== FAT32 Boot Sector ===\n");
                    kprintf("Bytes/Sector: %d\n", bs->bytes_per_sector);
                    kprintf("Sectors/Cluster: %d\n", bs->sectors_per_cluster);
                    kprintf("Reserved: %d\n", bs->reserved_sectors);
                    kprintf("FATs: %d\n", bs->num_fats);
                    kprintf("FAT Size: %u\n", bs->fat_size_32);
                    kprintf("Root Cluster: %u\n", bs->root_cluster);
                    
                    uint32_t fat_start = bs->reserved_sectors;
                    uint32_t data_start = fat_start + (bs->num_fats * bs->fat_size_32);
                    uint32_t root_lba = data_start + ((bs->root_cluster - 2) * bs->sectors_per_cluster);
                    
                    kprintf("Data starts: %u\n", data_start);
                    kprintf("Root LBA: %u\n", root_lba);
                }
                kfree(buffer);
            }
        }
        else {
            kprintf("Unknown command: %s\n", line);
        }
    }
}
