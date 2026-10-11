#include "drivers/paging.h"
#include "drivers/physmem.h"
#include "drivers/memory.h"
#include "core/exceptions.h"
#include <stdint.h>

typedef uint64_t page_entry_t;

#define ADDR_MASK 0x000FFFFFFFFFF000ULL
/* The table an entry points to (tables are reached through the direct map). */
#define TABLE(e) ((page_entry_t *)phys_to_virt((e) & ADDR_MASK))

extern char stack_guard[];   // boot stack guard page (main.asm): must stay unmapped

static page_entry_t *pml4;   // the kernel's, through the direct map
static uint64_t pml4_phys;

// The kernel's own page tables come from a fixed pool between the kernel
// image (ends below 0x400000, see linker.ld) and the heap (starts at or
// above 0x600000). Programs' tables come from the heap.
#define PAGE_TABLE_AREA 0x400000
#define PAGE_TABLE_END  0x600000
static uint64_t next_table = PAGE_TABLE_AREA;

/* Extra physical ranges (the framebuffer, boot modules) for the direct map. */
#define MAX_EXTRA_REGIONS 16
static struct { uint64_t base, size; } extra_regions[MAX_EXTRA_REGIONS];
static int extra_count;

void paging_add_direct_region(uint64_t base, uint64_t size) {
    if (extra_count < MAX_EXTRA_REGIONS && size) {
        extra_regions[extra_count].base = base;
        extra_regions[extra_count].size = size;
        extra_count++;
    }
}

/* Device registers are mapped into their own window. Its PDPT exists from
 * the start (paging_init), before any program's address space copies the
 * kernel's PML4 entries, so mappings added later are visible everywhere. */
static uint64_t mmio_next = MMIO_WINDOW;

void *mmio_map(uint64_t phys, uint64_t size) {
    uint64_t first = phys & ~0xFFFULL, last = (phys + size + 0xFFF) & ~0xFFFULL;
    uint64_t va = mmio_next;
    for (uint64_t p = first; p < last; p += PAGE_SIZE)
        map_page(va + (p - first), p, PAGE_PRESENT | PAGE_RW | 0x10 | 0x08);   /* uncached */
    mmio_next += (last - first) + PAGE_SIZE;                                   /* plus a gap */
    return (void *)(va + (phys - first));
}

/* Memory types: PAT entry 1 (pages with only PWT set) becomes write-combining
 * instead of write-through, which nothing used. Writes to the screen then
 * go out in bursts instead of one uncached store at a time. Every core. */
void paging_pat_init(void) {
    uint32_t lo, hi;
    __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(0x277));
    lo = (lo & ~0x0000FF00u) | 0x00000100u;               /* PA1 = WC (1) */
    __asm__ volatile("wbinvd" ::: "memory");
    __asm__ volatile("wrmsr" : : "c"(0x277), "a"(lo), "d"(hi));
    __asm__ volatile("mov %%cr3, %%rax; mov %%rax, %%cr3" ::: "rax", "memory");
}

/* Scattered RAM pages one after another for the CPU, write-combining (a
 * buffer the display engine reads). */
void *paging_map_pages_wc(const uint64_t *phys, uint64_t n) {
    uint64_t va = mmio_next;
    for (uint64_t i = 0; i < n; i++) map_page(va + i * PAGE_SIZE, phys[i], PAGE_PRESENT | PAGE_RW | 0x08);
    mmio_next += (n + 1) * PAGE_SIZE;
    return (void *)va;
}

void *mmio_map_wc(uint64_t phys, uint64_t size) {
    uint64_t first = phys & ~0xFFFULL, last = (phys + size + 0xFFF) & ~0xFFFULL;
    uint64_t va = mmio_next;
    for (uint64_t p = first; p < last; p += PAGE_SIZE)
        map_page(va + (p - first), p, PAGE_PRESENT | PAGE_RW | 0x08);          /* PAT 1: WC */
    mmio_next += (last - first) + PAGE_SIZE;
    return (void *)(va + (phys - first));
}

static page_entry_t *alloc_table(void) {
    if (next_table >= PAGE_TABLE_END) kpanic("page table pool exhausted");
    page_entry_t *t = phys_to_virt(next_table);
    next_table += 0x1000;
    for (int i = 0; i < 512; i++) t[i] = 0;
    return t;
}

/* The next table down from entry `e`, created (kernel table) if missing. */
static page_entry_t *kernel_next(page_entry_t *e, uint64_t upper) {
    if (!(*e & PAGE_PRESENT)) *e = virt_to_phys(alloc_table()) | upper;
    else *e |= upper;
    return TABLE(*e);
}

// Page directory for `virt`, creating the PDPT and PD if needed.
static page_entry_t *get_pd(uint64_t virt) {
    page_entry_t *pdpt = kernel_next(&pml4[(virt >> 39) & 0x1FF], PAGE_PRESENT | PAGE_RW);
    return kernel_next(&pdpt[(virt >> 30) & 0x1FF], PAGE_PRESENT | PAGE_RW);
}

