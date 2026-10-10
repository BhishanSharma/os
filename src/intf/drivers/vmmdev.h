// vmmdev.h - the VirtualBox guest device (PCI 80ee:cafe)
//
// What VirtualBox Guest Additions talk to: requests are structures in memory,
// and writing one's physical address to the device's I/O port makes
// VirtualBox handle it and fill in the answer. Used for the window size
// (drivers/display.c) and the mouse position (drivers/mouse.c).
#ifndef VMMDEV_H
#define VMMDEV_H

#include <stdint.h>

#define VMMDEV_REQUEST_VERSION   0x10001

typedef struct {
    uint32_t size, version, type;
    int32_t rc;                      /* VirtualBox status: >= 0 success */
    uint32_t reserved1, requestor;
} __attribute__((packed)) vmmdev_header_t;

/* Request types */
#define VMMDEV_GET_MOUSE_STATUS         1
#define VMMDEV_SET_MOUSE_STATUS         2
#define VMMDEV_REPORT_GUEST_INFO        50
#define VMMDEV_GET_DISPLAY_CHANGE2      54
#define VMMDEV_SET_GUEST_CAPABILITIES   56
#define VMMDEV_REPORT_GUEST_STATUS      59

/* Facilities for VMMDEV_REPORT_GUEST_STATUS */
#define VMMDEV_FACILITY_GUEST_DRIVER    20     /* makes the "additions run level" System */
#define VMMDEV_FACILITY_GRAPHICS        1100
#define VMMDEV_FACILITY_ACTIVE          50

/* Find the device and say hello (guest info, driver running). 0 if this is
 * VirtualBox. Call once, after PCI is usable. */
int vmmdev_init(void);
int vmmdev_present(void);

/* Send `req` (starting with a vmmdev_header_t, `size` bytes in all, in memory
 * below 4 GiB such as a static variable). Fills in the header first. Returns
 * the VirtualBox status (negative = error, or no device). */
int vmmdev_request(void *req, uint32_t type, uint32_t size);

/* Tell VirtualBox a facility is running (VMMDEV_FACILITY_*). */
int vmmdev_report_facility(uint32_t facility);

#endif
