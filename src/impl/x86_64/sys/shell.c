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
#include "sys/users.h"
#include "sys/task.h"
#include "drivers/pci.h"
#include "drivers/wifi.h"
#include "drivers/usb.h"
#include "drivers/touchpad.h"
#include "drivers/iwlwifi.h"
#include "drivers/hda.h"
#include "drivers/nvme.h"
#include "drivers/part.h"
#include "drivers/fatvol.h"
#include "sys/vfs.h"
#include "net/tcp.h"
#include "drivers/msi.h"
#include "sys/smp.h"
#include "drivers/display.h"
#include "lib/fbcon.h"

#define MAX_TEST_ALLOCS 16
static void *test_allocs[MAX_TEST_ALLOCS];
static uint64_t test_alloc_sizes[MAX_TEST_ALLOCS];
static int test_alloc_count = 0;

static void cmd_help(void);
static void cmd_crash(const char *what);
static void cmd_ping(const char *args);
static void cmd_download(const char *args);
static void cmd_ls(const char *path);
static void cmd_cat(const char *filename);
static void cmd_mount(const char *which);
static void cmd_umount(const char *path);
static int run_program(const char *line, int report_missing);
static int need_root(const char *what);
static void report_exit(const char *name, int pid, int code, int background);
static void reap_background(void);
static void cmd_ps(int programs_only);
static void cmd_kill(const char *arg);
static void cmd_fg(const char *arg);
static void cmd_programs(void);
static void cmd_lspci(void);
static int cmd_sound(const char *line);
static int play_file(const char *name);
static void cmd_resolution(const char *arg);
static void cmd_font(const char *arg);
int shell_execute_command(const char* line);
static int kernel_command(const char *line, int run_programs);

/* The built-in kernel shell: used when the user-mode shell (SHELL.ELF) is
 * missing or crashed. Returns at logout. */
static void kernel_shell(void)
{
    char line[128];
    while (1)
    {
        char cwd[256], who[32];
        reap_background();
        cwd[0] = 0;
        fat32_get_current_directory(cwd, sizeof(cwd));
        if (print_get_col() != 0) print_str("\n");   // never start the prompt mid-line
        statusbar_update(1);
        k_snprintf(who, sizeof(who), "%s@%s", user_current()->name, OS_HOSTNAME);
        print_shell_prompt(who, cwd[0] ? cwd : "/", user_is_root());
        get_line(line, sizeof(line));

        const char *cmd = line;
        while (*cmd == ' ') cmd++;
        if (strcmp(cmd, "logout") == 0 || strcmp(cmd, "exit") == 0)
        {
            if (users_logout())
            {
                kprintf("Back to %s.\n", user_current()->name);
                continue;
            }
            return;
        }
        shell_execute_command(line);
    }
}

/* Run the user-mode shell (SHELL.ELF from the root folder or the RAM disk) as
 * the logged-in user, in their home folder, until it exits. Returns its exit
 * code, or -1 if it could not be started. */
static int user_shell(void)
{
    char home[256] = "/";
    fat32_get_current_directory(home, sizeof(home));
    fat32_change_directory("/");          // only root can put programs here
    uint32_t size = 0;
    uint8_t *image = process_find_program("shell", &size);
    fat32_change_directory(home);
    if (!image) return -1;

    char name[] = "shell";
    char *argv[] = { name, 0 };
    int pid = process_spawn(image, size, 1, argv);
    kfree(image);
    if (pid < 0)
    {
        kprintf("Cannot start the shell: %s\n", process_error_text(pid));
        return -1;
    }
    int code = process_wait(pid, 1);
    process_end_all();                    // logout ends the session's programs
    return code;
}

/* pid 1: log a user in, run their shell, repeat. */
void shell_run(void)
{
    users_init();
    while (1)
    {
        users_login();
        int code = user_shell();
        if (code < 0 || code == EXIT_SEGFAULT || code == EXIT_ILLEGAL || code == EXIT_ARITHMETIC)
        {
            if (code >= 0)
                print_warning("The shell program crashed; continuing with the kernel's built-in shell.");
            else
                print_warning("No shell program (SHELL.ELF); using the kernel's built-in shell.");
            kernel_shell();
        }
        users_logout();
        reap_background();
        print_clear();
    }
}

