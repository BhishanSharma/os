// sysinfo.c - logo, status bar and the `sysinfo` command
#include "sys/sysinfo.h"
#include "sys/vfs.h"
#include "drivers/wifi.h"
#include "lib/print.h"
#include "lib/string.h"
#include "drivers/rtc.h"
#include "drivers/timer.h"
#include "drivers/heap.h"
#include "drivers/memory.h"
#include "drivers/disk.h"
#include "drivers/nic.h"
#include "net/net.h"
#include "sys/users.h"

boot_info_t boot_info;

#define SEP " \xB3 "   /* CP437 vertical line */

static const char *logo[] = {
    " _____   ___   ___    __  __   ___   _  _     _      _           ___    ___",
    "|_   _| | __| | _ \\  |  \\/  | |_ _| | \\| |   /_\\    | |         / _ \\  / __|",
    "  | |   | _|  |   /  | |\\/| |  | |  | .` |  / _ \\   | |__      | (_) | \\__ \\",
    "  |_|   |___| |_|_\\  |_|  |_| |___| |_|\\_| /_/ \\_\\  |____|      \\___/  |___/",
};
#define LOGO_LINES (int)(sizeof(logo) / sizeof(logo[0]))
#define LOGO_WIDTH 77

/* Bold version for screens at least BIG_LOGO_WIDTH + 2 columns wide, in CP437
 * block (0xDB) and double-line box characters. */
static const char *big_logo[] = {
    "\xDB\xDB\xDB\xDB\xDB\xDB\xDB\xDB\xBB\xDB\xDB\xDB\xDB\xDB\xDB\xDB\xBB\xDB\xDB\xDB\xDB\xDB\xDB\xBB \xDB\xDB\xDB\xBB   \xDB\xDB\xDB\xBB\xDB\xDB\xBB\xDB\xDB\xDB\xBB   \xDB\xDB\xBB \xDB\xDB\xDB\xDB\xDB\xBB \xDB\xDB\xBB        \xDB\xDB\xDB\xDB\xDB\xDB\xBB \xDB\xDB\xDB\xDB\xDB\xDB\xDB\xBB",
    "\xC8\xCD\xCD\xDB\xDB\xC9\xCD\xCD\xBC\xDB\xDB\xC9\xCD\xCD\xCD\xCD\xBC\xDB\xDB\xC9\xCD\xCD\xDB\xDB\xBB\xDB\xDB\xDB\xDB\xBB \xDB\xDB\xDB\xDB\xBA\xDB\xDB\xBA\xDB\xDB\xDB\xDB\xBB  \xDB\xDB\xBA\xDB\xDB\xC9\xCD\xCD\xDB\xDB\xBB\xDB\xDB\xBA       \xDB\xDB\xC9\xCD\xCD\xCD\xDB\xDB\xBB\xDB\xDB\xC9\xCD\xCD\xCD\xCD\xBC",
    "   \xDB\xDB\xBA   \xDB\xDB\xDB\xDB\xDB\xBB  \xDB\xDB\xDB\xDB\xDB\xDB\xC9\xBC\xDB\xDB\xC9\xDB\xDB\xDB\xDB\xC9\xDB\xDB\xBA\xDB\xDB\xBA\xDB\xDB\xC9\xDB\xDB\xBB \xDB\xDB\xBA\xDB\xDB\xDB\xDB\xDB\xDB\xDB\xBA\xDB\xDB\xBA       \xDB\xDB\xBA   \xDB\xDB\xBA\xDB\xDB\xDB\xDB\xDB\xDB\xDB\xBB",
    "   \xDB\xDB\xBA   \xDB\xDB\xC9\xCD\xCD\xBC  \xDB\xDB\xC9\xCD\xCD\xDB\xDB\xBB\xDB\xDB\xBA\xC8\xDB\xDB\xC9\xBC\xDB\xDB\xBA\xDB\xDB\xBA\xDB\xDB\xBA\xC8\xDB\xDB\xBB\xDB\xDB\xBA\xDB\xDB\xC9\xCD\xCD\xDB\xDB\xBA\xDB\xDB\xBA       \xDB\xDB\xBA   \xDB\xDB\xBA\xC8\xCD\xCD\xCD\xCD\xDB\xDB\xBA",
    "   \xDB\xDB\xBA   \xDB\xDB\xDB\xDB\xDB\xDB\xDB\xBB\xDB\xDB\xBA  \xDB\xDB\xBA\xDB\xDB\xBA \xC8\xCD\xBC \xDB\xDB\xBA\xDB\xDB\xBA\xDB\xDB\xBA \xC8\xDB\xDB\xDB\xDB\xBA\xDB\xDB\xBA  \xDB\xDB\xBA\xDB\xDB\xDB\xDB\xDB\xDB\xDB\xBB  \xC8\xDB\xDB\xDB\xDB\xDB\xDB\xC9\xBC\xDB\xDB\xDB\xDB\xDB\xDB\xDB\xBA",
    "   \xC8\xCD\xBC   \xC8\xCD\xCD\xCD\xCD\xCD\xCD\xBC\xC8\xCD\xBC  \xC8\xCD\xBC\xC8\xCD\xBC     \xC8\xCD\xBC\xC8\xCD\xBC\xC8\xCD\xBC  \xC8\xCD\xCD\xCD\xBC\xC8\xCD\xBC  \xC8\xCD\xBC\xC8\xCD\xCD\xCD\xCD\xCD\xCD\xBC   \xC8\xCD\xCD\xCD\xCD\xCD\xBC \xC8\xCD\xCD\xCD\xCD\xCD\xCD\xBC",
};
#define BIG_LOGO_LINES (int)(sizeof(big_logo) / sizeof(big_logo[0]))
#define BIG_LOGO_WIDTH 84

