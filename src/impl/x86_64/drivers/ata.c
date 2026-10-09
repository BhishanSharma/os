// ata.c - Simple ATA PIO driver
#include "drivers/ata.h"
#include "../lib/ports.h"

#define ATA_PRIMARY_IO      0x1F0
#define ATA_PRIMARY_CONTROL 0x3F6

// ATA registers
#define ATA_REG_DATA       0x00
#define ATA_REG_ERROR      0x01
#define ATA_REG_FEATURES   0x01
#define ATA_REG_SECCOUNT   0x02
#define ATA_REG_LBA_LOW    0x03
#define ATA_REG_LBA_MID    0x04
#define ATA_REG_LBA_HIGH   0x05
#define ATA_REG_DRIVE      0x06
#define ATA_REG_STATUS     0x07
#define ATA_REG_COMMAND    0x07

// Status bits
#define ATA_SR_BSY  0x80
#define ATA_SR_DRDY 0x40
#define ATA_SR_DRQ  0x08
#define ATA_SR_ERR  0x01

// Commands
#define ATA_CMD_READ_SECTORS  0x20
#define ATA_CMD_WRITE_SECTORS 0x30
#define ATA_CMD_IDENTIFY      0xEC

extern uint8_t inb(uint16_t port);
extern void outb(uint16_t port, uint8_t val);
extern uint16_t inw(uint16_t port);
extern void outw(uint16_t port, uint16_t val);

// Each status read is an I/O port access (about a microsecond), so this is
// roughly a second. Without a limit a missing controller (status 0xFF, BSY
// always set) would hang the boot.
#define ATA_TIMEOUT 1000000

static int ata_wait_busy(void) {
    for (int i = 0; i < ATA_TIMEOUT; i++)
        if (!(inb(ATA_PRIMARY_IO + ATA_REG_STATUS) & ATA_SR_BSY)) return 0;
    return -1;
}

static int ata_wait_drq(void) {
    for (int i = 0; i < ATA_TIMEOUT; i++) {
        uint8_t status = inb(ATA_PRIMARY_IO + ATA_REG_STATUS);
        if (status & ATA_SR_ERR) return -1;
        if (!(status & ATA_SR_BSY) && (status & ATA_SR_DRQ)) return 0;
    }
    return -1;
}

int ata_init(void) {
    // A floating bus (no controller or no drive) reads as 0xFF.
    if (inb(ATA_PRIMARY_IO + ATA_REG_STATUS) == 0xFF) return -1;
    outb(ATA_PRIMARY_IO + ATA_REG_DRIVE, 0xA0);  // Select master drive
    if (ata_wait_busy() != 0) return -1;

    uint8_t status = inb(ATA_PRIMARY_IO + ATA_REG_STATUS);
    if (status == 0xFF || status == 0x00) {
        return -1;  // No drive
    }

    return 0;
}

int ata_read_sectors(uint32_t lba, uint32_t count, uint8_t* buffer) {
    if (count == 0 || count > 256) {
        return -1;
    }
    
    if (ata_wait_busy() != 0) return -3;

    // Select drive and set LBA mode
    outb(ATA_PRIMARY_IO + ATA_REG_DRIVE, 0xE0 | ((lba >> 24) & 0x0F));
    outb(ATA_PRIMARY_IO + ATA_REG_SECCOUNT, (uint8_t)count);
    outb(ATA_PRIMARY_IO + ATA_REG_LBA_LOW, (uint8_t)lba);
    outb(ATA_PRIMARY_IO + ATA_REG_LBA_MID, (uint8_t)(lba >> 8));
    outb(ATA_PRIMARY_IO + ATA_REG_LBA_HIGH, (uint8_t)(lba >> 16));
    outb(ATA_PRIMARY_IO + ATA_REG_COMMAND, ATA_CMD_READ_SECTORS);
    
    for (uint32_t sector = 0; sector < count; sector++) {
        if (ata_wait_drq() != 0) return -2;
        
        // Read 512 bytes (256 words)
        uint16_t* buf16 = (uint16_t*)(buffer + sector * 512);
        for (int i = 0; i < 256; i++) {
            buf16[i] = inw(ATA_PRIMARY_IO + ATA_REG_DATA);
        }
    }
    
    return 0;
}

int ata_write_sectors(uint32_t lba, uint32_t count, uint8_t* buffer) {
    if (count == 0 || count > 256) {
        return -1;
    }
    
    if (ata_wait_busy() != 0) return -3;

    outb(ATA_PRIMARY_IO + ATA_REG_DRIVE, 0xE0 | ((lba >> 24) & 0x0F));
    outb(ATA_PRIMARY_IO + ATA_REG_SECCOUNT, (uint8_t)count);
    outb(ATA_PRIMARY_IO + ATA_REG_LBA_LOW, (uint8_t)lba);
    outb(ATA_PRIMARY_IO + ATA_REG_LBA_MID, (uint8_t)(lba >> 8));
    outb(ATA_PRIMARY_IO + ATA_REG_LBA_HIGH, (uint8_t)(lba >> 16));
    outb(ATA_PRIMARY_IO + ATA_REG_COMMAND, ATA_CMD_WRITE_SECTORS);
    
    for (uint32_t sector = 0; sector < count; sector++) {
        if (ata_wait_drq() != 0) return -2;

        uint16_t* buf16 = (uint16_t*)(buffer + sector * 512);
        for (int i = 0; i < 256; i++) {
            outw(ATA_PRIMARY_IO + ATA_REG_DATA, buf16[i]);
        }
        
        // Flush cache
        outb(ATA_PRIMARY_IO + ATA_REG_COMMAND, 0xE7);
        if (ata_wait_busy() != 0) return -3;
    }
    
    return 0;
}