void map_large_page(uint64_t virt, uint64_t phys) {
    page_entry_t *pd = get_pd(virt);
    pd[(virt >> 21) & 0x1FF] = (phys & ~(LARGE_PAGE - 1)) | PAGE_PRESENT | PAGE_RW | PAGE_SIZE_2MB;
    asm volatile("invlpg (%0)" :: "r"(virt) : "memory");
}

void map_page(uint64_t virt, uint64_t phys, uint64_t flags) {
    // A user page needs the user bit at every level; kernel pages under the
    // same upper entries stay protected because their own entries lack it.
    uint64_t upper = PAGE_PRESENT | PAGE_RW | (flags & PAGE_USER);
    page_entry_t *pdpt = kernel_next(&pml4[(virt >> 39) & 0x1FF], upper);
    page_entry_t *pd = kernel_next(&pdpt[(virt >> 30) & 0x1FF], upper);
    page_entry_t *pde = &pd[(virt >> 21) & 0x1FF];
    if ((*pde & PAGE_PRESENT) && (*pde & PAGE_SIZE_2MB)) kpanic("map_page: address is inside a 2 MiB page");
    page_entry_t *pt = kernel_next(pde, upper);
    pt[(virt >> 12) & 0x1FF] = (phys & ~0xFFFULL) | (flags & 0xFFF) | PAGE_PRESENT;
    asm volatile("invlpg (%0)" :: "r"(virt) : "memory");
}

/* Put physical [start, end) into the direct map (write-back): 2 MiB pages
 * where a whole aligned 2 MiB lies inside, 4 KiB pages at the edges. Parts
 * mapped already are left alone. */
static void direct_map_range(uint64_t start, uint64_t end) {
    start &= ~0xFFFULL;
    end = (end + 0xFFF) & ~0xFFFULL;
    for (uint64_t a = start; a < end; ) {
        uint64_t v = DIRECT_MAP + a;
        page_entry_t *pde = &get_pd(v)[(v >> 21) & 0x1FF];
        if ((*pde & PAGE_PRESENT) && (*pde & PAGE_SIZE_2MB)) {        /* covered */
            a = (a & ~(LARGE_PAGE - 1)) + LARGE_PAGE;
            continue;
        }
        if (!(*pde & PAGE_PRESENT) && !(a & (LARGE_PAGE - 1)) && end - a >= LARGE_PAGE) {
            *pde = a | PAGE_PRESENT | PAGE_RW | PAGE_SIZE_2MB;
            a += LARGE_PAGE;
            continue;
        }
        if (!(paging_get_entry(v) & PAGE_PRESENT)) map_page(v, a, PAGE_PRESENT | PAGE_RW);
        a += PAGE_SIZE;
    }
}

void paging_init(uint64_t image_start, uint64_t image_end,
                 uint64_t heap_start, uint64_t heap_size) {
    // Everything is mapped BEFORE switching to these tables; until then the
    // boot tables (main.asm) map the first 4 GiB in the direct map, which is
    // where the pool and the heap are.
    pml4 = alloc_table();
    pml4_phys = virt_to_phys(pml4);

    // The kernel image, at KERNEL_VMA
    for (uint64_t addr = image_start; addr < image_end; addr += PAGE_SIZE) {
        if (KERNEL_VMA + addr == (uint64_t)stack_guard) continue;   // guard page stays unmapped
        map_page(KERNEL_VMA + addr, addr, PAGE_PRESENT | PAGE_RW);
    }

    // The direct map: the first MiB (VGA text, the SMP trampoline), the
    // kernel image and the whole page-table pool (tables created later must
    // be reachable too), the heap, all RAM and ACPI memory, and the
    // registered ranges (framebuffer, boot modules).
    direct_map_range(0, 0x100000);
    direct_map_range(image_start, PAGE_TABLE_END);
    direct_map_range(heap_start, heap_start + heap_size);
    static mb2_region_t ram[64];
    int n = memory_get_ram_like(ram, 64);
    for (int i = 0; i < n; i++) direct_map_range(ram[i].base, ram[i].base + ram[i].length);
    for (int i = 0; i < extra_count; i++)
        direct_map_range(extra_regions[i].base, extra_regions[i].base + extra_regions[i].size);

    // The device and ACPI windows: their PDPTs exist from the start (see mmio_map).
    kernel_next(&pml4[(MMIO_WINDOW >> 39) & 0x1FF], PAGE_PRESENT | PAGE_RW);
    kernel_next(&pml4[(ACPI_WINDOW >> 39) & 0x1FF], PAGE_PRESENT | PAGE_RW);

    asm volatile("cli");
    asm volatile("mov %0, %%cr3" :: "r"(pml4_phys) : "memory");
    asm volatile("sti");
}

