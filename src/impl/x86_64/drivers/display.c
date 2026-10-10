// display.c - screen modes through the Bochs VBE interface, VirtualBox window size
#include "drivers/display.h"
#include "drivers/pci.h"
#include "drivers/timer.h"
#include "lib/fbcon.h"
#include "lib/print.h"
#include "lib/string.h"
#include "sys/sysinfo.h"
#include "../lib/ports.h"
#include "lib/serial.h"

/* ---- Bochs VBE ("DISPI") registers ---------------------------------------- */

#define DISPI_INDEX       0x01CE
#define DISPI_DATA        0x01CF
#define DISPI_ID          0
#define DISPI_XRES        1
#define DISPI_YRES        2
#define DISPI_BPP         3
#define DISPI_ENABLE      4
#define DISPI_VIRT_WIDTH  6
#define DISPI_X_OFFSET    8
#define DISPI_Y_OFFSET    9

#define DISPI_ENABLED     0x01
#define DISPI_GETCAPS     0x02    /* while set, XRES/YRES/BPP read back the maximums */
#define DISPI_LFB         0x40

#define MIN_WIDTH   640
#define MIN_HEIGHT  400

static fb_info_t fb;               /* the current mode */
static uint64_t vram_size;
static int has_dispi;
static uint32_t max_width = 2560, max_height = 1600;
static const char *adapter = "unknown";

static void dispi_write(uint16_t index, uint16_t value) {
    outw(DISPI_INDEX, index);
    outw(DISPI_DATA, value);
}

static uint16_t dispi_read(uint16_t index) {
    outw(DISPI_INDEX, index);
    return inw(DISPI_DATA);
}

/* ---- Finding the adapter ------------------------------------------------- */

/* Size of a memory BAR (writing all ones, as the PCI spec describes). */
static uint64_t bar_size(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset) {
    uint32_t old = pci_config_read_dword(bus, slot, func, offset);
    pci_config_write_dword(bus, slot, func, offset, 0xFFFFFFFF);
    uint32_t probe = pci_config_read_dword(bus, slot, func, offset);
    pci_config_write_dword(bus, slot, func, offset, old);
    probe &= ~0xFu;
    return probe ? (uint64_t)(~probe) + 1 : 0;
}

uint64_t display_early_init(const fb_info_t *boot_fb) {
    fb = *boot_fb;
    uint64_t fb_bytes = (uint64_t)fb.pitch * fb.height;

    // The display device whose memory BAR holds the boot framebuffer.
    for (int bus = 0; bus < 256 && !vram_size; bus++) {
        for (uint8_t slot = 0; slot < 32 && !vram_size; slot++) {
            uint32_t id = pci_config_read_dword(bus, slot, 0, 0);
            if ((id & 0xFFFF) == 0xFFFF) continue;
            if ((pci_config_read_dword(bus, slot, 0, 0x08) >> 24) != 0x03) continue;
            for (uint8_t bar = 0x10; bar <= 0x24; bar += 4) {
                uint32_t v = pci_config_read_dword(bus, slot, 0, bar);
                if (v & 1) continue;                       // I/O ports
                uint64_t base = v & ~0xFu;
                if (((v >> 1) & 3) == 2 && bar < 0x24)
                    base |= (uint64_t)pci_config_read_dword(bus, slot, 0, bar + 4) << 32;
                uint64_t size = bar_size(bus, slot, 0, bar);
                if (base && fb.addr >= base && fb.addr < base + size) {
                    vram_size = size - (fb.addr - base);
                    uint16_t vendor = id & 0xFFFF, device = id >> 16;
                    if (vendor == 0x1234) adapter = "Bochs VBE (QEMU standard VGA)";
                    else if (vendor == 0x80EE) adapter = "VirtualBox VGA (VBE)";
                    else if (vendor == 0x15AD) adapter = "VMSVGA (VirtualBox/VMware, VBE)";
                    else adapter = pci_vendor_name(vendor) ? pci_vendor_name(vendor) : "graphics adapter";
                    (void)device;
                    break;
                }
                if (((v >> 1) & 3) == 2) bar += 4;             // skip the upper half
            }
        }
    }

    // Bochs VBE answers with an ID of 0xB0C0-0xB0C5.
    uint16_t dispi_id = dispi_read(DISPI_ID);
    has_dispi = vram_size && (dispi_id & 0xFFF0) == 0xB0C0;   // new modes are always 32 bpp
    if (has_dispi) {
        uint16_t enable = dispi_read(DISPI_ENABLE);
        dispi_write(DISPI_ENABLE, enable | DISPI_GETCAPS);
        uint16_t mx = dispi_read(DISPI_XRES), my = dispi_read(DISPI_YRES);
        dispi_write(DISPI_ENABLE, enable);
        if (mx >= MIN_WIDTH && my >= MIN_HEIGHT) { max_width = mx; max_height = my; }
    }
    if (vram_size > 256ULL << 20) vram_size = 256ULL << 20;
    if (!has_dispi || vram_size < fb_bytes) return fb_bytes;
    return vram_size;
}

