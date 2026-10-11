// install.h - putting Terminal OS on this PC's SSD (the `install` command)
//
// Started from its USB stick, Terminal OS can install itself: the loader
// passes the three files to install (TERMOS.EFI, KERNEL.BIN, RAMDISK.GZ) as
// boot modules; `install` copies them to \EFI\TERMINALOS on a partition the
// user picks and adds "Terminal OS" to the firmware's boot menu (F12).
#ifndef INSTALL_H
#define INSTALL_H

/* Keep the boot modules and the boot options (call after memory_init,
 * before the heap and paging are set up). */
void install_save_boot_info(void);

/* How Terminal OS was started, for the boot screen ("" if unknown). */
const char *install_boot_description(void);

/* `install`: the interactive installer. */
void install_run(void);

/* `bootmenu`: the firmware's boot menu entries, in order. */
void install_print_boot_menu(void);

/* Was the kernel started by Terminal OS's own loader (not GRUB)? */
int install_own_loader(void);

#endif
