// fat32.c
#include "fat32.h"
#include "string.h"
#include <stdint.h>

extern int disk_read_sectors(uint32_t lba, uint32_t count, uint8_t* buffer);
extern int disk_write_sectors(uint32_t lba, uint32_t count, uint8_t* buffer);

static uint32_t current_directory_cluster = 0;
static char current_path[FAT32_MAX_PATH] = "/";
static fat32_boot_sector_t boot_sector;
static uint32_t partition_start_lba;
static uint32_t fat_start_sector;
static uint32_t data_start_sector;
static uint32_t sectors_per_cluster;
static uint32_t bytes_per_cluster;
static uint8_t sector_buffer[FAT32_SECTOR_SIZE];

static uint32_t fat32_get_fat_entry(uint32_t cluster) {
    uint32_t fat_offset = cluster * 4;
    uint32_t fat_sector = fat_start_sector + (fat_offset / FAT32_SECTOR_SIZE);
    uint32_t entry_offset = fat_offset % FAT32_SECTOR_SIZE;
    
    if (disk_read_sectors(fat_sector, 1, sector_buffer) != 0) {
        return 0xFFFFFFFF;
    }
    
    uint32_t entry = *(uint32_t*)(sector_buffer + entry_offset);
    return entry & 0x0FFFFFFF;  // Mask out top 4 bits
}

static int fat32_read_cluster(uint32_t cluster, uint8_t* buffer) {
    if (cluster < 2 || cluster >= 0x0FFFFFF8) {
        return -1;
    }
    
    uint32_t first_sector = data_start_sector + ((cluster - 2) * sectors_per_cluster);
    return disk_read_sectors(first_sector, sectors_per_cluster, buffer);
}

static void fat32_name_to_string(const uint8_t* fat_name, char* output) {
    int i, j = 0;
    
    // Copy name (8 chars)
    for (i = 0; i < 8 && fat_name[i] != ' '; i++) {
        output[j++] = fat_name[i];
    }
    
    // Add extension if present
    if (fat_name[8] != ' ') {
        output[j++] = '.';
        for (i = 8; i < 11 && fat_name[i] != ' '; i++) {
            output[j++] = fat_name[i];
        }
    }
    
    output[j] = '\0';
}

static void fat32_string_to_fat_name(const char* str, uint8_t* fat_name) {
    int i;
    
    for (i = 0; i < 11; i++) {
        fat_name[i] = ' ';
    }
    
    const char* dot = str;
    while (*dot && *dot != '.') dot++;
    
    for (i = 0; i < 8 && str < dot && *str; i++) {
        fat_name[i] = (*str >= 'a' && *str <= 'z') ? *str - 32 : *str;
        str++;
    }
    
    if (*dot == '.') {
        dot++;
        for (i = 8; i < 11 && *dot; i++) {
            fat_name[i] = (*dot >= 'a' && *dot <= 'z') ? *dot - 32 : *dot;
            dot++;
        }
    }
}

int fat32_init(uint32_t partition_lba) {
    partition_start_lba = partition_lba;
    
    if (disk_read_sectors(partition_lba, 1, (uint8_t*)&boot_sector) != 0) {
        return -1;
    }
    
    if (boot_sector.bytes_per_sector != 512) {
        return -2;
    }
    
    sectors_per_cluster = boot_sector.sectors_per_cluster;
    bytes_per_cluster = sectors_per_cluster * FAT32_SECTOR_SIZE;
    
    fat_start_sector = partition_lba + boot_sector.reserved_sectors;
    
    uint32_t fat_size = boot_sector.fat_size_32;
    uint32_t root_dir_sectors = ((boot_sector.root_entry_count * 32) + 
                                  (boot_sector.bytes_per_sector - 1)) / 
                                  boot_sector.bytes_per_sector;
    
    data_start_sector = fat_start_sector + 
                       (boot_sector.num_fats * fat_size) + 
                       root_dir_sectors;
    
    return 0;
}

