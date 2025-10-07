#include "sys/shell.h"
#include "lib/print.h"
#include "drivers/keyboard.h"
#include "lib/string.h"
#include "drivers/fat32.h"
#include "drivers/memory.h"
#include "drivers/timer.h"
#include "drivers/heap.h"
#include "sys/editor.h"
#include "drivers/ata.h"
#include "lib/string_utils.h"
#include "sys/system.h"
#include "sys/script.h"

#define MAX_TEST_ALLOCS 16
static void *test_allocs[MAX_TEST_ALLOCS];
static uint64_t test_alloc_sizes[MAX_TEST_ALLOCS];
static int test_alloc_count = 0;

static void cmd_help(void);
static void cmd_ls(void);
static void cmd_cat(const char *filename);
int shell_execute_command(const char* line);

void shell_run(void)
{
    char line[128];

    while (1)
    {
        char cwd[256];
        fat32_get_current_directory(cwd, sizeof(cwd));
        kprintf("%s\n", cwd);
        print_prompt("> ");
        get_line(line, sizeof(line));
        shell_execute_command(line);
        print_newLine();
    }
}

static void cmd_help(void)
{
    print_info("Available commands:\n");
    print_str("\n=== Appearance ===\n");
    print_str("theme <name> - change color theme\n");
    print_str("themes       - list available themes\n");
    print_str("demo         - show themed message examples\n");
    print_str("\n=== File System Commands ===\n");
    print_str("ls       - list files\n");
    print_str("cat      - display file\n");
    print_str("write    - write file (write file.txt content)\n");
    print_str("touch    - create empty file\n");
    print_str("rm       - delete file\n");
    print_str("mkdir    - create directory\n");
    print_str("cd       - change directory\n");
    print_str("pwd      - print working directory\n");
    print_str("tree     - show directory tree\n");
    print_str("\n=== Program Execution ===\n");
    print_str("exec     - execute ELF program\n");
    print_str("load     - load ELF into memory\n");
    print_str("elfinfo  - show ELF file info\n");
    print_str("\n=== System Commands ===\n");
    print_str("help     - show this message\n");
    print_str("clear    - clear screen\n");
    print_str("uptime   - show uptime\n");
    print_str("meminfo  - show memory stats\n");
    print_str("reboot   - reboot system\n");
}

static void cmd_ls(void)
{
    fat32_file_info_t files[32];
    int count = fat32_list_directory(files, 32);

    if (count < 0)
    {
        print_str("Failed to read directory\n");
    }
    else if (count == 0)
    {
        print_str("Empty directory\n");
    }
    else
    {
        kprintf("Found %d files:\n", count);
        print_line();

        for (int i = 0; i < count; i++)
        {
            if (files[i].is_directory)
            {
                kprintf("[DIR]  %s\n", files[i].name);
            }
            else
            {
                kprintf("[FILE] %s %u bytes\n", files[i].name, files[i].size);
            }
        }
    }
}

static void cmd_cat(const char *filename)
{
    if (!fat32_file_exists(filename))
    {
        kprintf("File not found: %s\n", filename);
    }
    else
    {
        uint32_t size = fat32_get_file_size(filename);

        if (size == 0)
        {
            print_str("Empty file\n");
        }
        else if (size > 4096)
        {
            print_str("File too large (max 4KB for display)\n");
        }
        else
        {
            uint8_t *buffer = kmalloc(size + 1);
            if (!buffer)
            {
                print_str("Out of memory\n");
            }
            else
            {
                int bytes = fat32_read_file(filename, buffer, size);
                if (bytes < 0)
                {
                    print_str("Failed to read file\n");
                }
                else
                {
                    buffer[bytes] = '\0';
                    print_str("=== File Contents ===\n");
                    print_str((char *)buffer);
                    print_str("\n=== End ===\n");
                }
                kfree(buffer);
            }
        }
    }
}

