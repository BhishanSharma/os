// efi.h - UEFI runtime services: firmware variables (the boot menu) and reset
//
// Available when Terminal OS was started by its own loader (or GRUB) on a
// UEFI PC: the loader passes the firmware's system table and memory map.
#ifndef KERNEL_EFI_H
#define KERNEL_EFI_H

#include <stdint.h>

/* EFI_GLOBAL_VARIABLE: Boot####, BootOrder, BootNext, ... */
extern const uint8_t efi_global_guid[16];

#define EFI_VAR_NV  1                /* kept across restarts */
#define EFI_VAR_BS  2                /* readable while booting */
#define EFI_VAR_RT  4                /* readable by the running system */

/* Copy what is needed from the boot information. Call before the heap
 * exists (it may overwrite the boot information). */
void efi_save_boot_info(void);

/* Are the runtime services there? */
int efi_available(void);

/* Read a variable (name in ASCII). Returns its size, or -1 if it is not
 * there (or does not fit in `size`). */
int efi_get_variable(const char *name, const uint8_t guid[16], void *data, uint32_t size, uint32_t *attributes);

/* Write (or with size 0, delete) a variable. 0 on success. */
int efi_set_variable(const char *name, const uint8_t guid[16], uint32_t attributes, const void *data, uint32_t size);

/* Restart (warm), or switch off, through the firmware. Returns only on failure. */
void efi_reset(int shutdown);

#endif