static uint32_t fat32_find_file(uint32_t dir_cluster, const char* filename, 
                                fat32_dir_entry_t* entry_out) {
    uint8_t* cluster_buffer = kmalloc(bytes_per_cluster);
    if (!cluster_buffer) return 0;
    
    char upper_filename[256];
    int idx = 0;
    while (filename[idx] && idx < 255) {
        char c = filename[idx];
        upper_filename[idx] = (c >= 'a' && c <= 'z') ? c - 32 : c;
        idx++;
    }
    upper_filename[idx] = '\0';
    
    uint8_t fat_name[11];
    fat32_string_to_fat_name(upper_filename, fat_name);
    
    uint32_t cluster = dir_cluster;
    
    while (cluster < 0x0FFFFFF8) {
        if (fat32_read_cluster(cluster, cluster_buffer) != 0) {
            kfree(cluster_buffer);
            return 0;
        }
        
        fat32_dir_entry_t* entries = (fat32_dir_entry_t*)cluster_buffer;
        uint32_t entries_per_cluster = bytes_per_cluster / sizeof(fat32_dir_entry_t);
        
        for (uint32_t i = 0; i < entries_per_cluster; i++) {
            if (entries[i].name[0] == 0x00) {
                kfree(cluster_buffer);
                return 0;
            }
            
            if (entries[i].name[0] == 0xE5 || 
                entries[i].attributes & FAT_ATTR_LONG_NAME) {
                continue;
            }
            
            int match = 1;
            for (int j = 0; j < 11; j++) {
                if (entries[i].name[j] != fat_name[j]) {
                    match = 0;
                    break;
                }
            }
            
            if (match) {
                if (entry_out) {
                    *entry_out = entries[i];
                }
                uint32_t result = (entries[i].first_cluster_high << 16) | 
                                  entries[i].first_cluster_low;
                kfree(cluster_buffer);
                return result;
            }
        }
        
        cluster = fat32_get_fat_entry(cluster);
    }
    
    kfree(cluster_buffer);
    return 0;
}

int fat32_read_file(const char* path, uint8_t* buffer, uint32_t max_size) {
    fat32_dir_entry_t entry;
    
    uint32_t cluster = fat32_find_file(boot_sector.root_cluster, path, &entry);
    
    if (cluster == 0) {
        return -1;
    }
    
    uint32_t bytes_read = 0;
    uint32_t file_size = entry.file_size;
    
    if (file_size > max_size) {
        file_size = max_size;
    }
    
    uint8_t* temp_cluster = kmalloc(bytes_per_cluster);
    if (!temp_cluster) {
        return -3;
    }
    
    while (cluster < 0x0FFFFFF8 && bytes_read < file_size) {
        if (fat32_read_cluster(cluster, temp_cluster) != 0) {
            kfree(temp_cluster);
            return -2;
        }
        
        uint32_t to_copy = bytes_per_cluster;
        if (bytes_read + to_copy > file_size) {
            to_copy = file_size - bytes_read;
        }
        
        for (uint32_t i = 0; i < to_copy; i++) {
            buffer[bytes_read + i] = temp_cluster[i];
        }
        
        bytes_read += to_copy;
        cluster = fat32_get_fat_entry(cluster);
    }
    
    kfree(temp_cluster);
    return file_size;
}

int fat32_list_directory(const char* path, fat32_file_info_t* files, uint32_t max_files) {
    uint8_t* cluster_buffer = kmalloc(bytes_per_cluster);
    if (!cluster_buffer) return -1;
    
    uint32_t cluster = boot_sector.root_cluster;  // Root for now
    uint32_t file_count = 0;
    
    while (cluster < 0x0FFFFFF8 && file_count < max_files) {
        if (fat32_read_cluster(cluster, cluster_buffer) != 0) {
            break;
        }
        
        fat32_dir_entry_t* entries = (fat32_dir_entry_t*)cluster_buffer;
        uint32_t entries_per_cluster = bytes_per_cluster / sizeof(fat32_dir_entry_t);
        
        for (uint32_t i = 0; i < entries_per_cluster && file_count < max_files; i++) {
            if (entries[i].name[0] == 0x00) {
                return file_count;
            }
            
            if (entries[i].name[0] == 0xE5 || 
                entries[i].attributes & FAT_ATTR_LONG_NAME) {
                continue;
            }
            
            fat32_name_to_string(entries[i].name, files[file_count].name);
            files[file_count].size = entries[i].file_size;
            files[file_count].first_cluster = 
                (entries[i].first_cluster_high << 16) | entries[i].first_cluster_low;
            files[file_count].attributes = entries[i].attributes;
            files[file_count].is_directory = 
                (entries[i].attributes & FAT_ATTR_DIRECTORY) ? 1 : 0;
            
            file_count++;
        }
        
        cluster = fat32_get_fat_entry(cluster);
    }
    
    kfree(cluster_buffer);
    return file_count;
}