int display_can_resize(void) { return has_dispi; }
const char *display_adapter_name(void) { return adapter; }

void display_get_mode(uint32_t *width, uint32_t *height) {
    *width = fb.width;
    *height = fb.height;
}

/* Lay the console out again on the current mode. */
static int relayout(void) {
    if (fbcon_init(&fb) != 0) return -1;
    uint32_t cols, rows;
    fbcon_grid_size(&fb, &cols, &rows);
    print_resize(cols, rows);
    boot_info.fb_width = fb.width;
    boot_info.fb_height = fb.height;
    statusbar_update(1);
    print_flush();
    return 0;
}

int display_set_mode(uint32_t width, uint32_t height) {
    if (!has_dispi) return -1;
    width &= ~7u;                                      // whole 8-pixel cells
    if (width < MIN_WIDTH) width = MIN_WIDTH;
    if (height < MIN_HEIGHT) height = MIN_HEIGHT;
    if (width > max_width) width = max_width & ~7u;
    if (height > max_height) height = max_height;
    while ((uint64_t)width * height * 4 > vram_size && height > MIN_HEIGHT) height -= 8;
    if ((uint64_t)width * height * 4 > vram_size) return -1;

    dispi_write(DISPI_ENABLE, 0);
    dispi_write(DISPI_XRES, (uint16_t)width);
    dispi_write(DISPI_YRES, (uint16_t)height);
    dispi_write(DISPI_BPP, 32);
    dispi_write(DISPI_ENABLE, DISPI_ENABLED | DISPI_LFB);
    dispi_write(DISPI_X_OFFSET, 0);
    dispi_write(DISPI_Y_OFFSET, 0);

    fb.width = dispi_read(DISPI_XRES);
    fb.height = dispi_read(DISPI_YRES);
    uint32_t virt = dispi_read(DISPI_VIRT_WIDTH);
    fb.pitch = (virt >= fb.width ? virt : fb.width) * 4;
    fb.bpp = 32;
    fb.red_pos = 16;   fb.red_size = 8;
    fb.green_pos = 8;  fb.green_size = 8;
    fb.blue_pos = 0;   fb.blue_size = 8;
    char msg[64];                                      // serial log only, not mid-prompt
    k_snprintf(msg, sizeof(msg), "Display: %ux%u, %u bytes per line\n", fb.width, fb.height, fb.pitch);
    serial_puts(msg);
    return relayout();
}

int display_set_font_scale(uint32_t scale) {
    fbcon_set_scale(scale);
    return relayout();
}

/* ---- VirtualBox guest device (VMMDev, PCI 80ee:cafe) ------------------------
 * Requests are structures in memory; writing one's physical address to the
 * device's I/O port makes VirtualBox process it and fill in the answer. */

#define VMMDEV_REQUEST_VERSION        0x10001
#define VMMDEV_INTERFACE_VERSION      0x00010004
#define REQ_REPORT_GUEST_INFO         50
#define REQ_GET_DISPLAY_CHANGE2       54
#define REQ_SET_GUEST_CAPABILITIES    56
#define REQ_REPORT_GUEST_STATUS       59
#define FACILITY_GUEST_DRIVER         20       /* makes the "additions run level" System */
#define FACILITY_GRAPHICS             1100
#define FACILITY_ACTIVE               50
#define GUEST_SUPPORTS_GRAPHICS       (1u << 2)
#define EVENT_DISPLAY_CHANGE_REQUEST  (1u << 2)

typedef struct {
    uint32_t size, version, type;
    int32_t rc;
    uint32_t reserved1, requestor;
} __attribute__((packed)) vmmdev_header_t;