uint64_t paging_get_entry(uint64_t virt) {
    if (!pml4) return 0;
    page_entry_t e = pml4[(virt >> 39) & 0x1FF];
    if (!(e & PAGE_PRESENT)) return 0;
    e = TABLE(e)[(virt >> 30) & 0x1FF];
    if (!(e & PAGE_PRESENT)) return 0;
    e = TABLE(e)[(virt >> 21) & 0x1FF];
    if (!(e & PAGE_PRESENT) || (e & PAGE_SIZE_2MB)) return e;
    return TABLE(e)[(virt >> 12) & 0x1FF];
}

uint64_t unmap_page(uint64_t virt) {
    page_entry_t e = pml4[(virt >> 39) & 0x1FF];
    if (!(e & PAGE_PRESENT)) return 0;
    e = TABLE(e)[(virt >> 30) & 0x1FF];
    if (!(e & PAGE_PRESENT)) return 0;
    e = TABLE(e)[(virt >> 21) & 0x1FF];
    if (!(e & PAGE_PRESENT) || (e & PAGE_SIZE_2MB)) return 0;
    page_entry_t *pt = TABLE(e);
    uint64_t pte = pt[(virt >> 12) & 0x1FF];
    if (!(pte & PAGE_PRESENT)) return 0;
    pt[(virt >> 12) & 0x1FF] = 0;
    asm volatile("invlpg (%0)" :: "r"(virt) : "memory");
    return pte & ADDR_MASK;
}

/* ---- Address spaces for user programs ------------------------------------
 *
 * Every program gets its own PML4. Entries 256-511 (the upper half) are the
 * kernel's, copied: the tables under them are shared, so kernel mappings are
 * identical everywhere. Entries 0-255 (the lower half, 128 TiB) are the
 * program's own. Its tables come from the heap and are freed with it. */

#define USER_ENTRIES 256

static page_entry_t *new_table(void) {
    page_entry_t *t = alloc_frame();
    if (t) for (int i = 0; i < 512; i++) t[i] = 0;
    return t;
}

uint64_t paging_kernel_root(void) {
    return pml4_phys;
}

uint64_t paging_create_address_space(void) {
    page_entry_t *root = new_table();
    if (!root) return 0;
    for (int i = USER_ENTRIES; i < 512; i++) root[i] = pml4[i];
    return virt_to_phys(root);
}

/* The next table down from user entry `e`, created if `create`; 0 if none. */
static page_entry_t *user_next(page_entry_t *e, int create) {
    if (!(*e & PAGE_PRESENT)) {
        if (!create) return 0;
        page_entry_t *t = new_table();
        if (!t) return 0;
        *e = virt_to_phys(t) | PAGE_PRESENT | PAGE_RW | PAGE_USER;
    }
    return TABLE(*e);
}

/* The page-table entry for `virt` in `root`'s lower half; 0 if not there. */
uint64_t *paging_user_pte(uint64_t root, uint64_t virt, int create) {
    if (!root || virt >= USER_SPACE_END) return 0;
    page_entry_t *t = TABLE(root);
    for (int shift = 39; shift > 12; shift -= 9) {
        t = user_next(&t[(virt >> shift) & 0x1FF], create);
        if (!t) return 0;
    }
    return &t[(virt >> 12) & 0x1FF];
}

int paging_map_user(uint64_t root, uint64_t virt, uint64_t phys) {
    return paging_map_user_prot(root, virt, phys, 1);
}

int paging_map_user_prot(uint64_t root, uint64_t virt, uint64_t phys, int writable) {
    page_entry_t *pte = paging_user_pte(root, virt, 1);
    if (!pte) return -1;
    *pte = (phys & ADDR_MASK) | PAGE_PRESENT | PAGE_USER | (writable ? PAGE_RW : 0);
    asm volatile("invlpg (%0)" :: "r"(virt) : "memory");
    return 0;
}

uint64_t paging_get_user_entry(uint64_t root, uint64_t virt) {
    page_entry_t *pte = paging_user_pte(root, virt, 0);
    return pte ? *pte : 0;
}

/* Call fn for every present page of `root`'s lower half. */
static void walk_level(page_entry_t *t, int level, uint64_t base,
                       void (*fn)(uint64_t virt, uint64_t *pte, void *ctx), void *ctx) {
    for (int i = 0; i < (level == 4 ? USER_ENTRIES : 512); i++) {
        if (!(t[i] & PAGE_PRESENT)) continue;
        uint64_t v = base | ((uint64_t)i << (12 + 9 * (level - 1)));
        if (level == 1) fn(v, &t[i], ctx);
        else walk_level(TABLE(t[i]), level - 1, v, fn, ctx);
    }
}

