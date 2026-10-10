// vmmdev.c - the VirtualBox guest device
#include "drivers/vmmdev.h"
#include "drivers/pci.h"
#include "lib/print.h"
#include "lib/string.h"
#include "../lib/ports.h"

#define VMMDEV_INTERFACE_VERSION 0x00010004

static uint16_t port;

typedef struct {
    vmmdev_header_t header;
    uint32_t interface_version, os_type;
} __attribute__((packed)) req_guest_info_t;

typedef struct {
    vmmdev_header_t header;
    uint32_t facility, status, flags;
} __attribute__((packed)) req_guest_status_t;

int vmmdev_present(void) { return port != 0; }

int vmmdev_request(void *req, uint32_t type, uint32_t size) {
    if (!port) return -1;
    vmmdev_header_t *h = req;
    *h = (vmmdev_header_t){ size, VMMDEV_REQUEST_VERSION, type, -1, 0, 0 };
    uint64_t flags;                                    // one request at a time (the mouse asks from IRQs)
    __asm__ volatile("pushfq; pop %0; cli" : "=r"(flags) :: "memory");
    outl(port, (uint32_t)(uintptr_t)req);
    if (flags & 0x200) __asm__ volatile("sti");
    return h->rc;
}

int vmmdev_report_facility(uint32_t facility) {
    static req_guest_status_t req;
    memset(&req, 0, sizeof(req));
    req.facility = facility;
    req.status = VMMDEV_FACILITY_ACTIVE;
    return vmmdev_request(&req, VMMDEV_REPORT_GUEST_STATUS, sizeof(req));
}

int vmmdev_init(void) {
    uint8_t bus, slot, func;
    if (pci_find_device(0x80EE, 0xCAFE, &bus, &slot, &func) != 0) return -1;
    uint32_t bar0 = pci_config_read_dword(bus, slot, func, 0x10);
    if (!(bar0 & 1)) return -1;
    uint32_t cmd = pci_config_read_dword(bus, slot, func, 0x04);
    pci_config_write_dword(bus, slot, func, 0x04, cmd | 0x1);
    port = (uint16_t)(bar0 & ~3u);

    static req_guest_info_t info;
    memset(&info, 0, sizeof(info));
    info.interface_version = VMMDEV_INTERFACE_VERSION;
    info.os_type = 0;                                  // "other"
    int rc_info = vmmdev_request(&info, VMMDEV_REPORT_GUEST_INFO, sizeof(info));
    int rc_driver = vmmdev_report_facility(VMMDEV_FACILITY_GUEST_DRIVER);
    kprintf("VirtualBox guest device at port 0x%x: guest info rc=%d, driver status rc=%d\n",
            port, rc_info, rc_driver);
    if (rc_info < 0) {
        port = 0;
        return -1;
    }
    return 0;
}
