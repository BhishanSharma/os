#include "drivers/paging.h"
#include "drivers/memory.h"
#include "core/exceptions.h"
#include <stdint.h>

typedef uint64_t page_entry_t;

extern char stack_guard[];   // boot stack guard page (main.asm): must stay unmapped

static page_entry_t* pml4;

// Page tables come from a fixed pool between the kernel image (ends below
// 0x200000, see linker.ld) and the heap (starts at or above 0x400000).
#define PAGE_TABLE_AREA 0x200000
#define PAGE_TABLE_END  0x400000
static uint64_t next_table = PAGE_TABLE_AREA;

/* Extra physical ranges (e.g. the framebuffer) to identity-map in paging_init. */
#define MAX_EXTRA_REGIONS 4
static struct { uint64_t base, size; } extra_regions[MAX_EXTRA_REGIONS];
static int extra_count;

void paging_add_identity_region(uint64_t base, uint64_t size) {
    if (extra_count < MAX_EXTRA_REGIONS && size) {
        extra_regions[extra_count].base = base;
        extra_regions[extra_count].size = size;
        extra_count++;
    }
}

static void* alloc_table() {
    if (next_table >= PAGE_TABLE_END) kpanic("page table pool exhausted");
    void* t = (void*)next_table;
    next_table += 0x1000;
    for (int i = 0; i < 512; i++) ((uint64_t*)t)[i] = 0;
    return t;
}

