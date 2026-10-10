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
#include "drivers/disk.h"
#include "sys/editor.h"
#include "sys/shell.h"
#include "drivers/pic.h"
#include "drivers/nic.h"
#include "core/gdt.h"
#include "lib/serial.h"
#include "net/net.h"
#include "core/multiboot2.h"
#include "lib/fbcon.h"
#include "drivers/rtc.h"
#include "sys/sysinfo.h"
#include "sys/task.h"
#include "drivers/wifi.h"
#include "drivers/iwlwifi.h"
#include "drivers/hda.h"
#include "drivers/nvme.h"
#include "drivers/msi.h"
#include "sys/smp.h"
#include "drivers/part.h"
#include "sys/vfs.h"
#include "drivers/display.h"
#include "drivers/mouse.h"
#include "drivers/apic.h"
#include "drivers/usb.h"
#include "drivers/touchpad.h"
#include "drivers/acpi.h"
#include "drivers/vmmdev.h"

extern void irq0_stub();
extern void irq1_stub();

extern void irq_nic_stub();
extern void irq_spurious_master();
extern void irq_mouse_stub();
extern void irq_spurious_slave();
extern void syscall_stub();

// Exported by targets/x86_64/linker.ld: the real extent of the kernel image
// (.text, .rodata, .data, .bss and the boot stack), page aligned at the end.
extern char kernel_start[];
extern char kernel_end[];

// Physical layout: kernel image 1-2 MiB (linker.ld asserts it stays below
// 2 MiB), page-table pool 2-4 MiB (paging.c), heap in the largest usable RAM
// region between 4 MiB and 1 GiB: 1-2 GiB is the user program range, and the
// NICs DMA from kmalloc'd buffers with 32-bit addresses.
#define HEAP_LOW       0x400000ULL
#define HEAP_HIGH      0x40000000ULL      // user programs own 1-2 GiB (sys/process.h)
#define HEAP_MAX       (1024ULL * 1024 * 1024)
#define HEAP_FALLBACK  (1024 * 1024)      // no memory map: assume 1 MiB at 4 MiB
#define HEAP_MIN       (8ULL * 1024 * 1024) // heap left after carving out the RAM disk

// Mount the main volume: the ATA disk if it holds FAT32 (with the RAM disk at
// /SYS), else the RAM disk. A TERMINALOS partition on an NVMe drive takes over
// "/" later, once the timer runs (mount_persistent).
static void mount_filesystem(void) {
    blockdev_t dev;
    if (ata_init() == 0 && disk_open(DISK_ATA, &dev) == 0 && vfs_mount("/", &dev) == 0) {
        blockdev_t ram;
        int sys = disk_open(DISK_RAM, &ram) == 0 && vfs_mount("/SYS", &ram) == 0;
        print_boot_status(BOOT_OK, "Storage", "FAT32 on ATA disk%s", sys ? ", RAM disk at /SYS" : "");
        return;
    }
    if (disk_open(DISK_RAM, &dev) == 0 && vfs_mount("/", &dev) == 0) {
        print_boot_status(BOOT_OK, "Storage", "FAT32 on RAM disk, %u MiB (changes are lost at reboot)",
                          (uint32_t)(disk_ramdisk_size() >> 20));
        return;
    }
    print_boot_status(BOOT_FAIL, "Storage", "no disk with a FAT32 volume: file commands will not work");
}

// The NVMe drive's TERMINALOS partition (FAT32, made in Windows' Disk
// Management) becomes "/": homes, accounts and settings survive reboots. The
// RAM disk's system files stay reachable at /SYS.
static void mount_persistent(void) {
    static partition_t parts[16];
    int n = part_scan(nvme_partition_reader, parts, 16);
    for (int i = 0; i < n; i++) {
        if (!parts[i].ours) continue;
        blockdev_t nv, ram;
        if (disk_open_nvme_partition(parts[i].start, parts[i].sectors, 1, i + 1, &nv) != 0) return;
        if (vfs_mount("/", &nv) != 0) {
            print_boot_status(BOOT_WARN, "Files", "TERMINALOS partition found but it is not readable FAT32");
            return;
        }
        if (disk_open(DISK_RAM, &ram) == 0) vfs_mount("/SYS", &ram);
        fat32_change_directory("/");
        print_boot_status(BOOT_OK, "Files", "on the NVMe TERMINALOS partition (%u MiB, kept across reboots); "
                          "system files at /SYS", (uint32_t)(parts[i].sectors / 2048));
        return;
    }
}

