#include "sys/shell.h"
#include "lib/print.h"
#include "drivers/keyboard.h"
#include "lib/string.h"
#include "drivers/fat32.h"
#include "drivers/memory.h"
#include "drivers/timer.h"
#include "drivers/heap.h"
#include "sys/editor.h"
#include "drivers/disk.h"
#include "drivers/ata.h"
#include "lib/string_utils.h"
#include "sys/system.h"
#include "sys/script.h"
#include "lib/compiler.h"
#include "core/exceptions.h"
#include "net/net.h"
#include "drivers/rtc.h"
#include "sys/sysinfo.h"
#include "sys/process.h"

#define MAX_TEST_ALLOCS 16
static void *test_allocs[MAX_TEST_ALLOCS];
static uint64_t test_alloc_sizes[MAX_TEST_ALLOCS];
static int test_alloc_count = 0;

static void cmd_help(void);
static void cmd_crash(const char *what);
static void cmd_ping(const char *args);
static void cmd_download(const char *args);
static void cmd_ls(void);
static void cmd_cat(const char *filename);
static void cmd_mount(const char *which);
static int run_program(const char *line, int report_missing);
static void cmd_programs(void);
int shell_execute_command(const char* line);

void shell_run(void)
{
    char line[128];

    while (1)
    {
        char cwd[256];
        cwd[0] = 0;
        fat32_get_current_directory(cwd, sizeof(cwd));
        if (print_get_col() != 0) print_str("\n");   // never start the prompt mid-line
        statusbar_update(1);
        print_shell_prompt(OS_USER "@" OS_HOSTNAME, cwd[0] ? cwd : "/");
        get_line(line, sizeof(line));
        shell_execute_command(line);
    }
}

static void cmd_help(void)
{
    print_info("Available commands:\n");
    print_str("\n=== Files ===\n");
    print_str("ls                 - list current directory\n");
    print_str("cat <file>         - display file (max 1 MB)\n");
    print_str("write <file> <txt> - write text to file\n");
    print_str("touch <file>       - create empty file\n");
    print_str("rm <file>          - delete file\n");
    print_str("mkdir <dir>        - create directory\n");
    print_str("cd <dir>           - change directory\n");
    print_str("pwd                - print working directory\n");
    print_str("tree               - list current directory\n");
    print_str("hexdump <file>     - hex dump of a file\n");
    print_str("fileinfo <file>    - file name and size\n");
    print_str("edit <file>        - open text editor\n");
    print_str("\n=== Programs and Scripts ===\n");
    print_str("<program> [args]   - run a user-mode program (e.g. hello, snake, primes)\n");
    print_str("run <file> [args]  - run a program by file name\n");
    print_str("programs           - list the installed programs\n");
    print_str("sh <script>        - run a script file\n");
    print_str("compile <file>     - compile and run a C file (tiny subset)\n");
    print_str("\n=== System ===\n");
    print_str("help               - show this message\n");
    print_str("sysinfo            - this machine at a glance (also: neofetch)\n");
    print_str("dmesg              - full boot log, including driver messages\n");
    print_str("clear              - clear screen\n");
    print_str("echo <text>        - print text\n");
    print_str("uptime             - seconds since boot\n");
    print_str("date [-u]          - current date and time (IST, or UTC with -u)\n");
    print_str("sleep <seconds>    - wait\n");
    print_str("status             - uptime and allocation count\n");
    print_str("reboot             - reboot system\n");
    print_str("\n=== Memory and Disk (debug) ===\n");
    print_str("meminfo            - heap statistics\n");
    print_str("alloc              - allocate a physical frame\n");
    print_str("malloc <bytes>     - allocate heap memory (test)\n");
    print_str("free               - free last test allocation\n");
    print_str("freeidx <n>        - free n-th test allocation\n");
    print_str("listptr            - list test allocations\n");
    print_str("mount [ata|ram]    - show or switch the disk the files live on\n");
    print_str("diskinfo           - boot sector of the current disk\n");
    print_str("readsector <lba>   - dump a raw sector\n");
    print_str("fat32info          - FAT32 volume parameters\n");
    print_str("crash <kind>       - trigger a CPU exception on purpose\n");
    print_str("                     (div0 ud gp pf null stack int3 irq panic)\n");
    print_str("\n=== Network ===\n");
    print_str("ifconfig           - show MAC, IP settings and packet counters\n");
    print_str("dhcp               - get an IP address from the network's DHCP server\n");
    print_str("nettest            - send an ARP request to the gateway, wait for the reply\n");
    print_str("ping <ip> [count]  - send ICMP echo requests (default 4), e.g. ping 10.0.2.2\n");
    print_str("download <url> [file] - download HTTP/HTTPS URL to FAT32\n");
    print_str("netdebug <on|off>  - print a line for every received frame\n");
    print_str("\n=== Appearance ===\n");
    print_str("theme <name>       - change color theme\n");
    print_str("themes             - list available themes\n");
    print_str("demo               - show themed message examples\n");
}

