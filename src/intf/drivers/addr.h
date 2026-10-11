// addr.h - the kernel's view of memory (see docs/ARCHITECTURE.md)
//
//   0x0000000000000000 - 0x00007FFFFFFFFFFF   user programs (each has its own)
//   0xFFFF800000000000                        direct map: physical address X is at
//                                             DIRECT_MAP + X (all RAM, boot modules,
//                                             the framebuffer)
//   0xFFFFC00000000000                        device registers (mmio_map)
//   0xFFFFC08000000000                        ACPI table window (acpi.c)
//   0xFFFFFFFF80000000                        the kernel image: KERNEL_VMA + X
//
// Devices see physical addresses: hand them virt_to_phys(buffer), never the
// pointer itself.
#ifndef ADDR_H
#define ADDR_H

#include <stdint.h>

#define DIRECT_MAP  0xFFFF800000000000ULL
#define MMIO_WINDOW 0xFFFFC00000000000ULL
#define ACPI_WINDOW 0xFFFFC08000000000ULL
#define KERNEL_VMA  0xFFFFFFFF80000000ULL
#define USER_SPACE_END 0x0000800000000000ULL

/* Physical memory through the direct map. */
static inline void *phys_to_virt(uint64_t phys) { return (void *)(phys + DIRECT_MAP); }

/* The physical address of kernel memory: the heap, the kernel image (static
 * buffers, the boot stack) or a page from the direct map. */
static inline uint64_t virt_to_phys(const volatile void *p) {
    uint64_t a = (uint64_t)p;
    return a >= KERNEL_VMA ? a - KERNEL_VMA : a - DIRECT_MAP;
}

#endif