// Check if file exists
int fat32_file_exists(const char* path) {
    return fat32_find_file(boot_sector.root_cluster, path, 0) != 0;
}

// Get file size
uint32_t fat32_get_file_size(const char* path) {
    fat32_dir_entry_t entry;
    
    if (fat32_find_file(boot_sector.root_cluster, path, &entry) == 0) {
        return 0;
    }
    
    return entry.file_size;
}

// Helper: Set FAT entry
static int fat32_set_fat_entry(uint32_t cluster, uint32_t value) {
    uint32_t fat_offset = cluster * 4;
    uint32_t fat_sector = fat_start_sector + (fat_offset / FAT32_SECTOR_SIZE);
    uint32_t entry_offset = fat_offset % FAT32_SECTOR_SIZE;
    
    uint8_t* buffer = kmalloc(512);
    if (!buffer) return -1;
    
    if (disk_read_sectors(fat_sector, 1, buffer) != 0) {
        kfree(buffer);
        return -1;
    }
    
    uint32_t* entry = (uint32_t*)(buffer + entry_offset);
    *entry = (*entry & 0xF0000000) | (value & 0x0FFFFFFF);
    
    int result = disk_write_sectors(fat_sector, 1, buffer);
    
    // Write to backup FAT
    if (result == 0 && boot_sector.num_fats > 1) {
        uint32_t backup_fat_sector = fat_sector + boot_sector.fat_size_32;
        disk_write_sectors(backup_fat_sector, 1, buffer);
    }
    
    kfree(buffer);
    return result;
}

// Helper: Allocate a free cluster
static uint32_t fat32_alloc_cluster(void) {
    static uint32_t last_alloc = 2;
    
    uint32_t total_clusters = boot_sector.total_sectors_32 / boot_sector.sectors_per_cluster;
    
    for (uint32_t i = 0; i < total_clusters; i++) {
        uint32_t cluster = (last_alloc + i) % total_clusters;
        if (cluster < 2) cluster = 2;
        
        uint32_t entry = fat32_get_fat_entry(cluster);
        if (entry == 0) {
            fat32_set_fat_entry(cluster, 0x0FFFFFFF);
            last_alloc = cluster + 1;
            return cluster;
        }
    }
    
    return 0;
}

// Helper: Write cluster
static int fat32_write_cluster(uint32_t cluster, const uint8_t* buffer) {
    if (cluster < 2 || cluster >= 0x0FFFFFF8) {
        return -1;
    }
    
    uint32_t first_sector = data_start_sector + ((cluster - 2) * sectors_per_cluster);
    return disk_write_sectors(first_sector, sectors_per_cluster, (uint8_t*)buffer);
}