/* "71 B", "31.7 KiB", "2.4 MiB" */
static void format_size(char *out, size_t size, uint32_t bytes)
{
    if (bytes < 1024)
        k_snprintf(out, size, "%u B", bytes);
    else if (bytes < 1024 * 1024)
        k_snprintf(out, size, "%u.%u KiB", bytes / 1024, (bytes % 1024) * 10 / 1024);
    else
        k_snprintf(out, size, "%u.%u MiB", bytes >> 20, (bytes % (1024 * 1024)) * 10 / (1024 * 1024));
}

static void cmd_ls(void)
{
    // fat32_file_info_t is 268 bytes: 32 of them must not live on the boot stack.
    fat32_file_info_t *files = kmalloc(32 * sizeof(fat32_file_info_t));
    if (!files)
    {
        print_str("Out of memory\n");
        return;
    }
    int count = fat32_list_directory(files, 32);

    if (count < 0)
    {
        print_error("Cannot read the directory (no filesystem mounted?)");
    }
    else if (count == 0)
    {
        print_str("(empty)\n");
    }
    else
    {
        int dirs = 0, regular = 0;
        uint32_t total = 0;
        char size[16], name[20];
        for (int i = 0; i < count; i++)
        {
            if (files[i].is_directory)
            {
                k_snprintf(name, sizeof(name), "%s/", files[i].name);
                print_accent("  ");
                k_snprintf(size, sizeof(size), "%-16s", name);
                print_accent(size);
                print_str("     <DIR>\n");
                dirs++;
            }
            else
            {
                format_size(size, sizeof(size), files[i].size);
                kprintf("  %-16s%10s\n", files[i].name, size);
                regular++;
                total += files[i].size;
            }
        }
        format_size(size, sizeof(size), total);
        kprintf("\n  %d file%s, %d director%s, %s\n", regular, regular == 1 ? "" : "s",
                dirs, dirs == 1 ? "y" : "ies", size);
    }
    kfree(files);
}

/* `dmesg`: everything the kernel printed during boot, including the driver
 * messages the boot screen hides. */
static void cmd_dmesg(void)
{
    size_t len;
    const char *log = print_get_bootlog(&len);
    print_batch_begin();
    for (size_t i = 0; i < len; i++)
        print_char(log[i]);
    print_batch_end();
    if (len && log[len - 1] != '\n')
        print_str("\n");
}