static void report_clock(void) {
    rtc_time_t t;
    if (rtc_read(&t) != 0) {
        print_boot_status(BOOT_WARN, "Clock", "RTC holds an invalid time");
        return;
    }
    rtc_add_minutes(&t, RTC_LOCAL_OFFSET_MIN);
    print_boot_status(BOOT_OK, "Clock", "%u-%02u-%02u %02u:%02u %s (hardware clock in %s: `clock` to change)",
                      t.year, t.month, t.day, t.hour, t.minute, RTC_LOCAL_TZ_NAME,
                      rtc_is_local() ? "local time, like Windows" : "UTC");
}

static void statusbar_idle(void) {
    display_poll();          // VirtualBox window resized?
    usb_service();           // USB device plugged in or out?
    iwl_service();           // Wi-Fi dropped? reconnect
    mouse_poll();            // pointer, wheel, selection
    statusbar_update(0);
}

void kernel_main() {
    serial_init();      // mirror all output to COM1 from the very first line

    // UEFI has no VGA text mode: if GRUB gave us a graphics framebuffer, keep the
    // text grid in RAM until the framebuffer is mapped (after paging_init).
    fb_info_t fb;
    int have_fb = mb2_get_framebuffer(&fb) == 0;
    if (have_fb) {
        uint32_t cols, rows;
        fbcon_grid_size(&fb, &cols, &rows);
        print_use_shadow_buffer(cols, rows);
        // All of video memory if the mode can be changed later (`resolution`).
        paging_add_identity_region(fb.addr, display_early_init(&fb));
    }
    boot_info.uefi = mb2_booted_from_uefi();
    acpi_save_rsdp();
    int no_smp = mb2_cmdline_has("nosmp");    // the `nosmp` boot option (read now, for the same reason)
    // The memory map is in the multiboot info, which the heap may overwrite.
    int mem_regions = memory_init();
    uint64_t heap_start = HEAP_LOW, heap_size = HEAP_FALLBACK;

    // GRUB loads the RAM disk image (grub.cfg `module2 ... ramdisk`) wherever it
    // likes. Keep the heap away from it and use it in place, unless it sits in
    // the first 4 MiB (kernel image, page-table pool): then copy it to the
    // bottom of the heap region and start the heap after it.
    uint64_t rd_start = 0, rd_end = 0, rd_span = 0;
    int have_rd = mem_regions > 0 && mb2_get_module("ramdisk", &rd_start, &rd_end) == 0;
    int rd_copy = have_rd && rd_start < HEAP_LOW;
    if (have_rd) {
        memory_reserve(rd_start, rd_end);
        if (rd_copy) rd_span = (rd_end - rd_start + LARGE_PAGE - 1) & ~(LARGE_PAGE - 1);
    }
    if (mem_regions > 0) memory_pick_heap(HEAP_LOW, HEAP_HIGH, HEAP_MAX + rd_span, &heap_start, &heap_size);
    if (have_rd && !rd_copy) {
        disk_set_ramdisk((uint8_t *)rd_start, rd_end - rd_start);
        paging_add_identity_region(rd_start, rd_end - rd_start);
    } else if (rd_copy && heap_size >= rd_span + HEAP_MIN) {
        memcpy((void *)heap_start, (const void *)rd_start, rd_end - rd_start);
        disk_set_ramdisk((uint8_t *)heap_start, rd_end - rd_start);
        paging_add_identity_region(heap_start, rd_end - rd_start);
        heap_start += rd_span;
        heap_size -= rd_span;
    }
    if (heap_size > HEAP_MAX) heap_size = HEAP_MAX;

    gdt_init();         // GDT + TSS (own stack for double faults)

    // Screen: status bar on the top row, logo, then one status line per
    // subsystem. Driver chatter goes to serial and the boot log (`dmesg`).
    print_reserve_status_line();
    print_set_theme(THEME_CYBERPUNK);
    sysinfo_print_logo();
    print_set_muted(1);

    print_boot_status(BOOT_OK, "CPU", "%s", sysinfo_cpu_name());

    // Initialize IDT (installs the CPU exception handlers) and PIC
    idt_init();
    pic_remap();

    // Set keyboard IRQ (IRQ1) handler
    idt_set_entry(0x21, irq1_stub, 0x8E);
    idt_set_entry(0x20, irq0_stub, 0x8E);
    idt_set_entry(0x27, irq_spurious_master, 0x8E);   // spurious PIC interrupts
    idt_set_entry(0x2F, irq_spurious_slave, 0x8E);
    idt_set_entry(0xFF, irq_spurious_master, 0x8E);   // local APIC spurious interrupt (no EOI)
    idt_set_entry(0x2C, irq_mouse_stub, 0x8E);        // IRQ 12: PS/2 mouse
    idt_set_entry(0x80, syscall_stub, 0xEE);   // system calls: DPL 3, so ring 3 may `int 0x80`

    // Initialize keyboard and enable interrupts
    init_keyboard();
    timer_init();
    int have_mouse = mouse_init() == 0;      // interrupts are still off: answers are read directly

    paging_init((uint64_t)kernel_start, (uint64_t)kernel_end, heap_start, heap_size);
    heap_init(heap_start, heap_size);
    if (mem_regions > 0) {
        print_boot_status(BOOT_OK, "Memory", "%u MiB RAM, %u MiB kernel heap",
                          (uint32_t)(get_total_memory() >> 20), (uint32_t)(heap_size >> 20));
        kprintf("Heap at %u MiB\n", (uint32_t)(heap_start >> 20));
        if (!memory_range_usable(0x100000, HEAP_LOW))
            kprintf("Note: RAM at 1-4 MiB (kernel, page tables) is not listed as usable\n");
    } else {
        print_boot_status(BOOT_WARN, "Memory", "no memory map from the bootloader: 1 MiB heap");
    }
    if (have_fb) {
        if (fbcon_init(&fb) == 0) {
            boot_info.fb_width = fb.width;
            boot_info.fb_height = fb.height;
            boot_info.fb_bpp = fb.bpp;
            print_boot_status(BOOT_OK, "Display", "%ux%u framebuffer, %ux%u text, Terminus font",
                              fb.width, fb.height, (uint32_t)print_get_cols(), (uint32_t)print_get_rows() + 1);
        } else {
            serial_puts("[ERR] Unsupported framebuffer format; output on serial only\n");
        }
    } else {
        print_boot_status(BOOT_OK, "Display", "VGA text mode, %ux%u",
                          (uint32_t)print_get_cols(), (uint32_t)print_get_rows() + 1);
    }

    expand_scrollback();
    if (vmmdev_init() == 0) {              // VirtualBox guest device: window size, mouse position
        if (have_fb) display_init();
        mouse_init_vbox();
    }
    print_boot_status(BOOT_OK, "Interrupts", "IDT, 8259 PIC, PIT timer at 100 Hz");
    print_boot_status(BOOT_OK, "Keyboard", "PS/2, US layout");
    if (have_mouse)
        print_boot_status(BOOT_OK, "Mouse", "%s", mouse_description());
    else
        print_boot_status(BOOT_WARN, "Mouse", "no PS/2 mouse (a USB mouse works; a touchpad may need Basic mode in the firmware)");

    if (nic_probe_init() == 0) {
        if (nic_get_irq() != NIC_IRQ_NONE)
            idt_set_entry(0x20 + nic_get_irq(), irq_nic_stub, 0x8E);
        net_init();
        char mac[18];
        net_fmt_mac(mac, net_get_config()->mac);
        print_boot_status(BOOT_OK, "Network", "%s, MAC %s", nic_name(), mac);
    } else {
        print_boot_status(BOOT_WARN, "Network", "no supported network card (RTL8139, RTL8168, Intel e1000)");
    }

    if (wifi_detect() > 0) {
        char name[80];
        wifi_describe(wifi_get(0), name, sizeof(name));
        print_boot_status(BOOT_OK, "Wi-Fi", "%s found (`wifi scan`, `wifi connect <name>`)", name);
    } else {
        print_boot_status(BOOT_WARN, "Wi-Fi", "no Wi-Fi adapter on PCI");
    }

    mount_filesystem();
    fat32_change_directory("/");
    report_clock();

    __asm__ volatile("sti");

    // Real PCs do not always deliver the PIT's interrupt the way VMs do;
    // everything that waits (DHCP, sleep, the scheduler) needs the ticks.
    char timer_how[96];
    int have_ticks = timer_check(timer_how, sizeof(timer_how)) == 0;
    print_boot_status(have_ticks ? BOOT_OK : BOOT_FAIL, "Timer", "%s", timer_how);

    // Message-signalled interrupts (devices interrupt through the local APIC).
    if (have_ticks) msi_init();

    // USB waits for the controller's answers, so it needs the ticks too.
    if (have_ticks) {
        int mice = usb_init();
        print_boot_status(mice > 0 ? BOOT_OK : BOOT_WARN, "USB", "%s%s", usb_description(),
                          mice == 0 ? " (plug a mouse in any time)" : "");
        char nvme_how[160];
        int nv = nvme_init(nvme_how, sizeof(nvme_how));
        if (nv == 0 || strncmp(nvme_how, "no NVMe", 7) != 0)
            print_boot_status(nv == 0 ? BOOT_OK : BOOT_WARN, "NVMe", "%s%s", nvme_how,
                              nv == 0 ? " (`nvme` lists partitions)" : "");
        if (nv == 0) mount_persistent();
        char sound_how[96];
        int snd = sound_init(sound_how, sizeof(sound_how));
        print_boot_status(snd == 0 ? BOOT_OK : BOOT_WARN, "Sound", "%s", sound_how);
        int tp = touchpad_init();
        print_boot_status(tp == 0 ? BOOT_OK : BOOT_WARN, "Touchpad", "%s", touchpad_description());
    }

    // DHCP needs the timer and NIC interrupts, so it runs after sti.
    if (net_is_up() && !have_ticks) {
        print_boot_status(BOOT_WARN, "DHCP", "skipped: no timer to time it out");
    } else if (net_is_up()) {
        char ip[16], gw[16];
        int ok = net_configure() == 0;
        net_fmt_ip(ip, net_get_config()->ip);
        net_fmt_ip(gw, net_get_config()->gateway);
        if (ok)
            print_boot_status(BOOT_OK, "DHCP", "%s, gateway %s", ip, gw);
        else
            print_boot_status(BOOT_WARN, "DHCP", "no answer, using %s", ip);
    }

    // A Wi-Fi network joined before (saved in /WIFI.CFG): join it again.
    if (have_ticks && wifi_count() > 0) {
        char how[128];
        int r = iwl_autoconnect(how, sizeof(how));
        if (r == 0) print_boot_status(BOOT_OK, "Wi-Fi", "%s", how);
        else if (r < 0) print_boot_status(BOOT_WARN, "Wi-Fi", "%s", how);
    }

    print_set_muted(0);
    print_bootlog_stop();
    task_init();        // the shell becomes task 1; programs run alongside it
    if (have_ticks) {   // the other cores join the scheduler
        int cores = smp_init(!no_smp);
        kprintf("CPU: %d core%s running\n", cores, cores == 1 ? "" : "s");
    }
    statusbar_update(1);
    keyboard_set_idle_hook(statusbar_idle);

    shell_run();
}