/* Root-only shell commands call this first. */
static int need_root(const char *what)
{
    if (user_is_root()) return 1;
    char msg[96];
    k_snprintf(msg, sizeof(msg), "%s: only root can do that (try `su`)", what);
    print_error(msg);
    return 0;
}

static void print_fs_error(const char *what, const char *name, int result)
{
    if (result == FAT32_ERR_PERMISSION)
        kprintf("%s: %s: permission denied (you can change files in %s and /tmp)\n", what, name,
                user_current()->home);
    else
        kprintf("%s: %s: %s\n", what, name, vfs_error(result));
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
    print_str("<program> ... &    - run it in the background and keep using the shell\n");
    print_str("jobs / ps          - running programs / every task with its CPU time\n");
    print_str("fg [pid]           - bring a background program to the foreground\n");
    print_str("kill <pid>         - stop a program\n");
    print_str("sh <script>        - run a script file\n");
    print_str("compile <file>     - compile and run a C file (tiny subset)\n");
    print_str("\n=== Users ===\n");
    print_str("whoami / id        - who you are (name, uid, home folder)\n");
    print_str("users              - list the accounts\n");
    print_str("su [user]          - become another user (default root); exit to go back\n");
    print_str("passwd [user]      - change your password (root: anyone's)\n");
    print_str("useradd <name>     - create an account with a home folder (root)\n");
    print_str("userdel <name>     - delete an account, keeping its files (root)\n");
    print_str("logout / exit      - end the session, back to the login prompt\n");
    print_str("\n=== System ===\n");
    print_str("help               - show this message\n");
    print_str("sysinfo            - this machine at a glance (also: neofetch)\n");
    print_str("dmesg              - full boot log, including driver messages\n");
    print_str("lspci              - every device on the PCI bus, and its driver\n");
    print_str("lsusb              - the USB controller and what is plugged into it\n");
    print_str("touchpad           - find the I2C touchpad and show what it sends\n");
    print_str("clear              - clear screen\n");
    print_str("echo <text>        - print text\n");
    print_str("uptime             - seconds since boot\n");
    print_str("date [-u]          - current date and time (IST, or UTC with -u)\n");
    print_str("clock [local|utc]  - does the hardware clock hold local time (Windows) or UTC?\n");
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
    print_str("mount [ram|ata|nvme n] [dir] - mount a volume (no arguments: list mounts)\n");
    print_str("umount <dir>       - unmount a volume\n");
    print_str("nvme               - the NVMe drive and its partitions\n");
    print_str("diskinfo           - boot sector of the current disk\n");
    print_str("readsector <lba>   - dump a raw sector\n");
    print_str("fat32info          - FAT32 volume parameters\n");
    print_str("crash <kind>       - trigger a CPU exception on purpose\n");
    print_str("                     (div0 ud gp pf null stack int3 irq panic)\n");
    print_str("\n=== Sound ===\n");
    print_str("sound              - the sound card, codec and speaker/headphone paths\n");
    print_str("beep [hz] [ms]     - play a tone (default 880 Hz, 300 ms)\n");
    print_str("play <file.wav>    - play a WAV file (Ctrl+C stops), e.g. play /CHIME.WAV\n");
    print_str("volume [0-100]     - show or set the volume\n");
    print_str("\n=== Network ===\n");
    print_str("ifconfig           - show MAC, IP settings and packet counters\n");
    print_str("netstat            - TCP connections and listening ports\n");
    print_str("interrupts         - devices that interrupt (MSI/MSI-X) and how often\n");
    print_str("cpus               - the CPU cores and what each one runs\n");
    print_str("wifi               - Wi-Fi adapters in this machine and what they need\n");
    print_str("wifi scan          - list the Wi-Fi networks in range\n");
    print_str("wifi connect [n]   - join network number n of the scan (or give its name;\n");
    print_str("                     no argument: scan and choose). Asks the password once\n");
    print_str("wifi status        - the network joined, signal, address\n");
    print_str("wifi disconnect    - leave the network;  wifi forget - delete the saved one\n");
    print_str("dhcp               - get an IP address from the network's DHCP server\n");
    print_str("nettest            - send an ARP request to the gateway, wait for the reply\n");
    print_str("ping <ip> [count]  - send ICMP echo requests (default 4), e.g. ping 10.0.2.2\n");
    print_str("download <url> [file] - download HTTP/HTTPS URL to FAT32\n");
    print_str("netdebug <on|off>  - print a line for every received frame\n");
    print_str("\n=== Appearance ===\n");
    print_str("theme <name>       - change color theme\n");
    print_str("resolution [WxH]   - show or change the screen mode, e.g. resolution 1280x800\n");
    print_str("font [1-4|auto]    - text size (scale of the 8x16 font)\n");
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

static void cmd_ls(const char *path)
{
    // fat32_file_info_t is 268 bytes: these must not live on the boot stack.
    fat32_file_info_t *files = kmalloc(128 * sizeof(fat32_file_info_t));
    if (!files)
    {
        print_str("Out of memory\n");
        return;
    }
    while (*path == ' ') path++;
    int count = fat32_list_directory_ex(path, files, 128);

    if (count < 0)
    {
        kprintf("ls: %s: no such folder\n", *path ? path : ".");
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
        print_str("Mounted volumes:\n");
        vfs_print_mounts();
        return;
    }
    /* mount <ata|ram|nvme N> [folder] */
    char kind[8];
    int k = 0;
    while (*which && *which != ' ' && k < 7) kind[k++] = *which++;
    kind[k] = 0;
    while (*which == ' ') which++;
    int number = 0;
    if (!strcmp(kind, "nvme"))
        while (*which >= '0' && *which <= '9') number = number * 10 + (*which++ - '0');
    while (*which == ' ') which++;
    const char *at = *which ? which : !strcmp(kind, "nvme") ? "/NVME" : !strcmp(kind, "ram") ? "/RAM" : "/ATA";
    blockdev_t dev;
    if (!strcmp(kind, "ram"))
    {
        if (disk_open(DISK_RAM, &dev) != 0) { print_error("No RAM disk was loaded at boot"); return; }
    }
    else if (!strcmp(kind, "ata"))
    {
        if (ata_init() != 0 || disk_open(DISK_ATA, &dev) != 0) { print_error("No ATA disk found"); return; }
    }
    else if (!strcmp(kind, "nvme"))
    {
        char how[160];
        if (!nvme_present() && nvme_init(how, sizeof(how)) != 0)
        {
            kprintf("mount: %s\n", how);
            return;
        }
        static partition_t parts[16];
        int n = part_scan(nvme_partition_reader, parts, 16);
        if (number < 1 || number > n)
        {
            print_str("usage: mount nvme <partition> [folder]   (`nvme` lists the partitions)\n");
            return;
        }
        partition_t *p = &parts[number - 1];
        if (!p->fat32)
        {
            print_error("That partition is not FAT32 (NTFS cannot be read yet)");
            return;
        }
        if (disk_open_nvme_partition(p->start, p->sectors, p->ours, number, &dev) != 0) return;
    }
    else
    {
        print_str("usage: mount [ram|ata|nvme <n>] [folder]   (no arguments: list the mounts)\n");
        return;
    }
    int r = vfs_mount(at, &dev);
    if (r != 0)
    {
        kprintf("mount: %s\n", vfs_error(r));
        return;
    }
    char abs[64];
    vfs_normalize(at, abs, sizeof(abs));
    kprintf("Mounted %s at %s%s\n", dev.name, abs, dev.writable ? "" : " (read-only)");
}

static void cmd_umount(const char *path)
{
    while (*path == ' ') path++;
    int r = vfs_unmount(path);
    if (r != 0) kprintf("umount: %s: %s\n", path, r == FATV_BAD_NAME ? "cannot unmount /" : "not a mount point");
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
                if (bytes == FAT32_ERR_PERMISSION)
                {
                    kprintf("cat: %s: permission denied (only root can read it)\n", filename);
                }
                else if (bytes < 0)
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

static int kernel_command(const char *line, int run_programs) {
    while (*line == ' ') line++;
    if (*line == '\0')
    {
        return 1;   // empty line: just show a new prompt
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
    else if (users_command(line))
    {
        // whoami, id, users, useradd, userdel, passwd, su
    }
    else if (strcmp(line, "resolution") == 0 || strncmp(line, "resolution ", 11) == 0)
    {
        cmd_resolution(line + 10);
    }
    else if (strcmp(line, "font") == 0 || strncmp(line, "font ", 5) == 0)
    {
        cmd_font(line + 4);
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
    else if (strcmp(line, "ps") == 0)
    {
        cmd_ps(0);
    }
    else if (strcmp(line, "jobs") == 0)
    {
        cmd_ps(1);
    }
    else if (strncmp(line, "kill ", 5) == 0 || strcmp(line, "kill") == 0)
    {
        cmd_kill(line + 4);
    }
    else if (strncmp(line, "fg ", 3) == 0 || strcmp(line, "fg") == 0)
    {
        cmd_fg(line + 2);
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
    else if (strcmp(line, "clock") == 0 || strncmp(line, "clock ", 6) == 0)
    {
        const char *arg = line[5] ? line + 6 : "";
        if (strcmp(arg, "local") == 0 || strcmp(arg, "utc") == 0) {
            if (!need_root("clock")) return 1;
            rtc_set_local(arg[0] == 'l');
        } else if (arg[0]) {
            print_str("Usage: clock [local|utc]\n");
            return 1;
        }
        kprintf("The hardware clock holds %s.\n", rtc_is_local()
                ? "local time (" RTC_LOCAL_TZ_NAME "), as Windows keeps it. `clock utc` if the time is wrong"
                : "UTC, as Linux and VMs keep it. `clock local` if the time is wrong");
    }
    else if (strcmp(line, "reboot") == 0)
    {
        if (!need_root("reboot")) return 1;
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
    else if (strcmp(line, "ls") == 0 || strncmp(line, "ls ", 3) == 0)
    {
        cmd_ls(line[2] ? line + 3 : "");
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
        if (line[5] && !need_root("mount")) return 1;
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
            if (vfs_read_sectors(0, 1, buffer) == 0)
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

            if (vfs_read_sectors(lba, 1, buffer) == 0)
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
            if (vfs_read_sectors(0, 1, buffer) == 0)
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
                print_fs_error("write", filename, result);
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

        int result = fat32_create_file(filename);
        if (result == 0)
        {
            kprintf("Created file: %s\n", filename);
        }
        else
        {
            print_fs_error("touch", filename, result);
        }
    }
    else if (strncmp(line, "rm ", 3) == 0)
    {
        const char *filename = line + 3;
        int result = fat32_delete_file(filename);
        if (result == 0)
        {
            kprintf("Deleted: %s\n", filename);
        }
        else
        {
            print_fs_error("rm", filename, result);
        }
    }
    else if (strncmp(line, "mkdir ", 6) == 0)
    {
        const char *dirname = line + 6;
        int result = fat32_mkdir(dirname);
        if (result == 0)
        {
            kprintf("Created directory: %s\n", dirname);
        }
        else
        {
            print_fs_error("mkdir", dirname, result);
        }
    }
    else if (strcmp(line, "cd") == 0 || strncmp(line, "cd ", 3) == 0)
    {
        const char *path = line[2] ? line + 3 : "~";
        while (*path == ' ') path++;
        if (*path == '\0' || strcmp(path, "~") == 0) path = user_current()->home;   // home folder
        if (fat32_change_directory(path) != 0)
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
        if (!need_root("crash")) return 1;
        cmd_crash(line + 6);
    }
    else if (strcmp(line, "wifi") == 0)
    {
        wifi_print_status();
    }
    else if (strcmp(line, "wifi start") == 0)
    {
        if (!need_root("wifi start")) return 1;
        iwl_start();
    }
    else if (strcmp(line, "wifi scan") == 0)
    {
        if (!need_root("wifi scan")) return 1;
        iwl_scan();
    }
    else if (strncmp(line, "wifi connect", 12) == 0)
    {
        if (!need_root("wifi connect")) return 1;
        const char *name = line + 12;
        while (*name == ' ') name++;
        /* No name: scan and choose by number. A number: from the last scan. */
        char choice[16];
        int number = 0, digits = *name != 0;
        for (const char *q = name; *q; q++)
        {
            if (*q < '0' || *q > '9') { digits = 0; break; }
            number = number * 10 + (*q - '0');
        }
        if (!*name)
        {
            if (iwl_scan() != 0 || !iwl_scan_pick(1)) return 1;
            print_str("Network number: ");
            if (users_read_line(choice, sizeof(choice), 1) <= 0) return 1;
            number = 0;
            for (const char *q = choice; *q >= '0' && *q <= '9'; q++) number = number * 10 + (*q - '0');
            digits = 1;
        }
        if (digits)
        {
            if (!iwl_scan_pick(1) && iwl_scan() != 0) return 1;
            if (!iwl_scan_pick(number))
            {
                kprintf("No network number %d in the list (`wifi scan` shows it).\n", number);
                return 1;
            }
            name = iwl_scan_pick(number);
            kprintf("Network: %s\n", name);
        }
        char password[64] = "";
        if (!iwl_has_saved(name))
        {
            print_str("Password (empty for an open network): ");
            if (users_read_line(password, sizeof(password), 0) < 0) return 1;
        }
        iwl_connect(name, password);
        memset(password, 0, sizeof(password));
    }
    else if (strcmp(line, "wifi status") == 0)
    {
        iwl_print_status();
    }
    else if (strcmp(line, "wifi disconnect") == 0)
    {
        if (!need_root("wifi disconnect")) return 1;
        iwl_disconnect();
    }
    else if (strcmp(line, "wifi forget") == 0)
    {
        if (!need_root("wifi forget")) return 1;
        iwl_forget();
    }
    else if (strcmp(line, "sound") == 0 || strncmp(line, "beep", 4) == 0 || strncmp(line, "play ", 5) == 0 ||
             strncmp(line, "volume", 6) == 0)
    {
        if ((line[0] == 'b' && line[4] && line[4] != ' ') || (line[0] == 'v' && line[6] && line[6] != ' '))
        {
            kprintf("%s: unknown command (`help` lists them, `programs` the programs)\n", line);
            return 1;
        }
        cmd_sound(line);
    }
    else if (strncmp(line, "umount ", 7) == 0)
    {
        if (!need_root("umount")) return 1;
        cmd_umount(line + 7);
    }
    else if (strcmp(line, "nvme") == 0)
    {
        nvme_print_info();
    }
    else if (strcmp(line, "lspci") == 0)
    {
        cmd_lspci();
    }
    else if (strcmp(line, "lsusb") == 0)
    {
        usb_print_devices();
    }
    else if (strcmp(line, "touchpad") == 0)
    {
        touchpad_diagnose();
    }
    else if (strcmp(line, "cpus") == 0)
    {
        smp_print();
    }
    else if (strcmp(line, "interrupts") == 0)
    {
        msi_print();
    }
    else if (strcmp(line, "netstat") == 0)
    {
        tcp_print_sockets();
    }
    else if (strcmp(line, "ifconfig") == 0)
    {
        net_print_ifconfig();
    }
    else if (strcmp(line, "dhcp") == 0)
    {
        if (!need_root("dhcp")) return 1;
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
        if (!need_root("netdebug")) return 1;
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
    else if (!run_programs)
    {
        return 0;
    }
    else if (strncmp(line, "run ", 4) == 0)
    {
        run_program(line + 4, 1);
    }
    else if (!run_program(line, 0))
    {
        kprintf("Unknown command: %s\n", line);
    }
    return 1;
}

int shell_execute_command(const char *line)
{
    kernel_command(line, 1);
    return 0;
}

/* For user programs (kcommand(), e.g. the user-mode shell): the commands
 * built into the kernel, run as the calling program's user. Programs are left
 * to the caller, and so are the commands that change the shell itself. */
int shell_kernel_command(const char *line)
{
    while (*line == ' ') line++;
    static const char *const not_here[] = { "su", "logout", "exit", "cd", "run", "fg" };
    for (size_t i = 0; i < sizeof(not_here) / sizeof(not_here[0]); i++)
    {
        size_t n = strlen(not_here[i]);
        if (strncmp(line, not_here[i], n) == 0 && (line[n] == 0 || line[n] == ' ')) return 0;
    }
    return kernel_command(line, 0);
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
    char cwd[256] = "/";
    fat32_get_current_directory(cwd, sizeof(cwd));
    const char *sys = vfs_system_dir();
    if (strcmp(cwd, sys) != 0) list_elf_files("In this directory");
    char here[256];
    kstrncpy(here, cwd, sizeof(here));
    if (fat32_change_directory(sys) == 0)
    {
        list_elf_files(strcmp(sys, "/") ? "In /SYS (the RAM disk)" : "On the RAM disk");
        fat32_change_directory(here);
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
    // A trailing "&" (separate or attached to the last word) runs it in the background.
    int background = 0;
    if (argc > 0 && strcmp(argv[argc - 1], "&") == 0)
    {
        background = 1;
        argc--;
    }
    else if (argc > 0)
    {
        size_t len = strlen(argv[argc - 1]);
        if (len > 1 && argv[argc - 1][len - 1] == '&')
        {
            argv[argc - 1][len - 1] = '\0';
            background = 1;
        }
    }
    if (argc == 0)
    {
        if (report_missing) print_str("Usage: run <program> [arguments] [&]\n");
        return 1;
    }

    uint32_t size;
    uint8_t *image = process_find_program(argv[0], &size);
    if (!image)
    {
        if (!report_missing) return 0;
        kprintf("%s: no such program\n", argv[0]);
        return 1;
    }

    int pid = process_spawn(image, size, argc, argv);
    kfree(image);
    if (pid < 0)
    {
        char msg[96];
        k_snprintf(msg, sizeof(msg), "%s: %s", argv[0], process_error_text(pid));
        print_error(msg);
        return 1;
    }
    if (background)
    {
        kprintf("[%d] %s running in the background\n", pid, argv[0]);
        return 1;
    }
    report_exit(argv[0], pid, process_wait(pid, 1), 0);
    return 1;
}

/* Explain how a program ended (nothing for a normal exit 0). */
static void report_exit(const char *name, int pid, int code, int background)
{
    if (background)
        kprintf("[%d] Done: %s (exit code %d)\n", pid, name, code);
    else if (code == EXIT_KILLED)
        kprintf("[%d] %s: killed\n", pid, name);
    else if (code != 0 && code != EXIT_INTERRUPTED && code < 128)
        kprintf("[%s exited with code %d]\n", name, code);
}

/* Before each prompt: collect background programs that have ended. */
static void reap_background(void)
{
    for (int i = 0; i < MAX_TASKS; i++)
    {
        task_t *t = task_at(i);
        if (t->state != TASK_ZOMBIE || !t->is_user || t->parent != 1) continue;
        char name[16];
        k_snprintf(name, sizeof(name), "%s", t->name);
        int pid = t->pid, code = t->exit_code;
        task_reap(t);
        if (code == EXIT_KILLED)
            kprintf("[%d] Killed: %s\n", pid, name);
        else
            report_exit(name, pid, code, 1);
    }
}

static const char *state_name(const task_t *t)
{
    switch (t->state)
    {
        case TASK_READY:    return t == task_current() ? "running" : "ready";
        case TASK_SLEEPING: return "sleeping";
        case TASK_ZOMBIE:   return "done";
        default:            return "?";
    }
}

/* ps: every task; jobs: only programs. */
static void cmd_ps(int programs_only)
{
    if (programs_only)
        kprintf("  %5s  %-9s %-9s %s\n", "PID", "USER", "STATE", "PROGRAM");
    else
        kprintf("  %5s  %-9s %-9s %8s %4s  %s\n", "PID", "USER", "STATE", "CPU", "CORE", "NAME");
    int shown = 0;
    for (int i = 0; i < MAX_TASKS; i++)
    {
        task_t *t = task_at(i);
        if (t->state == TASK_FREE || (programs_only && !t->is_user)) continue;
        const char *user = t->is_user ? t->user.name : "kernel";
        if (programs_only)
            kprintf("  %5d  %-9s %-9s %s%s\n", t->pid, user, state_name(t), t->name,
                    t->pid == process_foreground() ? "  (foreground)" : "");
        else
        {
            char core[8] = "-";
            if (t->running_on >= 0) k_snprintf(core, sizeof(core), "%d", t->running_on);
            kprintf("  %5d  %-9s %-9s %5u.%02us %4s  %s\n", t->pid, user, state_name(t),
                    t->ticks / 100, t->ticks % 100, core, t->name);
        }
        shown++;
    }
    if (programs_only && shown == 0) print_str("  (no programs running; start one with `name &`)\n");
}

static int parse_pid(const char *s)
{
    while (*s == ' ') s++;
    if (*s < '0' || *s > '9') return -1;
    int pid = 0;
    while (*s >= '0' && *s <= '9') pid = pid * 10 + (*s++ - '0');
    return pid;
}

static void cmd_kill(const char *arg)
{
    int pid = parse_pid(arg);
    task_t *t = pid >= 0 ? task_by_pid(pid) : 0;
    if (t && !t->is_user)
    {
        kprintf("kill: %d is the %s, part of the kernel: it cannot be stopped\n", pid, t->name);
        return;
    }
    if (!t || t->state == TASK_ZOMBIE)
    {
        print_str("Usage: kill <pid>   (see `jobs` or `ps` for the pids of programs)\n");
        return;
    }
    if (!user_is_root() && t->user.uid != user_current()->uid)
    {
        kprintf("kill: %d belongs to %s: permission denied\n", pid, t->user.name);
        return;
    }
    process_kill(pid, EXIT_KILLED);
    kprintf("Sent kill to %d (%s)\n", pid, t->name);
}

static void cmd_fg(const char *arg)
{
    int pid = parse_pid(arg);
    if (pid < 0)   // no pid: the most recent program
    {
        for (int i = 0; i < MAX_TASKS; i++)
        {
            task_t *t = task_at(i);
            if (t->state != TASK_FREE && t->state != TASK_ZOMBIE && t->is_user && t->pid > pid) pid = t->pid;
        }
    }
    task_t *t = pid > 0 ? task_by_pid(pid) : 0;
    if (!t || !t->is_user)
    {
        print_str("fg: no such program (see `jobs`)\n");
        return;
    }
    char name[16];
    k_snprintf(name, sizeof(name), "%s", t->name);
    kprintf("%s (pid %d) is now in the foreground. Ctrl+C stops it.\n", name, pid);
    report_exit(name, pid, process_wait(pid, 1), 0);
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

/* Which driver of this OS runs a PCI device, if any. */
static const char *pci_driver(const pci_device_t *d)
{
    if (d->vendor == 0x10EC && d->device == 0x8139) return "rtl8139";
    if (d->vendor == 0x10EC && (d->device == 0x8168 || d->device == 0x8161 || d->device == 0x8169)) return "rtl8168";
    if (d->class_code == 0x01 && d->subclass == 0x01) return "ata";
    if (d->vendor == 0x8086 && (d->device == 0x100E || d->device == 0x100F || d->device == 0x1004 || d->device == 0x10D3))
        return "e1000";
    for (int i = 0; i < wifi_count(); i++)
    {
        const pci_device_t *w = &wifi_get(i)->pci;
        if (w->bus == d->bus && w->slot == d->slot && w->func == d->func) return "Wi-Fi: no driver yet";
    }
    return 0;
}

static void cmd_lspci(void)
{
    pci_device_t *devs = kmalloc(64 * sizeof(pci_device_t));
    if (!devs)
    {
        print_str("lspci: out of memory\n");
        return;
    }
    int n = pci_scan(devs, 64);
    for (int i = 0; i < n; i++)
    {
        const pci_device_t *d = &devs[i];
        const char *vendor = pci_vendor_name(d->vendor), *driver = pci_driver(d);
        kprintf("%02x:%02x.%x  %04x:%04x  ", d->bus, d->slot, d->func, d->vendor, d->device);
        char cls[40];
        k_snprintf(cls, sizeof(cls), "%-30s", pci_class_name(d->class_code, d->subclass, d->prog_if));
        print_accent(cls);
        kprintf(" %s", vendor ? vendor : "");
        if (driver)
        {
            print_str("  [");
            print_highlight(driver);
            print_str("]");
        }
        print_str("\n");
    }
    kprintf("%d devices\n", n);
    kfree(devs);
}

static void show_display(void)
{
    uint32_t w, h;
    display_get_mode(&w, &h);
    kprintf("Screen %ux%u, text %ux%u, font x%u, adapter: %s\n", w, h, (uint32_t)print_get_cols(),
            (uint32_t)print_get_rows() + 1, fbcon_get_scale(), display_adapter_name());
    if (display_follows_window())
        print_str("The screen follows the VirtualBox window: resize the window to change it.\n");
}

/* resolution [WxH] */
static void cmd_resolution(const char *arg)
{
    while (*arg == ' ') arg++;
    if (!fbcon_active())
    {
        print_str("resolution: VGA text mode (no framebuffer): fixed at 80x25\n");
        return;
    }
    if (!*arg)
    {
        show_display();
        if (display_can_resize()) print_str("Change it with e.g. `resolution 1280x800`.\n");
        return;
    }
    uint32_t w = 0, h = 0;
    while (*arg >= '0' && *arg <= '9') w = w * 10 + (uint32_t)(*arg++ - '0');
    if (*arg == 'x' || *arg == 'X') arg++;
    while (*arg >= '0' && *arg <= '9') h = h * 10 + (uint32_t)(*arg++ - '0');
    if (!w || !h)
    {
        print_str("Usage: resolution <width>x<height>, e.g. resolution 1024x768\n");
        return;
    }
    if (!display_can_resize())
    {
        kprintf("resolution: the mode was set by the firmware and this adapter (%s) cannot change it\n",
                display_adapter_name());
        return;
    }
    if (display_set_mode(w, h) != 0)
        print_str("resolution: that mode does not fit in video memory\n");
    else
        show_display();
}

/* font [1-4|auto] */
static void cmd_font(const char *arg)
{
    while (*arg == ' ') arg++;
    if (!fbcon_active())
    {
        print_str("font: VGA text mode has a fixed font\n");
        return;
    }
    if (!*arg)
    {
        kprintf("Font scale x%u (8x16 pixels per cell at x1). Use `font 1`..`font 4` or `font auto`.\n",
                fbcon_get_scale());
        return;
    }
    uint32_t scale = strcmp(arg, "auto") == 0 ? 0 : (*arg >= '1' && *arg <= '4' && !arg[1]) ? (uint32_t)(*arg - '0') : 99;
    if (scale == 99)
    {
        print_str("Usage: font <1-4|auto>\n");
        return;
    }
    if (display_set_font_scale(scale) != 0)
        print_str("font: too large for this screen (needs at least 40x10 cells)\n");
    else
        show_display();
}

/* sound, beep, play, volume */
static uint32_t parse_number(const char **p, uint32_t fallback)
{
    while (**p == ' ') (*p)++;
    if (**p < '0' || **p > '9') return fallback;
    uint32_t v = 0;
    while (**p >= '0' && **p <= '9') v = v * 10 + (uint32_t)(*(*p)++ - '0');
    return v;
}

static int cmd_sound(const char *line)
{
    if (strcmp(line, "sound") == 0)
    {
        sound_print_info();
        return 0;
    }
    if (strncmp(line, "volume", 6) == 0)
    {
        const char *p = line + 6;
        uint32_t v = parse_number(&p, 1000);
        if (v != 1000) sound_set_volume((int)v);
        kprintf("Volume %d%%\n", sound_get_volume());
        return 0;
    }
    if (!sound_ready())
    {
        print_str("No sound output (`sound` says why).\n");
        return 1;
    }
    if (strncmp(line, "beep", 4) == 0)
    {
        const char *p = line + 4;
        uint32_t hz = parse_number(&p, 880), ms = parse_number(&p, 300);
        if (ms > 10000) ms = 10000;
        int r = sound_beep(hz, ms);
        if (r < 0) print_str("beep: the sound card did not play (`sound` for details)\n");
        return r < 0;
    }
    const char *name = line + 5;
    while (*name == ' ') name++;
    /* Not found: the system folder (where CHIME.WAV is) by its last name. */
    char sys_path[128];
    if (!fat32_file_exists(name))
    {
        const char *base = name;
        for (const char *q = name; *q; q++)
            if (*q == '/') base = q + 1;
        k_snprintf(sys_path, sizeof(sys_path), "%s/%s", strcmp(vfs_system_dir(), "/") ? vfs_system_dir() : "", base);
        if (fat32_file_exists(sys_path)) name = sys_path;
    }
    return play_file(name);
}

static int play_file(const char *name)
{
    if (!fat32_file_exists(name))
    {
        kprintf("play: %s: no such file\n", name);
        return 1;
    }
    uint32_t size = fat32_get_file_size(name);
    if (size == 0 || size > 64u * 1024 * 1024)
    {
        kprintf("play: %s: %s\n", name, size ? "too large (64 MiB at most)" : "empty file");
        return 1;
    }
    uint8_t *data = kmalloc(size);
    if (!data)
    {
        print_str("play: out of memory\n");
        return 1;
    }
    int got = fat32_read_file(name, data, size);
    int r = -1;
    const char *error = 0;
    if (got == FAT32_ERR_PERMISSION) kprintf("play: %s: permission denied\n", name);
    else if (got < (int)size) kprintf("play: %s: could not read it\n", name);
    else
    {
        r = sound_play_wav(data, size, &error);
        if (error) kprintf("play: %s: %s\n", name, error);
        else if (r < 0) print_str("play: the sound card did not play (`sound` for details)\n");
        else if (r == 1) print_str("^C\n");
    }
    kfree(data);
    return r < 0;
}