// Create a new file
int fat32_create_file(const char* path) {
    // Convert to uppercase
    char upper_path[256];
    int idx = 0;
    while (path[idx] && idx < 255) {
        char c = path[idx];
        upper_path[idx] = (c >= 'a' && c <= 'z') ? c - 32 : c;
        idx++;
    }
    upper_path[idx] = '\0';
    
    // Check if file exists
    if (fat32_file_exists(upper_path)) {
        return -1;
    }
    
    // Allocate cluster buffer
    uint8_t* cluster_buffer = kmalloc(bytes_per_cluster);
    if (!cluster_buffer) return -1;
    
    uint32_t cluster = boot_sector.root_cluster;
    
    // Read root directory
    if (fat32_read_cluster(cluster, cluster_buffer) != 0) {
        kfree(cluster_buffer);
        return -2;
    }
    
    fat32_dir_entry_t* entries = (fat32_dir_entry_t*)cluster_buffer;
    uint32_t entries_per_cluster = bytes_per_cluster / sizeof(fat32_dir_entry_t);
    
    // Find empty slot
    for (uint32_t i = 0; i < entries_per_cluster; i++) {
        if (entries[i].name[0] == 0x00 || entries[i].name[0] == 0xE5) {
            // Found empty slot - create entry
            uint8_t fat_name[11];
            fat32_string_to_fat_name(upper_path, fat_name);
            
            for (int j = 0; j < 11; j++) {
                entries[i].name[j] = fat_name[j];
            }
            
            entries[i].attributes = FAT_ATTR_ARCHIVE;
            entries[i].reserved = 0;
            entries[i].creation_time_tenths = 0;
            entries[i].creation_time = 0;
            entries[i].creation_date = 0;
            entries[i].last_access_date = 0;
            entries[i].first_cluster_high = 0;
            entries[i].last_mod_time = 0;
            entries[i].last_mod_date = 0;
            entries[i].first_cluster_low = 0;
            entries[i].file_size = 0;
            
            // Write directory back
            int result = fat32_write_cluster(cluster, cluster_buffer);
            kfree(cluster_buffer);
            return result;
        }
    }
    
    kfree(cluster_buffer);
    return -3;
}

// Write file contents
int fat32_write_file(const char* path, const uint8_t* buffer, uint32_t size) {
    fat32_dir_entry_t entry;
    uint32_t dir_cluster = boot_sector.root_cluster;
    
    // Convert to uppercase
    char upper_path[256];
    int idx = 0;
    while (path[idx] && idx < 255) {
        char c = path[idx];
        upper_path[idx] = (c >= 'a' && c <= 'z') ? c - 32 : c;
        idx++;
    }
    upper_path[idx] = '\0';
    
    uint32_t file_cluster = fat32_find_file(dir_cluster, upper_path, &entry);
    
    if (file_cluster == 0) {
        return -1;
    }
    
    // Allocate temporary cluster buffer
    uint8_t* temp_cluster = kmalloc(bytes_per_cluster);
    if (!temp_cluster) {
        return -3;
    }
    
    // Clear cluster buffer
    for (uint32_t i = 0; i < bytes_per_cluster; i++) {
        temp_cluster[i] = 0;
    }
    
    uint32_t bytes_written = 0;
    uint32_t current_cluster = file_cluster;
    uint32_t prev_cluster = 0;
    
    // If file has no clusters, allocate one
    if (current_cluster == 0) {
        current_cluster = fat32_alloc_cluster();
        if (current_cluster == 0) {
            kfree(temp_cluster);
            return -4;
        }
        entry.first_cluster_low = current_cluster & 0xFFFF;
        entry.first_cluster_high = (current_cluster >> 16) & 0xFFFF;
    }
    
    while (bytes_written < size) {
        uint32_t to_write = bytes_per_cluster;
        if (bytes_written + to_write > size) {
            to_write = size - bytes_written;
        }
        
        // Copy data to cluster buffer
        for (uint32_t i = 0; i < to_write; i++) {
            temp_cluster[i] = buffer[bytes_written + i];
        }
        
        // Write cluster
        if (fat32_write_cluster(current_cluster, temp_cluster) != 0) {
            kfree(temp_cluster);
            return -2;
        }
        
        bytes_written += to_write;
        
        if (bytes_written < size) {
            // Need more clusters
            prev_cluster = current_cluster;
            current_cluster = fat32_get_fat_entry(current_cluster);
            
            if (current_cluster >= 0x0FFFFFF8) {
                // Allocate new cluster
                current_cluster = fat32_alloc_cluster();
                if (current_cluster == 0) {
                    kfree(temp_cluster);
                    return -4;
                }
                fat32_set_fat_entry(prev_cluster, current_cluster);
            }
        }
    }
    
    // Update file size in directory
    uint8_t* dir_buffer = kmalloc(bytes_per_cluster);
    if (!dir_buffer) {
        kfree(temp_cluster);
        return -5;
    }
    
    if (fat32_read_cluster(dir_cluster, dir_buffer) == 0) {
        fat32_dir_entry_t* dir_entries = (fat32_dir_entry_t*)dir_buffer;
        uint32_t entries_per_cluster = bytes_per_cluster / sizeof(fat32_dir_entry_t);
        
        uint8_t fat_name[11];
        fat32_string_to_fat_name(upper_path, fat_name);
        
        for (uint32_t i = 0; i < entries_per_cluster; i++) {
            int match = 1;
            for (int j = 0; j < 11; j++) {
                if (dir_entries[i].name[j] != fat_name[j]) {
                    match = 0;
                    break;
                }
            }
            
            if (match) {
                dir_entries[i].file_size = size;
                fat32_write_cluster(dir_cluster, dir_buffer);
                break;
            }
        }
    }
    
    kfree(dir_buffer);
    kfree(temp_cluster);
    return size;
}

