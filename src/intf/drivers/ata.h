// ata.h
#ifndef ATA_H
#define ATA_H

#include <stdint.h>

int ata_init(void);
int disk_read_sectors(uint32_t lba, uint32_t count, uint8_t* buffer);
int disk_write_sectors(uint32_t lba, uint32_t count, uint8_t* buffer);

#endif