// physmem.h - the rest of RAM: page frames for user programs
//
// The kernel heap is one identity-mapped region below 1 GiB (device DMA
// needs low addresses). Every other usable 2 MiB of RAM, including the
// 1-2 GiB range and everything above 4 GiB, is handed out here as 4 KiB
// pages. The kernel reaches them through the direct map: each such page is
// mapped at DIRECT_MAP_BASE + its physical address.
#ifndef PHYSMEM_H
#define PHYSMEM_H

#include <stdint.h>

#define DIRECT_MAP_BASE 0x0000020000000000ULL      /* 2 TiB (PML4 entry 4) */

static inline void *phys_to_virt(uint64_t phys) { return (void *)(phys + DIRECT_MAP_BASE); }

/* Map and take over all usable RAM outside [heap_start, heap_end) and the
 * first 16 MiB. Call after paging_init and heap_init, before any program. */
void physmem_init(uint64_t heap_start, uint64_t heap_end);

uint64_t page_alloc(void);           /* a free 4 KiB page (physical address), 0 if none */
void page_free(uint64_t phys);
int page_owned(uint64_t phys);       /* did page_alloc hand this out (not the heap)? */

/* The kernel's own address for a user page: direct map or identity (heap). */
static inline void *page_address(uint64_t phys) {
    return page_owned(phys) ? phys_to_virt(phys) : (void *)phys;
}

uint64_t physmem_total(void);        /* bytes */
uint64_t physmem_used(void);

#endif