// Delete file
int fat32_delete_file(const char* path) {
    fat32_dir_entry_t entry;
    uint32_t dir_cluster = boot_sector.root_cluster;
    
    // Convert to uppercase
    char upper_path[256];
    int idx = 0;
    while (path[idx] && idx < 255) {
        char c = path[idx];
        upper_path[idx] = (c >= 'a' && c <= 'z') ? c - 32 : c;
        idx++;
    }
    upper_path[idx] = '\0';
    
    uint32_t file_cluster = fat32_find_file(dir_cluster, upper_path, &entry);
    
    if (file_cluster == 0) {
        return -1;
    }
    
    // Free clusters
    uint32_t cluster = file_cluster;
    while (cluster < 0x0FFFFFF8) {
        uint32_t next = fat32_get_fat_entry(cluster);
        fat32_set_fat_entry(cluster, 0);
        cluster = next;
    }
    
    // Mark directory entry as deleted
    uint8_t* dir_buffer = kmalloc(bytes_per_cluster);
    if (!dir_buffer) return -2;
    
    if (fat32_read_cluster(dir_cluster, dir_buffer) == 0) {
        fat32_dir_entry_t* entries = (fat32_dir_entry_t*)dir_buffer;
        uint32_t entries_per_cluster = bytes_per_cluster / sizeof(fat32_dir_entry_t);
        
        uint8_t fat_name[11];
        fat32_string_to_fat_name(upper_path, fat_name);
        
        for (uint32_t i = 0; i < entries_per_cluster; i++) {
            int match = 1;
            for (int j = 0; j < 11; j++) {
                if (entries[i].name[j] != fat_name[j]) {
                    match = 0;
                    break;
                }
            }
            
            if (match) {
                entries[i].name[0] = 0xE5;
                fat32_write_cluster(dir_cluster, dir_buffer);
                break;
            }
        }
    }
    
    kfree(dir_buffer);
    return 0;
}

// Helper: Parse path into components
static int parse_path(const char* path, path_component_t* components, uint32_t* count) {
    *count = 0;
    
    if (path[0] == '/') {
        // Absolute path
        current_directory_cluster = boot_sector.root_cluster;
        path++;
    }
    
    char component[256];
    int comp_idx = 0;
    
    while (*path) {
        if (*path == '/') {
            if (comp_idx > 0) {
                component[comp_idx] = '\0';
                
                // Convert to uppercase
                for (int i = 0; i < comp_idx; i++) {
                    components[*count].name[i] = 
                        (component[i] >= 'a' && component[i] <= 'z') 
                        ? component[i] - 32 : component[i];
                }
                components[*count].name[comp_idx] = '\0';
                (*count)++;
                comp_idx = 0;
            }
            path++;
        } else {
            component[comp_idx++] = *path++;
            if (comp_idx >= 255) return -1;
        }
    }
    
    if (comp_idx > 0) {
        component[comp_idx] = '\0';
        for (int i = 0; i < comp_idx; i++) {
            components[*count].name[i] = 
                (component[i] >= 'a' && component[i] <= 'z') 
                ? component[i] - 32 : component[i];
        }
        components[*count].name[comp_idx] = '\0';
        (*count)++;
    }
    
    return 0;
}

