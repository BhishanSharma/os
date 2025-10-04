// src/intf/paging.h
#ifndef PAGING_H
#define PAGING_H

#include <stdint.h>

#define PAGE_PRESENT   0x1
#define PAGE_RW        0x2
#define PAGE_USER      0x4
#define PAGE_SIZE_2MB  0x80

#define PAGE_SIZE 4096

void paging_init(uint64_t phys_base, uint64_t phys_end, uint64_t heap_start, uint64_t heap_size);
void map_page(uint64_t virt, uint64_t phys, uint64_t flags);

#endif