static const char *day_names[] = { "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat" };
static const char *month_names[] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                     "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };

static const char *theme_names[] = { "default", "dracula", "nord", "monokai",
                                     "gruvbox", "solarized", "matrix", "cyberpunk" };

/* Day of the week, 0 = Sunday (Sakamoto's method). */
static int day_of_week(int y, int m, int d) {
    static const int t[] = { 0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4 };
    if (m < 3) y--;
    return (y + y / 4 - y / 100 + y / 400 + t[m - 1] + d) % 7;
}

static int local_time(rtc_time_t *t) {
    if (rtc_read(t) != 0) return -1;
    rtc_add_minutes(t, RTC_LOCAL_OFFSET_MIN);
    return 0;
}

static void format_uptime(char *out, size_t size) {
    uint32_t s = get_seconds();
    uint32_t d = s / 86400, h = (s / 3600) % 24, m = (s / 60) % 60;
    if (d)      k_snprintf(out, size, "%ud %uh %um", d, h, m);
    else if (h) k_snprintf(out, size, "%uh %um", h, m);
    else if (m) k_snprintf(out, size, "%um %us", m, s % 60);
    else        k_snprintf(out, size, "%us", s);
}

static void format_ip(char *out, size_t size) {
    if (!net_is_up()) {
        k_snprintf(out, size, "offline");
        return;
    }
    char ip[16];
    net_fmt_ip(ip, net_get_config()->ip);
    k_snprintf(out, size, "%s", ip);
}

void sysinfo_print_logo(void) {
    size_t cols = print_get_cols();
    print_str("\n");
    if (cols >= BIG_LOGO_WIDTH + 2) {
        // Solid blocks in the accent colour, the box-drawing "shadow" in the text colour.
        size_t pad = (cols - BIG_LOGO_WIDTH) / 2;
        for (int i = 0; i < BIG_LOGO_LINES; i++) {
            print_repeat(' ', pad);
            for (const char *p = big_logo[i]; *p; ) {
                char run[96];
                int block = (uint8_t)*p == 0xDB, n = 0;
                while (*p && ((uint8_t)*p == 0xDB) == block && n < (int)sizeof(run) - 1) run[n++] = *p++;
                run[n] = 0;
                if (block) print_accent(run);
                else print_str(run);
            }
            print_str("\n");
        }
    } else {
        size_t pad = cols > LOGO_WIDTH ? (cols - LOGO_WIDTH) / 2 : 0;
        for (int i = 0; i < LOGO_LINES; i++) {
            print_repeat(' ', pad);
            print_accent(logo[i]);
            print_str("\n");
        }
    }
    char tagline[64];
    k_snprintf(tagline, sizeof(tagline), "version %s  -  x86_64", OS_VERSION);
    size_t len = strlen(tagline);
    print_repeat(' ', cols > len ? (cols - len) / 2 : 0);
    print_str(tagline);
    print_str("\n\n");
}

const char *sysinfo_cpu_name(void) {
    static char name[49];
    if (name[0]) return name;
    uint32_t regs[12];
    uint32_t max_ext;
    __asm__ volatile("cpuid" : "=a"(max_ext) : "a"(0x80000000) : "ebx", "ecx", "edx");
    if (max_ext < 0x80000004) {
        k_snprintf(name, sizeof(name), "unknown x86_64 CPU");
        return name;
    }
    for (uint32_t i = 0; i < 3; i++)
        __asm__ volatile("cpuid"
                         : "=a"(regs[i * 4]), "=b"(regs[i * 4 + 1]), "=c"(regs[i * 4 + 2]), "=d"(regs[i * 4 + 3])
                         : "a"(0x80000002 + i));
    memcpy(name, regs, 48);
    name[48] = 0;
    // The brand string is often padded with leading spaces.
    char *start = name;
    while (*start == ' ') start++;
    if (start != name) memmove(name, start, strlen(start) + 1);
    return name;
}