void paging_walk_user(uint64_t root, void (*fn)(uint64_t virt, uint64_t *pte, void *ctx), void *ctx) {
    if (root) walk_level(TABLE(root), 4, 0, fn, ctx);
}

void paging_flush(void) {
    uint64_t cr3;
    asm volatile("mov %%cr3, %0; mov %0, %%cr3" : "=r"(cr3) :: "memory");
}

/* Free the table under entry `e` (`level` 3 = PDPT ... 1 = PT) and all below it. */
static void free_user_table(page_entry_t e, int level) {
    page_entry_t *t = TABLE(e);
    for (int i = 0; i < 512; i++) {
        if (!(t[i] & PAGE_PRESENT)) continue;
        if (level > 1) {
            free_user_table(t[i], level - 1);
        } else {
            uint64_t phys = t[i] & ADDR_MASK;
            if (page_owned(phys)) page_free(phys);          /* all of RAM (physmem.c) */
            else free_frame(phys_to_virt(phys));            /* the heap */
        }
    }
    free_frame(t);
}

void paging_free_address_space(uint64_t root) {
    if (!root) return;
    page_entry_t *r = TABLE(root);
    for (int i = 0; i < USER_ENTRIES; i++)
        if (r[i] & PAGE_PRESENT) free_user_table(r[i], 3);
    free_frame(r);
}

void paging_switch(uint64_t root) {
    uint64_t current;
    asm volatile("mov %%cr3, %0" : "=r"(current));
    if (current != root) asm volatile("mov %0, %%cr3" :: "r"(root) : "memory");
}

/* ---- An address space for the firmware ----------------------------------------- */

/* Tables made for it (the rest are the kernel's, shared, and are copied
 * before anything is added to them). */
static page_entry_t *own_tables[128];
static int own_count;

static int owned(page_entry_t *t) {
    for (int i = 0; i < own_count; i++)
        if (own_tables[i] == t) return 1;
    return 0;
}

/* The table that entry `e` points to, ours to change (copied if shared). */
static page_entry_t *own_table(page_entry_t *e) {
    page_entry_t *t = TABLE(*e);
    if ((*e & PAGE_PRESENT) && owned(t)) return t;
    if (own_count == 128) return 0;
    page_entry_t *copy = new_table();
    if (!copy) return 0;
    if (*e & PAGE_PRESENT)
        for (int i = 0; i < 512; i++) copy[i] = t[i];
    own_tables[own_count++] = copy;
    *e = virt_to_phys(copy) | PAGE_PRESENT | PAGE_RW;
    return copy;
}

uint64_t paging_create_identity_space(const paging_range_t *ranges, int count) {
    own_count = 0;
    page_entry_t root_entry = 0;
    page_entry_t *root = own_table(&root_entry);          /* a copy of nothing: all zero */
    if (!root) return 0;
    for (int i = 0; i < 512; i++) root[i] = pml4[i];
    page_entry_t *pdpt = own_table(&root[0]);             /* the first 512 GiB */
    if (!pdpt) return 0;
    const uint64_t cache_off = 0x18;                      /* PCD | PWT */
    for (int r = 0; r < count; r++) {
        uint64_t start = ranges[r].start & ~0xFFFULL;
        uint64_t end = (ranges[r].start + ranges[r].size + 0xFFF) & ~0xFFFULL;
        uint64_t flags = PAGE_PRESENT | PAGE_RW | (ranges[r].uncached ? cache_off : 0);
        for (uint64_t a = start; a < end && a < (512ULL << 30); ) {
            page_entry_t *pdpte = &pdpt[(a >> 30) & 0x1FF];
            if ((*pdpte & PAGE_PRESENT) && (*pdpte & PAGE_SIZE_2MB)) {     /* a 1 GiB page: there already */
                a = ((a >> 30) + 1) << 30;
                continue;
            }
            page_entry_t *pd = own_table(pdpte);
            if (!pd) return 0;
            page_entry_t *pde = &pd[(a >> 21) & 0x1FF];
            uint64_t big = a & ~(LARGE_PAGE - 1);
            if (!(*pde & PAGE_PRESENT)) {                 /* nothing here: one 2 MiB page */
                *pde = big | flags | PAGE_SIZE_2MB;
                a = big + LARGE_PAGE;
                continue;
            }
            if (*pde & PAGE_SIZE_2MB) {                   /* mapped already */
                a = big + LARGE_PAGE;
                continue;
            }
            page_entry_t *pt = own_table(pde);            /* 4 KiB pages: fill in the gaps */
            if (!pt) return 0;
            for (; a < end && a < big + LARGE_PAGE; a += PAGE_SIZE) {
                page_entry_t *pte = &pt[(a >> 12) & 0x1FF];
                if (!(*pte & PAGE_PRESENT)) *pte = a | flags;
            }
        }
    }
    return virt_to_phys(root);
}