int shell_execute_command(const char* line) {
    if (strcmp(line, "help") == 0)
    {
        cmd_help();
    }
    else if (strncmp(line, "echo ", 5) == 0)
    {
        kprintf("%s\n", line + 5);
    }
    else if (strcmp(line, "clear") == 0)
    {
        print_clear();
    }
    else if (strcmp(line, "uptime") == 0)
    {
        uint32_t seconds = get_seconds();
        kprintf("Uptime: %d seconds\n", seconds);
    }
    else if (strcmp(line, "reboot") == 0)
    {
        print_str("Rebooting...\n");
        reboot();
    }
    else if (strcmp(line, "status") == 0)
    {
        uint32_t sec = get_seconds();
        uint32_t frames = 0; // You'd get this from your frame allocator
        kprintf("Uptime: %d sec, Test allocations: %d\n", sec, test_alloc_count);
    }
    else if (strcmp(line, "alloc") == 0)
    {
        void *frame = alloc_frame();
        if (frame)
        {
            kprintf("Allocated frame at 0x%x\n", frame);
        }
        else
        {
            print_str("Out of memory!\n");
        }
    }
    else if (strncmp(line, "malloc ", 7) == 0)
    {
        uint32_t size = kstr_to_uint32(line + 7);
        if (size == 0)
        {
            print_str("Invalid size\n");
        }
        else if (test_alloc_count >= MAX_TEST_ALLOCS)
        {
            print_str("Test allocation limit reached (max 16)\n");
        }
        else
        {
            void *ptr = kmalloc(size);
            if (ptr)
            {
                test_allocs[test_alloc_count] = ptr;
                test_alloc_sizes[test_alloc_count] = size;
                kprintf("Allocated %d bytes at 0x%x [slot %d]\n",
                        size, ptr, test_alloc_count);
                test_alloc_count++;
            }
            else
            {
                print_str("kmalloc failed - out of heap memory!\n");
            }
        }
    }
    else if (strcmp(line, "free") == 0)
    {
        if (test_alloc_count == 0)
        {
            print_str("No allocations to free\n");
        }
        else
        {
            test_alloc_count--;
            kprintf("Freeing 0x%x [slot %d]\n",
                    test_allocs[test_alloc_count], test_alloc_count);
            kfree(test_allocs[test_alloc_count]);
            test_allocs[test_alloc_count] = 0;
        }
    }
    else if (strncmp(line, "freeidx ", 8) == 0)
    {
        uint32_t idx = kstr_to_uint32(line + 8);
        if (idx >= test_alloc_count)
        {
            print_str("Invalid index\n");
        }
        else if (test_allocs[idx] == 0)
        {
            print_str("Already freed\n");
        }
        else
        {
            kprintf("Freeing 0x%x [slot %d]\n", test_allocs[idx], idx);
            kfree(test_allocs[idx]);
            test_allocs[idx] = 0;
        }
    }
    else if (strcmp(line, "listptr") == 0)
    {
        print_str("Test allocations:\n");
        for (int i = 0; i < test_alloc_count; i++)
        {
            if (test_allocs[i])
            {
                kprintf("[%d] 0x%x (%d bytes)\n",
                        i, test_allocs[i], test_alloc_sizes[i]);
            }
            else
            {
                kprintf("[%d] (freed)\n", i);
            }
        }
    }
    else if (strcmp(line, "meminfo") == 0)
    {
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
    else if (strncmp(line, "sleep ", 6) == 0)
    {
        uint32_t s = kstr_to_uint32(line + 6);
        sleep(s * 1000);
        print_str("Done sleeping\n");
    }
    else if (strcmp(line, "ls") == 0)
    {
        cmd_ls();
    }
    else if (strncmp(line, "cat ", 4) == 0)
    {
        const char *filename = line + 4;
        cmd_cat(filename);
    }
    else if (strncmp(line, "hexdump ", 8) == 0)
    {
        const char *filename = line + 8;

        if (!fat32_file_exists(filename))
        {
            kprintf("File not found: %s\n", filename);
        }
        else
        {
            uint32_t size = fat32_get_file_size(filename);
            uint32_t display_size = (size > 256) ? 256 : size;

            uint8_t *buffer = kmalloc(display_size);
            if (!buffer)
            {
                print_str("Out of memory\n");
            }
            else
            {
                int bytes = fat32_read_file(filename, buffer, display_size);
                if (bytes < 0)
                {
                    print_str("Failed to read file\n");
                }
                else
                {
                    kprintf("=== Hex Dump (first %d bytes) ===\n", bytes);

                    for (int i = 0; i < bytes; i += 16)
                    {
                        kprintf("%x: ", i);

                        // Hex values
                        for (int j = 0; j < 16 && i + j < bytes; j++)
                        {
                            kprintf("%x ", buffer[i + j]);
                        }

                        print_str(" | ");

                        // ASCII representation
                        for (int j = 0; j < 16 && i + j < bytes; j++)
                        {
                            char c = buffer[i + j];
                            if (c >= 32 && c <= 126)
                            {
                                print_char(c);
                            }
                            else
                            {
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
    else if (strncmp(line, "fileinfo ", 9) == 0)
    {
        const char *filename = line + 9;

        if (!fat32_file_exists(filename))
        {
            kprintf("File not found: %s\n", filename);
        }
        else
        {
            uint32_t size = fat32_get_file_size(filename);
            kprintf("File: %s\n", filename);
            kprintf("Size: %u bytes (%u KB)\n", size, size / 1024);
        }
    }
    else if (strcmp(line, "diskinfo") == 0)
    {
        uint8_t *buffer = kmalloc(512);
        if (!buffer)
        {
            print_str("Out of memory\n");
        }
        else
        {
            // Read boot sector
            if (disk_read_sectors(0, 1, buffer) == 0)
            {
                print_str("=== Boot Sector (LBA 0) ===\n");

                // Check for FAT32 signature
                if (buffer[510] == 0x55 && buffer[511] == 0xAA)
                {
                    print_str("Valid boot signature found!\n");
                }
                else
                {
                    kprintf("Invalid signature: %x %x\n", buffer[510], buffer[511]);
                }

                // Show OEM name
                print_str("OEM: ");
                for (int i = 3; i < 11; i++)
                {
                    print_char(buffer[i]);
                }
                print_str("\n");

                // Show bytes per sector
                uint16_t bytes_per_sector = *(uint16_t *)(buffer + 11);
                kprintf("Bytes/Sector: %d\n", bytes_per_sector);

                // Show sectors per cluster
                uint8_t sectors_per_cluster = buffer[13];
                kprintf("Sectors/Cluster: %d\n", sectors_per_cluster);

                // Show reserved sectors
                uint16_t reserved = *(uint16_t *)(buffer + 14);
                kprintf("Reserved sectors: %d\n", reserved);

                // Show number of FATs
                uint8_t num_fats = buffer[16];
                kprintf("Number of FATs: %d\n", num_fats);

                // Check FS type
                print_str("FS Type: ");
                for (int i = 82; i < 90; i++)
                {
                    print_char(buffer[i]);
                }
                print_str("\n");

                // Show first 32 bytes in hex
                print_str("\nFirst 32 bytes:\n");
                for (int i = 0; i < 32; i++)
                {
                    kprintf("%x ", buffer[i]);
                    if ((i + 1) % 16 == 0)
                        print_str("\n");
                }
            }
            else
            {
                print_str("Failed to read boot sector\n");
            }
            kfree(buffer);
        }
    }
    else if (strncmp(line, "readsector ", 11) == 0)
    {
        uint32_t lba = kstr_to_uint32(line + 11);
        uint8_t *buffer = kmalloc(512);

        if (!buffer)
        {
            print_str("Out of memory\n");
        }
        else
        {
            kprintf("Reading sector %d...\n", lba);

            if (disk_read_sectors(lba, 1, buffer) == 0)
            {
                print_str("Success! First 64 bytes:\n");
                for (int i = 0; i < 64; i++)
                {
                    kprintf("%x ", buffer[i]);
                    if ((i + 1) % 16 == 0)
                        print_str("\n");
                }
            }
            else
            {
                print_str("Read failed!\n");
            }
            kfree(buffer);
        }
    }
    else if (strcmp(line, "fat32info") == 0)
    {
        uint8_t *buffer = kmalloc(512);
        if (!buffer)
        {
            print_str("Out of memory\n");
        }
        else
        {
            if (disk_read_sectors(0, 1, buffer) == 0)
            {
                fat32_boot_sector_t *bs = (fat32_boot_sector_t *)buffer;

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
    else if (strncmp(line, "write ", 6) == 0)
    {
        char *space = line + 6;
        while (*space && *space != ' ')
            space++;
        if (*space == ' ')
        {
            *space = '\0';
            const char *filename = line + 6;
            const char *content = space + 1;

            int result = fat32_write_file(filename, (uint8_t *)content, strlen(content));
            if (result < 0)
            {
                kprintf("Failed to write file: %d\n", result);
            }
            else
            {
                kprintf("Wrote %d bytes to %s\n", result, filename);
            }
        }
        else
        {
            print_str("Usage: write <filename> <content>\n");
        }
    }
    else if (strncmp(line, "touch ", 6) == 0)
    {
        const char *filename = line + 6;

        if (fat32_create_file(filename) == 0)
        {
            kprintf("Created file: %s\n", filename);
        }
        else
        {
            print_str("Failed to create file\n");
        }
    }
    else if (strncmp(line, "rm ", 3) == 0)
    {
        const char *filename = line + 3;
        if (fat32_delete_file(filename) == 0)
        {
            kprintf("Deleted: %s\n", filename);
        }
        else
        {
            print_str("Failed to delete file\n");
        }
    }
    else if (strncmp(line, "mkdir ", 6) == 0)
    {
        const char *dirname = line + 6;
        if (fat32_mkdir(dirname) == 0)
        {
            kprintf("Created directory: %s\n", dirname);
        }
        else
        {
            print_str("Failed to create directory\n");
        }
    }
    else if (strncmp(line, "cd ", 3) == 0)
    {
        const char *path = line + 3;
        if (fat32_change_directory(path) == 0)
        {
            char cwd[256];
            fat32_get_current_directory(cwd, sizeof(cwd));
            kprintf("Changed to: %s\n", cwd);
        }
        else
        {
            print_str("Directory not found\n");
        }
    }
    else if (strcmp(line, "pwd") == 0)
    {
        char cwd[256];
        fat32_get_current_directory(cwd, sizeof(cwd));
        kprintf("Current directory: %s\n", cwd);
    }
    else if (strcmp(line, "tree") == 0)
    {
        // Show directory tree (simple version)
        print_str("Directory tree:\n");
        fat32_file_info_t files[32];
        int count = fat32_list_directory_ex(NULL, files, 32);

        for (int i = 0; i < count; i++)
        {
            if (files[i].is_directory)
            {
                kprintf("  [DIR]  %s/\n", files[i].name);
            }
            else
            {
                kprintf("  [FILE] %s\n", files[i].name);
            }
        }
    }
    else if (strncmp(line, "theme ", 6) == 0)
    {
        const char *theme_name = line + 6;

        if (strcmp(theme_name, "dracula") == 0)
        {
            print_set_theme(THEME_DRACULA);
            print_success("Theme changed to Dracula");
        }
        else if (strcmp(theme_name, "nord") == 0)
        {
            print_set_theme(THEME_NORD);
            print_success("Theme changed to Nord");
        }
        else if (strcmp(theme_name, "monokai") == 0)
        {
            print_set_theme(THEME_MONOKAI);
            print_success("Theme changed to Monokai");
        }
        else if (strcmp(theme_name, "gruvbox") == 0)
        {
            print_set_theme(THEME_GRUVBOX);
            print_success("Theme changed to Gruvbox");
        }
        else if (strcmp(theme_name, "solarized") == 0)
        {
            print_set_theme(THEME_SOLARIZED);
            print_success("Theme changed to Solarized Dark");
        }
        else if (strcmp(theme_name, "matrix") == 0)
        {
            print_set_theme(THEME_MATRIX);
            print_success("Theme changed to Matrix");
        }
        else if (strcmp(theme_name, "cyberpunk") == 0)
        {
            print_set_theme(THEME_CYBERPUNK);
            print_success("Theme changed to Cyberpunk");
        }
        else if (strcmp(theme_name, "default") == 0)
        {
            print_set_theme(THEME_DEFAULT);
            print_success("Theme changed to Default");
        }
        else
        {
            print_error("Unknown theme");
            print_info("Available themes:");
            print_str("  dracula, nord, monokai, gruvbox\n");
            print_str("  solarized, matrix, cyberpunk, default\n");
        }
    }
    else if (strcmp(line, "themes") == 0)
    {
        print_info("Available color themes:");
        print_str("\n");

        print_set_color(PRINT_COLOR_MAGENTA, PRINT_COLOR_BLACK);
        print_str("  dracula    - ");
        print_set_color(PRINT_COLOR_LIGHT_GRAY, PRINT_COLOR_BLACK);
        print_str("Purple and cyan on dark background\n");

        print_set_color(PRINT_COLOR_LIGHT_CYAN, PRINT_COLOR_DARK_GRAY);
        print_str("  nord       - ");
        print_set_color(PRINT_COLOR_LIGHT_GRAY, PRINT_COLOR_DARK_GRAY);
        print_str("Arctic, north-bluish color palette\n");

        print_set_color(PRINT_COLOR_LIGHT_GREEN, PRINT_COLOR_BLACK);
        print_str("  monokai    - ");
        print_set_color(PRINT_COLOR_LIGHT_GRAY, PRINT_COLOR_BLACK);
        print_str("Vibrant colors on black\n");

        print_set_color(PRINT_COLOR_BROWN, PRINT_COLOR_BLACK);
        print_str("  gruvbox    - ");
        print_set_color(PRINT_COLOR_LIGHT_GRAY, PRINT_COLOR_BLACK);
        print_str("Retro groove warm colors\n");

        print_set_color(PRINT_COLOR_CYAN, PRINT_COLOR_DARK_GRAY);
        print_str("  solarized  - ");
        print_set_color(PRINT_COLOR_LIGHT_GRAY, PRINT_COLOR_DARK_GRAY);
        print_str("Precision colors for readability\n");

        print_set_color(PRINT_COLOR_LIGHT_GREEN, PRINT_COLOR_BLACK);
        print_str("  matrix     - ");
        print_set_color(PRINT_COLOR_GREEN, PRINT_COLOR_BLACK);
        print_str("Classic green terminal\n");

        print_set_color(PRINT_COLOR_MAGENTA, PRINT_COLOR_BLACK);
        print_str("  cyberpunk  - ");
        print_set_color(PRINT_COLOR_CYAN, PRINT_COLOR_BLACK);
        print_str("Neon cyan and magenta\n");

        print_set_color(PRINT_COLOR_WHITE, PRINT_COLOR_BLUE);
        print_str("  default    - ");
        print_str("Classic blue terminal\n");
    }
    else if (strcmp(line, "demo") == 0)
    {
        print_info("This is an info message");
        print_success("This is a success message");
        print_warning("This is a warning message");
        print_error("This is an error message");
        print_str("\n");
        print_box("Demo Box", "This is a themed box!");
    }
    else if (strncmp(line, "edit ", 5) == 0)
    {
        const char *filename = line + 5;
        editor_open(filename);
    }
    else if (strncmp(line, "sh ", 3) == 0) {
        const char* filename = line + 3;
        script_run(filename);
    }
    else
    {
        kprintf("Unknown command: %s\n", line);
    }
    return 0;
}