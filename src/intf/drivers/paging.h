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
#define LARGE_PAGE 0x200000ULL
void map_large_page(uint64_t virt, uint64_t phys);   // 2 MiB, both 2 MiB aligned

/* Register a physical range (e.g. the framebuffer) for paging_init() to identity-map.
 * Call before paging_init(). */
void paging_add_identity_region(uint64_t base, uint64_t size);

/* The page-table entry for `virt` (flags in the low 12 bits), the 2 MiB PD
 * entry if a large page maps it, or 0 if it is not mapped. */
uint64_t paging_get_entry(uint64_t virt);

/* Remove the 4 KiB mapping of `virt`. Returns the physical page, or 0 if it
 * was not mapped. */
uint64_t unmap_page(uint64_t virt);

#endif
