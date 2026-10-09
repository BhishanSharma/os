// ata.h
#ifndef ATA_H
#define ATA_H

#include <stdint.h>

/* Probe the primary master. Returns 0 if a drive answers, -1 otherwise (no
 * legacy IDE controller, as on NVMe laptops, or no drive). Never hangs. */
int ata_init(void);
int ata_read_sectors(uint32_t lba, uint32_t count, uint8_t* buffer);
int ata_write_sectors(uint32_t lba, uint32_t count, uint8_t* buffer);

#endif
