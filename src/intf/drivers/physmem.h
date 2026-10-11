// physmem.h - the rest of RAM: page frames for user programs
//
// The kernel heap is one region below 1 GiB (device DMA needs low
// addresses). Every other usable 2 MiB of RAM, including everything above
// 4 GiB, is handed out here as 4 KiB pages. The kernel reaches all of them
// through the direct map (drivers/addr.h): phys_to_virt(page).
#ifndef PHYSMEM_H
#define PHYSMEM_H

#include <stdint.h>
#include "drivers/addr.h"

/* Map and take over all usable RAM outside [heap_start, heap_end) and the
 * first 16 MiB. Call after paging_init and heap_init, before any program. */
void physmem_init(uint64_t heap_start, uint64_t heap_end);

uint64_t page_alloc(void);           /* a free 4 KiB page (physical address), 0 if none */
void page_free(uint64_t phys);
int page_owned(uint64_t phys);       /* did page_alloc hand this out (not the heap)? */

/* The kernel's own address for a user page (from here or the heap). */
static inline void *page_address(uint64_t phys) {
    return phys_to_virt(phys);
}

uint64_t physmem_total(void);        /* bytes */
uint64_t physmem_used(void);

#endif