void statusbar_update(int force) {
    static uint32_t last_second = 0xFFFFFFFF;
    uint32_t now = get_seconds();
    if (!force && now == last_second) return;
    last_second = now;

    char ip[24], up[24], left[160], right[48];
    format_ip(ip, sizeof(ip));
    format_uptime(up, sizeof(up));
    uint64_t used = heap_get_used();
    uint32_t total = (uint32_t)(heap_get_total() >> 20);
    k_snprintf(left, sizeof(left), " %s %s" SEP "%s" SEP "%s" SEP "RAM %u.%u/%u MiB" SEP "%s" SEP "up %s",
               OS_NAME, OS_VERSION, user_current()->name, ip, (uint32_t)(used >> 20), (uint32_t)((used & 0xFFFFF) * 10 >> 20),
               total, vfs_root_name(), up);

    rtc_time_t t;
    if (local_time(&t) == 0)
        k_snprintf(right, sizeof(right), "%s %02u %s  %02u:%02u:%02u %s ",
                   day_names[day_of_week(t.year, t.month, t.day)], t.day, month_names[t.month - 1],
                   t.hour, t.minute, t.second, RTC_LOCAL_TZ_NAME);
    else
        k_snprintf(right, sizeof(right), "clock unavailable ");
    print_status_line(left, right);
}

static void info_line(const char *label, const char *fmt, ...) {
    print_accent("  ");
    char padded[16];
    k_snprintf(padded, sizeof(padded), "%-10s", label);
    print_accent(padded);
    va_list args;
    va_start(args, fmt);
    vkprintf(fmt, args);
    va_end(args);
    print_str("\n");
}

void sysinfo_print(void) {
    char title[64], buf[64];
    k_snprintf(title, sizeof(title), "%s@%s", user_current()->name, OS_HOSTNAME);
    print_str("\n  ");
    print_highlight(title);
    print_str("\n  ");
    print_repeat('-', strlen(title));
    print_str("\n");

    info_line("OS", "%s %s x86_64", OS_NAME, OS_VERSION);
    info_line("Kernel", "built %s %s", __DATE__, __TIME__);
    info_line("Boot", "%s, GRUB 2 (multiboot2)", boot_info.uefi ? "UEFI" : "BIOS");
    format_uptime(buf, sizeof(buf));
    info_line("Uptime", "%s", buf);
    info_line("CPU", "%s", sysinfo_cpu_name());
    info_line("Memory", "%u MiB RAM, heap %u / %u MiB used",
              (uint32_t)(get_total_memory() >> 20), (uint32_t)(heap_get_used() >> 20),
              (uint32_t)(heap_get_total() >> 20));
    if (boot_info.fb_width)
        info_line("Display", "%ux%u, %u bpp, %ux%u text", boot_info.fb_width, boot_info.fb_height,
                  boot_info.fb_bpp, (uint32_t)print_get_cols(), (uint32_t)print_get_rows() + 1);
    else
        info_line("Display", "VGA text mode, %ux%u", (uint32_t)print_get_cols(), (uint32_t)print_get_rows() + 1);
    if (net_is_up()) {
        format_ip(buf, sizeof(buf));
        info_line("Network", "%s, %s", nic_name(), buf);
    } else {
        info_line("Network", "no supported network card");
    }
    if (wifi_count() > 0) {
        wifi_describe(wifi_get(0), buf, sizeof(buf));
        info_line("Wi-Fi", "%s (no driver yet)", buf);
    } else {
        info_line("Wi-Fi", "none found");
    }
    info_line("Disk", "FAT32 on %s%s", vfs_root_name(), vfs_root_persistent() ? " (kept across reboots)" : " (not saved)");
    info_line("Shell", "SHELL.ELF, a user program (type `help`)");
    color_theme_t theme = print_get_current_theme();
    info_line("Theme", "%s", (unsigned)theme < sizeof(theme_names) / sizeof(theme_names[0]) ? theme_names[theme] : "?");
    rtc_time_t t;
    if (local_time(&t) == 0)
        info_line("Time", "%s %02u %s %u, %02u:%02u %s", day_names[day_of_week(t.year, t.month, t.day)],
                  t.day, month_names[t.month - 1], t.year, t.hour, t.minute, RTC_LOCAL_TZ_NAME);

    // Colour swatches, like neofetch.
    print_str("\n  ");
    for (int c = 0; c < 16; c++) {
        print_set_color(c, c);
        print_str("   ");
        if (c == 7) {
            print_set_theme_colors();
            print_str("\n  ");
        }
    }
    print_set_theme_colors();
    print_str("\n");
}
