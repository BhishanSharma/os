#include "elf.h"
#include "fat32.h"
#include "heap.h"
#include "print.h"
#include "paging.h"
#include "string.h"

// Validate ELF header
static int elf_validate(elf64_ehdr_t* header) {
    // Check magic number
    if (*(uint32_t*)header->e_ident != ELF_MAGIC) {
        return -1;
    }
    
    // Check 64-bit
    if (header->e_ident[4] != 2) {
        return -2;
    }
    
    // Check little endian
    if (header->e_ident[5] != 1) {
        return -3;
    }
    
    // Check x86-64
    if (header->e_machine != 0x3E) {
        return -4;
    }
    
    return 0;
}

// Load ELF file into memory
int elf_load(const char* path) {
    // Read file
    uint32_t file_size = fat32_get_file_size(path);
    if (file_size == 0) {
        print_str("File not found\n");
        return -1;
    }
    
    uint8_t* file_buffer = kmalloc(file_size);
    if (!file_buffer) {
        print_str("Out of memory\n");
        return -2;
    }
    
    int bytes = fat32_read_file(path, file_buffer, file_size);
    if (bytes < 0) {
        kfree(file_buffer);
        print_str("Failed to read file\n");
        return -3;
    }
    
    // Validate ELF header
    elf64_ehdr_t* ehdr = (elf64_ehdr_t*)file_buffer;
    if (elf_validate(ehdr) != 0) {
        kfree(file_buffer);
        print_str("Invalid ELF file\n");
        return -4;
    }
    
    kprintf("ELF Entry point: %lx\n", ehdr->e_entry);
    kprintf("Program headers: %d\n", ehdr->e_phnum);
    
    // Load program segments
    elf64_phdr_t* phdr = (elf64_phdr_t*)(file_buffer + ehdr->e_phoff);
    
    for (int i = 0; i < ehdr->e_phnum; i++) {
        if (phdr[i].p_type == PT_LOAD) {
            kprintf("Loading segment %d: vaddr=%lx size=%lu\n", 
                    i, phdr[i].p_vaddr, phdr[i].p_memsz);
            
            // Allocate memory for segment
            uint8_t* segment_mem = (uint8_t*)phdr[i].p_vaddr;
            
            // Map pages for this segment
            for (uint64_t addr = phdr[i].p_vaddr; 
                 addr < phdr[i].p_vaddr + phdr[i].p_memsz; 
                 addr += 0x1000) {
                uint64_t phys = (uint64_t)kmalloc(0x1000);
                if (phys) {
                    map_page(addr, phys, PAGE_PRESENT | PAGE_RW | PAGE_USER);
                }
            }
            
            // Copy segment data
            for (uint64_t j = 0; j < phdr[i].p_filesz; j++) {
                segment_mem[j] = file_buffer[phdr[i].p_offset + j];
            }
            
            // Zero out BSS section
            for (uint64_t j = phdr[i].p_filesz; j < phdr[i].p_memsz; j++) {
                segment_mem[j] = 0;
            }
        }
    }
    
    kfree(file_buffer);
    return 0;
}

// Execute ELF program
int elf_exec(const char* path) {
    // Read file
    uint32_t file_size = fat32_get_file_size(path);
    if (file_size == 0) {
        print_str("File not found\n");
        return -1;
    }
    
    uint8_t* file_buffer = kmalloc(file_size);
    if (!file_buffer) {
        print_str("Out of memory\n");
        return -2;
    }
    
    int bytes = fat32_read_file(path, file_buffer, file_size);
    if (bytes < 0) {
        kfree(file_buffer);
        print_str("Failed to read file\n");
        return -3;
    }
    
    elf64_ehdr_t* ehdr = (elf64_ehdr_t*)file_buffer;
    if (elf_validate(ehdr) != 0) {
        kfree(file_buffer);
        print_str("Invalid ELF file\n");
        return -4;
    }
    
    // Load segments
    elf64_phdr_t* phdr = (elf64_phdr_t*)(file_buffer + ehdr->e_phoff);
    
    for (int i = 0; i < ehdr->e_phnum; i++) {
        if (phdr[i].p_type == PT_LOAD) {
            uint8_t* segment_mem = (uint8_t*)phdr[i].p_vaddr;
            
            // Map and copy data
            for (uint64_t addr = phdr[i].p_vaddr; 
                 addr < phdr[i].p_vaddr + phdr[i].p_memsz; 
                 addr += 0x1000) {
                uint64_t phys = (uint64_t)kmalloc(0x1000);
                if (phys) {
                    map_page(addr, phys, PAGE_PRESENT | PAGE_RW | PAGE_USER);
                }
            }
            
            for (uint64_t j = 0; j < phdr[i].p_filesz; j++) {
                segment_mem[j] = file_buffer[phdr[i].p_offset + j];
            }
            
            for (uint64_t j = phdr[i].p_filesz; j < phdr[i].p_memsz; j++) {
                segment_mem[j] = 0;
            }
        }
    }
    
    // Get entry point
    void (*entry)(void) = (void (*)(void))ehdr->e_entry;
    
    kprintf("Executing program at %lx\n", ehdr->e_entry);
    kfree(file_buffer);
    
    // Jump to program
    entry();
    
    return 0;
}