// physmem.c - page frames from all of RAM (see drivers/physmem.h)
//
// RAM comes in chunks: the 2 MiB-aligned parts of the usable regions that
// the heap, the kernel and the boot modules do not use. Each chunk has a
// bitmap of its pages (from the heap) and is mapped with 2 MiB pages in the
// direct map.
#include "drivers/physmem.h"
#include "drivers/memory.h"
#include "drivers/paging.h"
#include "drivers/heap.h"
#include "core/multiboot2.h"
#include "lib/string.h"

#define CHUNK_ALIGN  LARGE_PAGE
#define MAX_CHUNKS   64
#define LOW_RESERVED (16ULL * 1024 * 1024)           /* kernel, page tables, SMP trampoline, ... */

typedef struct {
    uint64_t base, pages, free;
    uint64_t *bitmap;                                 /* 1 = in use */
    uint64_t hint;                                    /* where to look next (a word index) */
} chunk_t;

static chunk_t chunks[MAX_CHUNKS];
static int chunk_count;
static uint64_t total_pages, used_pages;

static void add_chunk(uint64_t start, uint64_t end) {
    start = (start + CHUNK_ALIGN - 1) & ~(CHUNK_ALIGN - 1);
    end &= ~(CHUNK_ALIGN - 1);
    if (end <= start || chunk_count == MAX_CHUNKS) return;
    if (end > (512ULL << 30)) end = 512ULL << 30;     /* the direct map's PML4 entry: 512 GiB */
    if (end <= start) return;
    chunk_t *c = &chunks[chunk_count];
    c->pages = (end - start) / PAGE_SIZE;
    c->bitmap = kmalloc((c->pages + 63) / 64 * 8);
    if (!c->bitmap) return;
    memset(c->bitmap, 0, (c->pages + 63) / 64 * 8);
    c->base = start;
    c->free = c->pages;
    c->hint = 0;
    for (uint64_t a = start; a < end; a += CHUNK_ALIGN) map_large_page(DIRECT_MAP_BASE + a, a);
    total_pages += c->pages;
    chunk_count++;
}

void physmem_init(uint64_t heap_start, uint64_t heap_end) {
    static mb2_region_t regions[32];
    int n = memory_get_regions(regions, 32);
    for (int i = 0; i < n; i++) {
        uint64_t s = regions[i].base, e = s + regions[i].length;
        if (s < LOW_RESERVED) s = LOW_RESERVED;
        if (e <= s) continue;
        /* Leave out the heap. */
        if (heap_start < e && heap_end > s) {
            if (heap_start > s) add_chunk(s, heap_start);
            if (heap_end < e) add_chunk(heap_end, e);
        } else {
            add_chunk(s, e);
        }
    }
}

static chunk_t *chunk_of(uint64_t phys) {
    for (int i = 0; i < chunk_count; i++)
        if (phys >= chunks[i].base && phys < chunks[i].base + chunks[i].pages * PAGE_SIZE) return &chunks[i];
    return 0;
}

int page_owned(uint64_t phys) {
    return chunk_of(phys) != 0;
}

uint64_t page_alloc(void) {
    for (int i = 0; i < chunk_count; i++) {
        chunk_t *c = &chunks[i];
        if (!c->free) continue;
        uint64_t words = (c->pages + 63) / 64;
        for (uint64_t k = 0; k < words; k++) {
            uint64_t w = (c->hint + k) % words;
            if (c->bitmap[w] == ~0ULL) continue;
            int bit = __builtin_ctzll(~c->bitmap[w]);
            uint64_t page = w * 64 + (uint64_t)bit;
            if (page >= c->pages) continue;
            c->bitmap[w] |= 1ULL << bit;
            c->free--;
            c->hint = w;
            used_pages++;
            return c->base + page * PAGE_SIZE;
        }
    }
    return 0;
}

void page_free(uint64_t phys) {
    chunk_t *c = chunk_of(phys);
    if (!c) return;
    uint64_t page = (phys - c->base) / PAGE_SIZE;
    uint64_t bit = 1ULL << (page % 64);
    if (!(c->bitmap[page / 64] & bit)) return;          /* not in use: a double free */
    c->bitmap[page / 64] &= ~bit;
    c->free++;
    used_pages--;
}

uint64_t physmem_total(void) { return total_pages * PAGE_SIZE; }
uint64_t physmem_used(void) { return used_pages * PAGE_SIZE; }