static void cmd_mount(const char *which)
{
    while (*which == ' ') which++;
    if (!*which)
    {
        kprintf("Files are on: %s\n", disk_name());
        if (disk_ramdisk_size())
            kprintf("RAM disk loaded: %u MiB (changes are lost at reboot)\n",
                    (uint32_t)(disk_ramdisk_size() >> 20));
        else
            print_str("No RAM disk loaded\n");
        return;
    }
    disk_kind_t kind;
    if (strcmp(which, "ata") == 0)
        kind = DISK_ATA;
    else if (strcmp(which, "ram") == 0)
        kind = DISK_RAM;
    else
    {
        print_str("Usage: mount [ata|ram]\n");
        return;
    }
    if (kind == DISK_RAM && !disk_ramdisk_size())
    {
        print_error("No RAM disk was loaded at boot");
        return;
    }
    if (kind == DISK_ATA && ata_init() != 0)
    {
        print_error("No ATA disk found");
        return;
    }
    disk_kind_t old = disk_selected();
    disk_select(kind);
    if (fat32_init(0) != 0)
    {
        print_error("No FAT32 volume on that disk; keeping the old one");
        disk_select(old);
        if (old != DISK_NONE) fat32_init(0);
        return;
    }
    fat32_change_directory("/");
    kprintf("Files are now on: %s\n", disk_name());
}

#define CAT_MAX   (1024 * 1024)   // largest file `cat` will show
#define CAT_CHUNK 4096

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
        else if (size > CAT_MAX)
        {
            kprintf("File too large to display (%u KB, max %u KB)\n", size / 1024, CAT_MAX / 1024);
        }
        else
        {
            uint8_t *buffer = kmalloc(size);
            char *chunk = kmalloc(CAT_CHUNK + 8);
            if (!buffer || !chunk)
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
                    // Print in chunks; drop CR (web pages use CRLF), expand tabs,
                    // and show other control bytes (binary files) as '.'.
                    print_str("=== File Contents ===\n");
                    int n = 0;
                    for (int i = 0; i < bytes; i++)
                    {
                        uint8_t c = buffer[i];
                        if (c == '\r') continue;
                        if (c == '\t') { chunk[n++] = ' '; chunk[n++] = ' '; chunk[n++] = ' '; chunk[n++] = ' '; }
                        else if (c == '\n' || (c >= 32 && c != 127)) chunk[n++] = (char)c;
                        else chunk[n++] = '.';
                        if (n >= CAT_CHUNK) { chunk[n] = '\0'; print_str(chunk); n = 0; }
                    }
                    chunk[n] = '\0';
                    print_str(chunk);
                    print_str("\n=== End === (Shift+Up/Down scrolls)\n");
                }
            }
            kfree(chunk);
            kfree(buffer);
        }
    }
}

// Deliberately trigger a CPU exception, to see the panic screen / test handlers.
static void stack_overflow(int depth)
{
    volatile char pad[512];
    pad[0] = (char)depth;
    stack_overflow(depth + 1);
    pad[1] = 0; // keeps the recursion from being turned into a loop
}

static void cmd_crash(const char *what)
{
    if (strcmp(what, "div0") == 0)
    {
        __asm__ volatile("xor %%edx, %%edx; mov $1, %%eax; xor %%ecx, %%ecx; div %%ecx" ::: "eax", "ecx", "edx");
    }
    else if (strcmp(what, "ud") == 0)
    {
        __asm__ volatile("ud2");
    }
    else if (strcmp(what, "gp") == 0)
    {
        *(volatile uint64_t *)0x8000000000000000ULL = 1; // non-canonical address
    }
    else if (strcmp(what, "pf") == 0)
    {
        *(volatile uint32_t *)0x5000000 = 1; // unmapped page
    }
    else if (strcmp(what, "null") == 0)
    {
        *(volatile uint32_t *)0 = 1;
    }
    else if (strcmp(what, "stack") == 0)
    {
        stack_overflow(0);
    }
    else if (strcmp(what, "int3") == 0)
    {
        __asm__ volatile("int3"); // non-fatal: reports and continues
        print_str("Returned from breakpoint\n");
    }
    else if (strcmp(what, "irq") == 0)
    {
        __asm__ volatile("int $0x2f"); // vector with no handler installed
    }
    else if (strcmp(what, "panic") == 0)
    {
        kpanic("manual panic from the shell");
    }
    else
    {
        print_str("Usage: crash <div0|ud|gp|pf|null|stack|int3|irq|panic>\n");
    }
}

