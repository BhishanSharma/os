// wifi.h - finding Wi-Fi adapters
//
// Laptops have their Wi-Fi card on PCI Express (an M.2 card, or built into
// the chipset with Intel CNVi). This finds them by PCI class and by a table
// of common chips, and says what each one is. Talking to the radio (scanning,
// connecting) needs a driver for that chip family: none is written yet.
#ifndef WIFI_H
#define WIFI_H

#include <stdint.h>
#include "drivers/pci.h"

#define WIFI_MAX_ADAPTERS 4

typedef struct {
    pci_device_t pci;
    const char *vendor;       /* "Intel" */
    const char *model;        /* "Wireless-AC 3168", or 0 if the chip is not in the table */
    const char *standard;     /* "Wi-Fi 5 (802.11ac)", or 0 */
    const char *family;       /* the Linux driver for it, a good guide to the work: "iwlwifi" */
    int needs_firmware;       /* the chip runs firmware the driver must load first */
} wifi_adapter_t;

/* Scan the PCI bus. Returns the number of Wi-Fi adapters found. */
int wifi_detect(void);

int wifi_count(void);
const wifi_adapter_t *wifi_get(int index);

/* "Intel Wireless-AC 3168" */
void wifi_describe(const wifi_adapter_t *a, char *out, uint32_t size);

/* The `wifi` command: adapters found and what they need. */
void wifi_print_status(void);

#endif
