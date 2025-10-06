// src/intf/memory.h
#ifndef MEMORY_H
#define MEMORY_H

#include <stdint.h>

#define PAGE_SIZE 4096  // 4 KB

void memory_init(uint64_t mem_upper); // initialize memory manager
void* alloc_frame();
void free_frame(void* frame);
uint64_t get_total_memory();

#endif