void paging_init(uint64_t phys_base, uint64_t phys_end,
                 uint64_t heap_start, uint64_t heap_size) {
    pml4 = (page_entry_t*)alloc_table();

    // Identity map everything BEFORE switching to these tables. Tables are
    // allocated on demand; the last loop below maps them all, including any
    // it allocates itself.
    // Identity map kernel
    for (uint64_t addr = phys_base; addr < phys_end; addr += PAGE_SIZE) {
        if (addr == (uint64_t)stack_guard) continue;   // guard page stays unmapped
        map_page(addr, addr, PAGE_PRESENT | PAGE_RW);
    }

    // Identity map heap: 2 MiB pages where aligned (the heap can be up to 1 GiB)
    for (uint64_t addr = heap_start; addr < heap_start + heap_size; ) {
        if ((addr & (LARGE_PAGE - 1)) == 0 && heap_start + heap_size - addr >= LARGE_PAGE) {
            map_large_page(addr, addr);
            addr += LARGE_PAGE;
        } else {
            map_page(addr, addr, PAGE_PRESENT | PAGE_RW);
            addr += PAGE_SIZE;
        }
    }

    // Identity map registered device regions (framebuffer)
    for (int i = 0; i < extra_count; i++) {
        uint64_t start = extra_regions[i].base & ~(uint64_t)(PAGE_SIZE - 1);
        uint64_t end = extra_regions[i].base + extra_regions[i].size;
        for (uint64_t addr = start; addr < end; addr += PAGE_SIZE)
            map_page(addr, addr, PAGE_PRESENT | PAGE_RW);
    }

    // Identity map the whole page-table pool, not just the tables allocated so
    // far: tables created later (user programs' mappings) must be reachable too.
    for (uint64_t addr = PAGE_TABLE_AREA; addr < PAGE_TABLE_END; addr += PAGE_SIZE) {
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

// Page directory for `virt`, creating the PDPT and PD if needed.
static page_entry_t* get_pd(uint64_t virt) {
    uint64_t pml4_idx = (virt >> 39) & 0x1FF;
    uint64_t pdpt_idx = (virt >> 30) & 0x1FF;
    if (!(pml4[pml4_idx] & PAGE_PRESENT))
        pml4[pml4_idx] = (uint64_t)alloc_table() | PAGE_PRESENT | PAGE_RW;
    page_entry_t* pdpt = (page_entry_t*)(pml4[pml4_idx] & ~0xFFFULL);
    if (!(pdpt[pdpt_idx] & PAGE_PRESENT))
        pdpt[pdpt_idx] = (uint64_t)alloc_table() | PAGE_PRESENT | PAGE_RW;
    return (page_entry_t*)(pdpt[pdpt_idx] & ~0xFFFULL);
}

void map_large_page(uint64_t virt, uint64_t phys) {
    page_entry_t* pd = get_pd(virt);
    pd[(virt >> 21) & 0x1FF] = (phys & ~(LARGE_PAGE - 1)) | PAGE_PRESENT | PAGE_RW | PAGE_SIZE_2MB;
}

void map_page(uint64_t virt, uint64_t phys, uint64_t flags) {
    uint64_t pml4_idx = (virt >> 39) & 0x1FF;
    uint64_t pdpt_idx = (virt >> 30) & 0x1FF;
    uint64_t pd_idx   = (virt >> 21) & 0x1FF;
    uint64_t pt_idx   = (virt >> 12) & 0x1FF;
    // A user page needs the user bit at every level; kernel pages under the
    // same upper entries stay protected because their own entries lack it.
    uint64_t upper = PAGE_PRESENT | PAGE_RW | (flags & PAGE_USER);

    page_entry_t *pdpt, *pd, *pt;

    // Get or create PDPT
    if (!(pml4[pml4_idx] & PAGE_PRESENT)) {
        pdpt = (page_entry_t*)alloc_table();
        pml4[pml4_idx] = ((uint64_t)pdpt) | upper;
    } else {
        // Page tables are identity mapped: physical address = virtual address
        pml4[pml4_idx] |= upper;
        pdpt = (page_entry_t*)(pml4[pml4_idx] & ~0xFFFULL);
    }

    // Get or create PD
    if (!(pdpt[pdpt_idx] & PAGE_PRESENT)) {
        pd = (page_entry_t*)alloc_table();
        pdpt[pdpt_idx] = ((uint64_t)pd) | upper;
    } else {
        pdpt[pdpt_idx] |= upper;
        pd = (page_entry_t*)(pdpt[pdpt_idx] & ~0xFFFULL);
    }

    // Get or create PT
    if (!(pd[pd_idx] & PAGE_PRESENT)) {
        pt = (page_entry_t*)alloc_table();
        pd[pd_idx] = ((uint64_t)pt) | upper;
    } else {
        if (pd[pd_idx] & PAGE_SIZE_2MB) kpanic("map_page: address is inside a 2 MiB page");
        pd[pd_idx] |= upper;
        pt = (page_entry_t*)(pd[pd_idx] & ~0xFFFULL);
    }

    // Map the actual page
    pt[pt_idx] = (phys & ~0xFFFULL) | (flags & 0xFFF) | PAGE_PRESENT;
    asm volatile("invlpg (%0)" :: "r"(virt) : "memory");
}

uint64_t paging_get_entry(uint64_t virt) {
    if (!pml4) return 0;
    page_entry_t e = pml4[(virt >> 39) & 0x1FF];
    if (!(e & PAGE_PRESENT)) return 0;
    e = ((page_entry_t*)(e & ~0xFFFULL))[(virt >> 30) & 0x1FF];
    if (!(e & PAGE_PRESENT)) return 0;
    e = ((page_entry_t*)(e & ~0xFFFULL))[(virt >> 21) & 0x1FF];
    if (!(e & PAGE_PRESENT) || (e & PAGE_SIZE_2MB)) return e;
    return ((page_entry_t*)(e & ~0xFFFULL))[(virt >> 12) & 0x1FF];
}

uint64_t unmap_page(uint64_t virt) {
    page_entry_t e = pml4[(virt >> 39) & 0x1FF];
    if (!(e & PAGE_PRESENT)) return 0;
    e = ((page_entry_t*)(e & ~0xFFFULL))[(virt >> 30) & 0x1FF];
    if (!(e & PAGE_PRESENT)) return 0;
    e = ((page_entry_t*)(e & ~0xFFFULL))[(virt >> 21) & 0x1FF];
    if (!(e & PAGE_PRESENT) || (e & PAGE_SIZE_2MB)) return 0;
    page_entry_t *pt = (page_entry_t*)(e & ~0xFFFULL);
    uint64_t pte = pt[(virt >> 12) & 0x1FF];
    if (!(pte & PAGE_PRESENT)) return 0;
    pt[(virt >> 12) & 0x1FF] = 0;
    asm volatile("invlpg (%0)" :: "r"(virt) : "memory");
    return pte & ~0xFFFULL;
}
