// ahci.h - SATA drives behind an AHCI controller (PCI class 01/06/01)
//
// Desktops and many laptops have their SSD or hard disk on SATA. Every disk
// found becomes a drive (drivers/drive.h). CD/DVD drives and port
// multipliers are listed but not used.
#ifndef AHCI_H
#define AHCI_H

/* Find the AHCI controllers and start their disks. How many disks; `how`
 * describes them ("no AHCI controller" when there is none). */
int ahci_init(char *how, int size);

/* `sata`: the controller and each port. */
void ahci_print_info(void);

#endif
