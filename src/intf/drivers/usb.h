// usb.h - USB through the xHCI (USB 3) host controller
//
// Finds the xHCI controller on PCI, takes it over from the firmware, and sets
// up the devices plugged straight into its ports. For now that means mice
// (HID boot protocol): their movement goes to the same pointer as the PS/2
// mouse. Hubs and other devices are listed but not used.
#ifndef USB_H
#define USB_H

#include <stdint.h>

/* Set up the controller and the devices on it. Needs the timer (call after
 * `sti`). Returns the number of USB mice found, or -1 without a controller. */
int usb_init(void);

/* A device was plugged in or out: set it up. Waits for the controller, so
 * call it from the idle loop, not an interrupt. */
void usb_service(void);

/* "Intel xHCI, 12 ports, 1 mouse" for the boot screen. */
const char *usb_description(void);

/* `lsusb`: the controller and what is plugged into each port. */
void usb_print_devices(void);

/* A USB network adapter (CDC-ECM) is the network card. usb_net_boot_done:
 * the boot asks DHCP itself (usb_service does for one plugged in later). */
int usb_net_attached(void);
void usb_net_boot_done(void);

/* USB sticks (mass storage), numbered from 0 in the order found. Sectors are
 * 512 bytes; read and write return 0 on success. */
int usb_storage_count(void);
uint64_t usb_storage_sectors(int n);
int usb_storage_read(int n, uint64_t lba, uint32_t count, void *buf);
int usb_storage_write(int n, uint64_t lba, uint32_t count, const void *buf);

#endif