// Helper: Find directory entry
static uint32_t find_directory(uint32_t parent_cluster, const char* name) {
    uint8_t* cluster_buffer = kmalloc(bytes_per_cluster);
    if (!cluster_buffer) return 0;
    
    uint8_t fat_name[11];
    fat32_string_to_fat_name(name, fat_name);
    
    uint32_t cluster = parent_cluster;
    
    while (cluster < 0x0FFFFFF8) {
        if (fat32_read_cluster(cluster, cluster_buffer) != 0) {
            kfree(cluster_buffer);
            return 0;
        }
        
        fat32_dir_entry_t* entries = (fat32_dir_entry_t*)cluster_buffer;
        uint32_t entries_per_cluster = bytes_per_cluster / sizeof(fat32_dir_entry_t);
        
        for (uint32_t i = 0; i < entries_per_cluster; i++) {
            if (entries[i].name[0] == 0x00) {
                kfree(cluster_buffer);
                return 0;
            }
            
            if (entries[i].name[0] == 0xE5 || 
                !(entries[i].attributes & FAT_ATTR_DIRECTORY)) {
                continue;
            }
            
            int match = 1;
            for (int j = 0; j < 11; j++) {
                if (entries[i].name[j] != fat_name[j]) {
                    match = 0;
                    break;
                }
            }
            
            if (match) {
                uint32_t result = (entries[i].first_cluster_high << 16) | 
                                  entries[i].first_cluster_low;
                kfree(cluster_buffer);
                return result;
            }
        }
        
        cluster = fat32_get_fat_entry(cluster);
    }
    
    kfree(cluster_buffer);
    return 0;
}

// Navigate path and return final cluster
static uint32_t navigate_path(const char* path, uint32_t* final_cluster) {
    path_component_t components[MAX_PATH_DEPTH];
    uint32_t count = 0;
    
    if (parse_path(path, components, &count) != 0) {
        return -1;
    }
    
    uint32_t cluster = (path[0] == '/') ? boot_sector.root_cluster : current_directory_cluster;
    
    for (uint32_t i = 0; i < count; i++) {
        if (strcmp(components[i].name, ".") == 0) {
            continue;
        } else if (strcmp(components[i].name, "..") == 0) {
            // Go up one level (simplified - would need parent tracking)
            cluster = boot_sector.root_cluster;
        } else {
            cluster = find_directory(cluster, components[i].name);
            if (cluster == 0) {
                return -1;
            }
        }
    }
    
    *final_cluster = cluster;
    return 0;
}

// Change directory
int fat32_change_directory(const char* path) {
    uint32_t new_cluster;
    
    if (navigate_path(path, &new_cluster) != 0) {
        return -1;
    }
    
    current_directory_cluster = new_cluster;
    
    // Update current path
    if (path[0] == '/') {
        // Absolute path
        int len = 0;
        while (path[len] && len < FAT32_MAX_PATH - 1) {
            current_path[len] = path[len];
            len++;
        }
        current_path[len] = '\0';
    } else {
        // Relative path - append
        int len = 0;
        while (current_path[len]) len++;
        
        if (len > 0 && current_path[len-1] != '/') {
            current_path[len++] = '/';
        }
        
        int i = 0;
        while (path[i] && len < FAT32_MAX_PATH - 1) {
            current_path[len++] = path[i++];
        }
        current_path[len] = '\0';
    }
    
    return 0;
}

// Get current directory
int fat32_get_current_directory(char* buffer, uint32_t size) {
    int len = 0;
    while (current_path[len] && len < size - 1) {
        buffer[len] = current_path[len];
        len++;
    }
    buffer[len] = '\0';
    return len;
}