typedef struct {
    vmmdev_header_t header;
    uint32_t interface_version, os_type;
} __attribute__((packed)) req_guest_info_t;

typedef struct {
    vmmdev_header_t header;
    uint32_t or_mask, not_mask;
} __attribute__((packed)) req_caps_t;

typedef struct {
    vmmdev_header_t header;
    uint32_t xres, yres, bpp, event_ack, display;
} __attribute__((packed)) req_display_change_t;

typedef struct {
    vmmdev_header_t header;
    uint32_t facility, status, flags;
} __attribute__((packed)) req_guest_status_t;

static uint16_t vmmdev_port;
static int follows_window;
static uint32_t last_poll, last_w, last_h;

/* Below 4 GiB (the kernel image is at 1-2 MiB), as the device needs. */
static union {
    req_guest_info_t info;
    req_caps_t caps;
    req_display_change_t display;
    req_guest_status_t status;
} request __attribute__((aligned(16)));

static int vmmdev_call(uint32_t type, uint32_t size) {
    request.info.header = (vmmdev_header_t){ size, VMMDEV_REQUEST_VERSION, type, -1, 0, 0 };
    outl(vmmdev_port, (uint32_t)(uintptr_t)&request);
    return request.info.header.rc;
}

void display_init(void) {
    uint8_t bus, slot, func;
    if (pci_find_device(0x80EE, 0xCAFE, &bus, &slot, &func) != 0) return;
    uint32_t bar0 = pci_config_read_dword(bus, slot, func, 0x10);
    if (!(bar0 & 1)) return;
    uint32_t cmd = pci_config_read_dword(bus, slot, func, 0x04);
    pci_config_write_dword(bus, slot, func, 0x04, cmd | 0x1);
    vmmdev_port = (uint16_t)(bar0 & ~3u);

    memset(&request, 0, sizeof(request));
    request.info.interface_version = VMMDEV_INTERFACE_VERSION;
    request.info.os_type = 0;                          // "other"
    int rc_info = vmmdev_call(REQ_REPORT_GUEST_INFO, sizeof(req_guest_info_t));

    memset(&request, 0, sizeof(request));
    request.caps.or_mask = GUEST_SUPPORTS_GRAPHICS;   // "resize me to the window"
    request.caps.not_mask = 0;
    int rc_caps = vmmdev_call(REQ_SET_GUEST_CAPABILITIES, sizeof(req_caps_t));

    // The VirtualBox window only sends its size ("Auto-resize Guest Display")
    // once the guest's additions are running and its graphics facility is active.
    static const uint32_t facilities[] = { FACILITY_GUEST_DRIVER, FACILITY_GRAPHICS };
    int rc_status = 0;
    for (unsigned i = 0; i < sizeof(facilities) / sizeof(facilities[0]); i++) {
        memset(&request, 0, sizeof(request));
        request.status.facility = facilities[i];
        request.status.status = FACILITY_ACTIVE;
        int rc = vmmdev_call(REQ_REPORT_GUEST_STATUS, sizeof(req_guest_status_t));
        if (rc < 0) rc_status = rc;
    }

    follows_window = rc_info >= 0 && rc_caps >= 0 && has_dispi;
    kprintf("VirtualBox guest device at port 0x%x: guest info rc=%d, capabilities rc=%d, status rc=%d\n",
            vmmdev_port, rc_info, rc_caps, rc_status);
}

int display_follows_window(void) { return follows_window; }

void display_poll(void) {
    if (!follows_window) return;
    uint32_t now = get_tick();
    if ((uint32_t)(now - last_poll) < 25) return;      // 4 times a second
    last_poll = now;

    memset(&request, 0, sizeof(request));
    request.display.event_ack = EVENT_DISPLAY_CHANGE_REQUEST;
    if (vmmdev_call(REQ_GET_DISPLAY_CHANGE2, sizeof(req_display_change_t)) < 0) return;
    uint32_t w = request.display.xres, h = request.display.yres;
    if (request.display.display != 0 || w == 0 || h == 0) return;   // no wish, or another monitor
    if (w == last_w && h == last_h) return;            // already handled
    last_w = w;
    last_h = h;
    if ((w & ~7u) == fb.width && h == fb.height) return;
    display_set_mode(w, h);
}