int shell_execute_command(const char* line) {
    while (*line == ' ') line++;
    if (*line == '\0')
    {
        return 0;   // empty line: just show a new prompt
    }
    else if (strcmp(line, "help") == 0)
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
    else if (strcmp(line, "sysinfo") == 0 || strcmp(line, "neofetch") == 0)
    {
        sysinfo_print();
    }
    else if (strcmp(line, "dmesg") == 0)
    {
        cmd_dmesg();
    }
    else if (strcmp(line, "programs") == 0)
    {
        cmd_programs();
    }
    else if (strcmp(line, "uptime") == 0)
    {
        uint32_t seconds = get_seconds();
        kprintf("Uptime: %d seconds\n", seconds);
    }
    else if (strcmp(line, "date") == 0 || strcmp(line, "date -u") == 0)
    {
        rtc_time_t t;
        int utc = line[4] != '\0';
        if (rtc_read(&t) != 0) {
            print_str("date: real-time clock unreadable\n");
        } else {
            if (!utc) rtc_add_minutes(&t, RTC_LOCAL_OFFSET_MIN);
            kprintf("%d-%s%d-%s%d %s%d:%s%d:%s%d %s\n", t.year,
                    t.month < 10 ? "0" : "", t.month, t.day < 10 ? "0" : "", t.day,
                    t.hour < 10 ? "0" : "", t.hour, t.minute < 10 ? "0" : "", t.minute,
                    t.second < 10 ? "0" : "", t.second, utc ? "UTC" : RTC_LOCAL_TZ_NAME);
        }
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
            kprintf("Allocated frame at 0x%lx\n", (uint64_t)frame);
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
                kprintf("Allocated %d bytes at 0x%lx [slot %d]\n",
                        size, (uint64_t)ptr, test_alloc_count);
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
            kprintf("Freeing 0x%lx [slot %d]\n",
                    (uint64_t)test_allocs[test_alloc_count], test_alloc_count);
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
            kprintf("Freeing 0x%lx [slot %d]\n", (uint64_t)test_allocs[idx], idx);
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
                kprintf("[%d] 0x%lx (%d bytes)\n",
                        i, (uint64_t)test_allocs[i], test_alloc_sizes[i]);
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
        kprintf("Physical:    %d MB usable RAM\n", (uint32_t)(get_total_memory() >> 20));
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
                        kprintf("%04x: ", i);

                        // Hex values
                        for (int j = 0; j < 16 && i + j < bytes; j++)
                        {
                            kprintf("%02X ", buffer[i + j]);
                        }
                        for (int j = bytes - i; j < 16; j++)
                            print_str("   ");   // keep the text column aligned

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
    else if (strcmp(line, "mount") == 0 || strncmp(line, "mount ", 6) == 0)
    {
        cmd_mount(line[5] ? line + 6 : "");
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
                    kprintf("Invalid signature: 0x%02X 0x%02X\n", buffer[510], buffer[511]);
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
                    kprintf("%02X ", buffer[i]);
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
                    kprintf("%02X ", buffer[i]);
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

            fat32_create_file(filename);   // fails harmlessly if it already exists
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
        fat32_file_info_t *files = kmalloc(32 * sizeof(fat32_file_info_t));
        if (!files)
        {
            print_str("Out of memory\n");
        }
        else
        {
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
            kfree(files);
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
    else if (strncmp(line, "compile ", 8) == 0) {
        cmd_compile(line+8);
    }
    else if (strncmp(line, "crash ", 6) == 0)
    {
        cmd_crash(line + 6);
    }
    else if (strcmp(line, "ifconfig") == 0)
    {
        net_print_ifconfig();
    }
    else if (strcmp(line, "dhcp") == 0)
    {
        net_configure();
    }
    else if (strcmp(line, "nettest") == 0)
    {
        net_selftest();
    }
    else if (strcmp(line, "ping") == 0 || strncmp(line, "ping ", 5) == 0)
    {
        cmd_ping(line + 4);
    }
    else if (strcmp(line, "download") == 0 || strncmp(line, "download ", 9) == 0)
    {
        cmd_download(line + 8);
    }
    else if (strncmp(line, "netdebug ", 9) == 0)
    {
        const char *arg = line + 9;
        if (strcmp(arg, "on") == 0)
        {
            net_set_debug(1);
            print_str("Network debug on\n");
        }
        else if (strcmp(arg, "off") == 0)
        {
            net_set_debug(0);
            print_str("Network debug off\n");
        }
        else
        {
            print_str("Usage: netdebug <on|off>\n");
        }
    }
    else if (strncmp(line, "run ", 4) == 0)
    {
        run_program(line + 4, 1);
    }
    else if (!run_program(line, 0))
    {
        kprintf("Unknown command: %s\n", line);
    }
    return 0;
}

/* Read `name` or `name.elf` from the current directory into a kmalloc'd buffer. */
static uint8_t *read_program_file(const char *name, uint32_t *size)
{
    char path[32];
    k_snprintf(path, sizeof(path), "%s", name);
    if (!fat32_file_exists(path))
    {
        k_snprintf(path, sizeof(path), "%s.elf", name);
        if (!fat32_file_exists(path)) return 0;
    }
    uint32_t n = fat32_get_file_size(path);
    if (n == 0 || n == 0xFFFFFFFF) return 0;
    uint8_t *data = kmalloc(n);
    if (!data) return 0;
    if (fat32_read_file(path, data, n) < 0)
    {
        kfree(data);
        return 0;
    }
    *size = n;
    return data;
}

/* Find a program: in the current directory first, then in the root of the RAM
 * disk, which works as the system's program directory even while the files
 * are on another disk. Returns the whole file (kfree it) or 0. */
static uint8_t *find_program(const char *name, uint32_t *size)
{
    uint8_t *data = read_program_file(name, size);
    if (data || disk_selected() == DISK_RAM || !disk_ramdisk_size()) return data;

    char cwd[256] = "/";
    fat32_get_current_directory(cwd, sizeof(cwd));
    disk_kind_t old = disk_selected();
    disk_select(DISK_RAM);
    if (fat32_init(0) == 0)
    {
        fat32_change_directory("/");
        data = read_program_file(name, size);
    }
    disk_select(old);
    if (old != DISK_NONE)
    {
        fat32_init(0);
        fat32_change_directory(cwd);
    }
    return data;
}

/* `programs`: the .ELF files in the current directory and on the RAM disk. */
static void list_elf_files(const char *where)
{
    fat32_file_info_t *list = kmalloc(32 * sizeof(fat32_file_info_t));
    if (!list) return;
    int count = fat32_list_directory(list, 32), shown = 0;
    for (int i = 0; i < count; i++)
    {
        size_t len = strlen(list[i].name);
        if (list[i].is_directory || len < 5 || strcmp(list[i].name + len - 4, ".ELF") != 0) continue;
        if (!shown++) kprintf("%s:\n", where);
        char name[16], padded[20];
        k_snprintf(name, sizeof(name), "%s", list[i].name);
        name[len - 4] = '\0';
        for (char *p = name; *p; p++)
            if (*p >= 'A' && *p <= 'Z') *p += 32;
        k_snprintf(padded, sizeof(padded), "%-12s", name);
        print_str("  ");
        print_accent(padded);
        kprintf("%4u KiB\n", (list[i].size + 1023) / 1024);
    }
    kfree(list);
}

static void cmd_programs(void)
{
    list_elf_files("In this directory");
    if (disk_selected() != DISK_RAM && disk_ramdisk_size())
    {
        char cwd[256] = "/";
        fat32_get_current_directory(cwd, sizeof(cwd));
        disk_kind_t old = disk_selected();
        disk_select(DISK_RAM);
        if (fat32_init(0) == 0)
        {
            fat32_change_directory("/");
            list_elf_files("On the RAM disk");
        }
        disk_select(old);
        if (old != DISK_NONE)
        {
            fat32_init(0);
            fat32_change_directory(cwd);
        }
    }
    print_str("Type a program's name (with arguments) to run it in user mode.\n");
}

/* Run a user program: `line` is "name arg1 arg2 ...". Returns 0 if there is no
 * such program (and `report_missing` is 0), 1 otherwise. */
static int run_program(const char *line, int report_missing)
{
    static char args[256];
    char *argv[16];
    int argc = 0;

    size_t n = strlen(line);
    if (n >= sizeof(args)) n = sizeof(args) - 1;
    memcpy(args, line, n);
    args[n] = '\0';
    for (char *p = args; *p && argc < 16; )
    {
        while (*p == ' ') *p++ = '\0';
        if (!*p) break;
        argv[argc++] = p;
        while (*p && *p != ' ') p++;
    }
    if (argc == 0)
    {
        if (report_missing) print_str("Usage: run <program> [arguments]\n");
        return 1;
    }

    uint32_t size;
    uint8_t *image = find_program(argv[0], &size);
    if (!image)
    {
        if (!report_missing) return 0;
        kprintf("%s: no such program\n", argv[0]);
        return 1;
    }

    int code = process_run(image, size, argc, argv);
    kfree(image);
    if (code < 0)
    {
        char msg[96];
        k_snprintf(msg, sizeof(msg), "%s: %s", argv[0], process_error_text(code));
        print_error(msg);
    }
    else if (code != 0 && code != EXIT_INTERRUPTED && code < 128)
    {
        kprintf("[%s exited with code %d]\n", argv[0], code);
    }
    return 1;
}


/* ping <ip> [count] */
static void cmd_ping(const char *args)
{
    while (*args == ' ') args++;

    char host[20];
    size_t n = 0;
    while (args[n] && args[n] != ' ' && n < sizeof(host) - 1) { host[n] = args[n]; n++; }
    host[n] = '\0';
    args += n;
    while (*args == ' ') args++;

    uint8_t ip[4];
    if (n == 0 || net_parse_ip(host, ip) != 0)
    {
        print_str("Usage: ping <ip> [count]   (dotted IPv4 address, e.g. ping 10.0.2.2)\n");
        print_str("There is no DNS client yet, so host names are not supported.\n");
        return;
    }

    uint32_t count = 4;
    if (*args)
    {
        if (*args < '0' || *args > '9')
        {
            print_str("Usage: ping <ip> [count]\n");
            return;
        }
        count = kstr_to_uint32(args);
        if (count == 0) count = 4;
        if (count > 1000) count = 1000;
    }

    net_ping(ip, count);
}


/* download <url> [file] */
static void cmd_download(const char *args)
{
    while (*args == ' ') args++;
    if (!*args) {
        print_str("Usage: download <url> [file]\n");
        return;
    }

    char url[192];
    size_t n = 0;
    while (args[n] && args[n] != ' ' && n < sizeof(url) - 1) {
        url[n] = args[n];
        n++;
    }
    url[n] = '\0';
    args += n;
    while (*args == ' ') args++;

    char filename[32];
    const char *out = 0;
    if (*args) {
        size_t m = 0;
        while (args[m] && args[m] != ' ' && m < sizeof(filename) - 1) {
            filename[m] = args[m];
            m++;
        }
        filename[m] = '\0';
        out = filename;
    }

    if (strncmp(url, "https://", 8) == 0) net_download_https(url, out);
    else net_download_http(url, out);
}
