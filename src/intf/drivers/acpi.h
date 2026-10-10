// acpi.h - reading the firmware's ACPI tables (no AML interpreter)
//
// The DSDT and SSDTs hold the firmware's description of the machine as AML
// bytecode. Resource descriptors inside it (which bus and address a device
// is on) are stored as plain byte templates, so they can be found by
// scanning, without running any AML.
#ifndef ACPI_H
#define ACPI_H

#include <stdint.h>

typedef struct {
    char device[5];          /* the AML Device() it is in, e.g. "TPD0" */
    char controller[40];     /* the I2C controller's path, e.g. "\_SB.PC00.I2C1" */
    uint16_t address;
    uint32_t speed;          /* Hz */
} acpi_i2c_device_t;

/* Keep a copy of the boot loader's RSDP. Call before paging_init (the
 * boot information is not mapped afterwards). */
void acpi_save_rsdp(void);

/* I2C devices described in the DSDT and SSDTs. Call during boot. Returns
 * how many were found (at most `max`), or -1 without ACPI tables. */
int acpi_find_i2c_devices(acpi_i2c_device_t *out, int max);

#endif
