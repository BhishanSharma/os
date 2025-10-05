#ifndef FAT32_H
#define FAT32_H

#include <stdint.h>

#define FAT32_SECTOR_SIZE 512
#define FAT32_MAX_PATH 256
#define FAT32_MAX_FILES 64

// File attributes
#define FAT_ATTR_READ_ONLY  0x01
#define FAT_ATTR_HIDDEN     0x02
#define FAT_ATTR_SYSTEM     0x04
#define FAT_ATTR_VOLUME_ID  0x08
#define FAT_ATTR_DIRECTORY  0x10
#define FAT_ATTR_ARCHIVE    0x20
#define FAT_ATTR_LONG_NAME  0x0F

#define MAX_PATH_DEPTH 16

typedef struct {
    uint8_t  jump[3];
    uint8_t  oem_name[8];
    uint16_t bytes_per_sector;
    uint8_t  sectors_per_cluster;
    uint16_t reserved_sectors;
    uint8_t  num_fats;
    uint16_t root_entry_count;
    uint16_t total_sectors_16;
    uint8_t  media_type;
    uint16_t fat_size_16;
    uint16_t sectors_per_track;
    uint16_t num_heads;
    uint32_t hidden_sectors;
    uint32_t total_sectors_32;
    
    // FAT32 extended fields
    uint32_t fat_size_32;
    uint16_t ext_flags;
    uint16_t fs_version;
    uint32_t root_cluster;
    uint16_t fs_info;
    uint16_t backup_boot_sector;
    uint8_t  reserved[12];
    uint8_t  drive_number;
    uint8_t  reserved1;
    uint8_t  boot_signature;
    uint32_t volume_id;
    uint8_t  volume_label[11];
    uint8_t  fs_type[8];
} __attribute__((packed)) fat32_boot_sector_t;

typedef struct {
    uint8_t  name[11];
    uint8_t  attributes;
    uint8_t  reserved;
    uint8_t  creation_time_tenths;
    uint16_t creation_time;
    uint16_t creation_date;
    uint16_t last_access_date;
    uint16_t first_cluster_high;
    uint16_t last_mod_time;
    uint16_t last_mod_date;
    uint16_t first_cluster_low;
    uint32_t file_size;
} __attribute__((packed)) fat32_dir_entry_t;

typedef struct {
    char name[256];
    uint32_t size;
    uint32_t first_cluster;
    uint8_t attributes;
    uint8_t is_directory;
} fat32_file_info_t;

typedef struct {
    char name[256];
    uint32_t cluster;
} path_component_t;

// Core functions
int fat32_init(uint32_t partition_lba);
int fat32_read_file(const char* path, uint8_t* buffer, uint32_t max_size);
int fat32_list_directory(fat32_file_info_t* files, uint32_t max_files);
int fat32_file_exists(const char* path);
uint32_t fat32_get_file_size(const char* path);

int fat32_write_file(const char* path, const uint8_t* buffer, uint32_t size);
int fat32_create_file(const char* path);
int fat32_delete_file(const char* path);
int fat32_change_directory(const char* path);
int fat32_get_current_directory(char* buffer, uint32_t size);
int fat32_mkdir(const char* path);
int fat32_list_directory_ex(const char* path, fat32_file_info_t* files, uint32_t max_files);

#endif