// Create directory
int fat32_mkdir(const char* path) {
    char upper_path[256];
    int idx = 0;
    while (path[idx] && idx < 255) {
        char c = path[idx];
        upper_path[idx] = (c >= 'a' && c <= 'z') ? c - 32 : c;
        idx++;
    }
    upper_path[idx] = '\0';
    
    // Allocate new cluster for directory
    uint32_t new_cluster = fat32_alloc_cluster();
    if (new_cluster == 0) {
        return -1;
    }
    
    // Initialize directory cluster with . and ..
    uint8_t* dir_buffer = kmalloc(bytes_per_cluster);
    if (!dir_buffer) {
        fat32_set_fat_entry(new_cluster, 0);
        return -2;
    }
    
    for (uint32_t i = 0; i < bytes_per_cluster; i++) {
        dir_buffer[i] = 0;
    }
    
    fat32_dir_entry_t* entries = (fat32_dir_entry_t*)dir_buffer;
    
    // Create "." entry
    for (int i = 0; i < 11; i++) {
        entries[0].name[i] = (i == 0) ? '.' : ' ';
    }
    entries[0].attributes = FAT_ATTR_DIRECTORY;
    entries[0].first_cluster_low = new_cluster & 0xFFFF;
    entries[0].first_cluster_high = (new_cluster >> 16) & 0xFFFF;
    
    // Create ".." entry
    for (int i = 0; i < 11; i++) {
        entries[1].name[i] = (i < 2) ? '.' : ' ';
    }
    entries[1].attributes = FAT_ATTR_DIRECTORY;
    entries[1].first_cluster_low = current_directory_cluster & 0xFFFF;
    entries[1].first_cluster_high = (current_directory_cluster >> 16) & 0xFFFF;
    
    fat32_write_cluster(new_cluster, dir_buffer);
    kfree(dir_buffer);
    
    // Add entry to parent directory
    uint8_t* parent_buffer = kmalloc(bytes_per_cluster);
    if (!parent_buffer) {
        return -3;
    }
    
    if (fat32_read_cluster(current_directory_cluster, parent_buffer) == 0) {
        fat32_dir_entry_t* parent_entries = (fat32_dir_entry_t*)parent_buffer;
        uint32_t entries_per_cluster = bytes_per_cluster / sizeof(fat32_dir_entry_t);
        
        for (uint32_t i = 0; i < entries_per_cluster; i++) {
            if (parent_entries[i].name[0] == 0x00 || parent_entries[i].name[0] == 0xE5) {
                uint8_t fat_name[11];
                fat32_string_to_fat_name(upper_path, fat_name);
                
                for (int j = 0; j < 11; j++) {
                    parent_entries[i].name[j] = fat_name[j];
                }
                
                parent_entries[i].attributes = FAT_ATTR_DIRECTORY;
                parent_entries[i].first_cluster_low = new_cluster & 0xFFFF;
                parent_entries[i].first_cluster_high = (new_cluster >> 16) & 0xFFFF;
                parent_entries[i].file_size = 0;
                
                fat32_write_cluster(current_directory_cluster, parent_buffer);
                break;
            }
        }
    }
    
    kfree(parent_buffer);
    return 0;
}

// Enhanced list directory with path support
int fat32_list_directory_ex(const char* path, fat32_file_info_t* files, uint32_t max_files) {
    uint32_t target_cluster;
    
    if (path == NULL || path[0] == '\0') {
        target_cluster = current_directory_cluster ? current_directory_cluster : boot_sector.root_cluster;
    } else {
        if (navigate_path(path, &target_cluster) != 0) {
            return -1;
        }
    }
    
    uint8_t* cluster_buffer = kmalloc(bytes_per_cluster);
    if (!cluster_buffer) return -1;
    
    uint32_t cluster = target_cluster;
    uint32_t file_count = 0;
    
    while (cluster < 0x0FFFFFF8 && file_count < max_files) {
        if (fat32_read_cluster(cluster, cluster_buffer) != 0) {
            break;
        }
        
        fat32_dir_entry_t* entries = (fat32_dir_entry_t*)cluster_buffer;
        uint32_t entries_per_cluster = bytes_per_cluster / sizeof(fat32_dir_entry_t);
        
        for (uint32_t i = 0; i < entries_per_cluster && file_count < max_files; i++) {
            if (entries[i].name[0] == 0x00) {
                kfree(cluster_buffer);
                return file_count;
            }
            
            if (entries[i].name[0] == 0xE5 || 
                entries[i].attributes & FAT_ATTR_LONG_NAME) {
                continue;
            }
            
            fat32_name_to_string(entries[i].name, files[file_count].name);
            files[file_count].size = entries[i].file_size;
            files[file_count].first_cluster = 
                (entries[i].first_cluster_high << 16) | entries[i].first_cluster_low;
            files[file_count].attributes = entries[i].attributes;
            files[file_count].is_directory = 
                (entries[i].attributes & FAT_ATTR_DIRECTORY) ? 1 : 0;
            
            file_count++;
        }
        
        cluster = fat32_get_fat_entry(cluster);
    }
    
     kfree(cluster_buffer);
    return file_count;
}