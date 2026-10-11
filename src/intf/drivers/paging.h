// src/intf/paging.h
#ifndef PAGING_H
#define PAGING_H

#include <stdint.h>
#include "drivers/addr.h"

#define PAGE_PRESENT   0x1
#define PAGE_RW        0x2
#define PAGE_USER      0x4
#define PAGE_SIZE_2MB  0x80

#define PAGE_SIZE 4096

/* Build the kernel's page tables and switch to them: the kernel image at
 * KERNEL_VMA + [image_start, image_end) and, in the direct map, the RAM-like
 * regions of the memory map, the heap and every range given to
 * paging_add_direct_region(). */
void paging_init(uint64_t image_start, uint64_t image_end, uint64_t heap_start, uint64_t heap_size);
void map_page(uint64_t virt, uint64_t phys, uint64_t flags);

/* Map device registers at [phys, phys + size), uncached, into the kernel's
 * device window; returns where they are. */
void *mmio_map(uint64_t phys, uint64_t size);
/* Write-combining (the framebuffer); paging_pat_init() on each core first. */
void *mmio_map_wc(uint64_t phys, uint64_t size);
void paging_pat_init(void);
void *paging_map_pages_wc(const uint64_t *phys, uint64_t n);
#define LARGE_PAGE 0x200000ULL
void map_large_page(uint64_t virt, uint64_t phys);   // 2 MiB, both 2 MiB aligned

/* Register a physical range (the framebuffer, a boot module) for paging_init()
 * to put in the direct map. Call before paging_init(). */
void paging_add_direct_region(uint64_t base, uint64_t size);

/* The page-table entry for `virt` (flags in the low 12 bits), the 2 MiB PD
 * entry if a large page maps it, or 0 if it is not mapped. */
uint64_t paging_get_entry(uint64_t virt);

/* Remove the 4 KiB mapping of `virt`. Returns the physical page, or 0 if it
 * was not mapped. */
uint64_t unmap_page(uint64_t virt);

/* Address spaces for user programs: the whole lower half is the program's,
 * the upper half (PML4 entries 256-511) is the kernel's, shared by all.
 * Roots are physical addresses of PML4 tables (what CR3 holds). */
uint64_t paging_kernel_root(void);
uint64_t paging_create_address_space(void);          /* 0 if out of memory */
int paging_map_user(uint64_t root, uint64_t virt, uint64_t phys);   /* 0 on success */
uint64_t paging_get_user_entry(uint64_t root, uint64_t virt);       /* 0 if unmapped */
int paging_map_user_prot(uint64_t root, uint64_t virt, uint64_t phys, int writable);
uint64_t *paging_user_pte(uint64_t root, uint64_t virt, int create);  /* the entry itself */
void paging_walk_user(uint64_t root, void (*fn)(uint64_t virt, uint64_t *pte, void *ctx), void *ctx);
void paging_flush(void);                             /* reload CR3: forget cached entries */
#define PAGE_COW 0x200                               /* software bit: copy on write */
void paging_free_address_space(uint64_t root);       /* also frees every mapped page */
void paging_switch(uint64_t root);                   /* load CR3 */

/* An address space for calling the firmware (drivers/efi.c): the kernel's
 * mappings plus identity mappings of these physical ranges (below 512 GiB).
 * The kernel's own tables are not changed. 0 if out of memory. */
typedef struct { uint64_t start, size; int uncached; } paging_range_t;
uint64_t paging_create_identity_space(const paging_range_t *ranges, int count);

#endif
