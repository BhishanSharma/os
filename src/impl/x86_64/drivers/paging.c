#include "paging.h"
#include "memory.h"
#include <stdint.h>

typedef uint64_t page_entry_t;

static page_entry_t* pml4;

#define PAGE_TABLE_AREA 0x300000
static uint64_t next_table = PAGE_TABLE_AREA;

static void* alloc_table() {
    void* t = (void*)next_table;
    next_table += 0x1000;
    for (int i = 0; i < 512; i++) ((uint64_t*)t)[i] = 0;
    return t;
}

void paging_init(uint64_t phys_base, uint64_t phys_end,
                 uint64_t heap_start, uint64_t heap_size) {
    pml4 = (page_entry_t*)alloc_table();

    // IMPORTANT: Pre-allocate and identity map ALL page tables we'll need
    // Calculate how many page tables we need for kernel
    uint64_t kernel_pages = (phys_end - phys_base) / PAGE_SIZE;
    uint64_t heap_pages = heap_size / PAGE_SIZE;
    uint64_t table_pages = (next_table - PAGE_TABLE_AREA) / PAGE_SIZE;
    
    // Estimate total tables needed (rough upper bound)
    uint64_t total_pages = kernel_pages + heap_pages + table_pages + 100; // +100 for safety
    uint64_t tables_needed = (total_pages / 512) + 10; // Pages per table + overhead
    
    // Pre-allocate tables
    for (uint64_t i = 0; i < tables_needed; i++) {
        alloc_table();
    }
    
    // NOW identity map everything BEFORE enabling paging
    // Identity map kernel
    for (uint64_t addr = phys_base; addr < phys_end; addr += PAGE_SIZE) {
        map_page(addr, addr, PAGE_PRESENT | PAGE_RW);
    }

    // Identity map heap
    for (uint64_t addr = heap_start; addr < heap_start + heap_size; addr += PAGE_SIZE) {
        map_page(addr, addr, PAGE_PRESENT | PAGE_RW);
    }

    // Identity map ALL page tables (including those we just allocated)
    for (uint64_t addr = PAGE_TABLE_AREA; addr < next_table; addr += PAGE_SIZE) {
        map_page(addr, addr, PAGE_PRESENT | PAGE_RW);
    }

    // Identity map video memory (0xB8000)
    map_page(0xB8000, 0xB8000, PAGE_PRESENT | PAGE_RW);

    asm volatile("cli");

    // Enable PAE (CR4.PAE = bit 5)
    uint64_t cr4;
    asm volatile("mov %%cr4, %0" : "=r"(cr4));
    cr4 |= (1UL << 5);
    asm volatile("mov %0, %%cr4" :: "r"(cr4));

    // Load PML4
    asm volatile("mov %0, %%cr3" :: "r"((uint64_t)pml4));

    // Enable paging (CR0.PG = bit 31)
    uint64_t cr0;
    asm volatile("mov %%cr0, %0" : "=r"(cr0));
    cr0 |= (1UL << 31);
    asm volatile("mov %0, %%cr0" :: "r"(cr0));

    asm volatile("sti");
}

void map_page(uint64_t virt, uint64_t phys, uint64_t flags) {
    uint64_t pml4_idx = (virt >> 39) & 0x1FF;
    uint64_t pdpt_idx = (virt >> 30) & 0x1FF;
    uint64_t pd_idx   = (virt >> 21) & 0x1FF;
    uint64_t pt_idx   = (virt >> 12) & 0x1FF;

    page_entry_t *pdpt, *pd, *pt;

    // Get or create PDPT
    if (!(pml4[pml4_idx] & PAGE_PRESENT)) {
        pdpt = (page_entry_t*)alloc_table();
        pml4[pml4_idx] = ((uint64_t)pdpt) | PAGE_PRESENT | PAGE_RW;
    } else {
        // FIX: Since we identity-mapped, phys addr = virt addr
        pdpt = (page_entry_t*)(pml4[pml4_idx] & ~0xFFF);
    }

    // Get or create PD
    if (!(pdpt[pdpt_idx] & PAGE_PRESENT)) {
        pd = (page_entry_t*)alloc_table();
        pdpt[pdpt_idx] = ((uint64_t)pd) | PAGE_PRESENT | PAGE_RW;
    } else {
        pd = (page_entry_t*)(pdpt[pdpt_idx] & ~0xFFF);
    }

    // Get or create PT
    if (!(pd[pd_idx] & PAGE_PRESENT)) {
        pt = (page_entry_t*)alloc_table();
        pd[pd_idx] = ((uint64_t)pt) | PAGE_PRESENT | PAGE_RW;
    } else {
        pt = (page_entry_t*)(pd[pd_idx] & ~0xFFF);
    }

    // Map the actual page
    pt[pt_idx] = (phys & ~0xFFF) | (flags & 0xFFF) | PAGE_PRESENT;
}