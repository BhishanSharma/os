// heap.c - the kernel heap
//
// Small requests (up to 2 KiB, most of them: ACPI's objects, network
// buffers, file nodes) come from per-size free lists: allocation and free
// are a list push or pop. Pages for them are carved from the big heap in
// 64 KiB chunks and stay with their size. Bigger requests use a first-fit
// list of blocks. (First fit for everything walked every block ever made on
// each call: after the drivers started, ACPI's start-up took 8 s on a laptop.)
#include "drivers/heap.h"
#include "drivers/memory.h"
#include <stdint.h>

/* Every block's header ends in a tag word just before the pointer handed out,
 * so kfree can tell a small block from a big one. */
#define TAG_BIG    0xB16B10C0B16B10C0ULL
#define TAG_SMALL  0x5A11B10C00000000ULL          /* | size class */

typedef struct block_header {
    uint64_t size;
    struct block_header* next;
    uint64_t is_free;
    uint64_t tag;
} block_header_t;                                 /* 32 bytes: pointers stay 16-byte aligned */

typedef struct small_block {
    struct small_block* next;                     /* while free */
    uint64_t tag;
} small_header_t;

#define CLASSES     8                             /* 16, 32, ... 2048 bytes */
#define SMALL_MAX   2048
#define CHUNK       (64 * 1024)

static uint64_t heap_base;
static uint64_t heap_end;
static uint64_t heap_size;
static block_header_t* free_list;                 /* every big block, in address order */
static small_header_t* small_free[CLASSES];
static uint64_t total_allocated;
static uint64_t allocation_count;

#define HEADER_SIZE sizeof(block_header_t)
#define ALIGN(size) (((size) + 15) & ~15ULL)

void heap_init(uint64_t start, uint64_t size) {
    start = (start + 15) & ~15ULL;
    heap_base = start;
    heap_end = start + size;
    heap_size = size;
    total_allocated = 0;
    allocation_count = 0;

    // Initialize with one large free block
    free_list = (block_header_t*)start;
    free_list->size = size - HEADER_SIZE;
    free_list->is_free = 1;
    free_list->next = 0;
    free_list->tag = TAG_BIG;
}

static void* big_alloc(uint64_t size) {
    size = ALIGN(size);
    for (block_header_t* current = free_list; current; current = current->next) {
        if (!current->is_free || current->size < size) continue;
        if (current->size > size + HEADER_SIZE + 64) {
            // Split the block if there's enough space left
            block_header_t* new_block = (block_header_t*)((uint64_t)current + HEADER_SIZE + size);
            new_block->size = current->size - size - HEADER_SIZE;
            new_block->is_free = 1;
            new_block->next = current->next;
            new_block->tag = TAG_BIG;
            current->size = size;
            current->next = new_block;
        }
        current->is_free = 0;
        return (void*)((uint64_t)current + HEADER_SIZE);
    }
    return 0;  // Out of memory
}

static void big_free(block_header_t* block) {
    if (block->is_free) return;  // Double free protection
    block->is_free = 1;

    // Coalesce with next block if it's free
    if (block->next && block->next->is_free) {
        block->size += HEADER_SIZE + block->next->size;
        block->next = block->next->next;
    }

    // Coalesce with previous block
    block_header_t* current = free_list;
    while (current && current->next != block) current = current->next;
    if (current && current->is_free &&
        (uint64_t)current + HEADER_SIZE + current->size == (uint64_t)block) {
        current->size += HEADER_SIZE + block->size;
        current->next = block->next;
    }
}

static int size_class(uint64_t size) {
    int c = 0;
    while ((16ULL << c) < size) c++;
    return c;
}

void* kmalloc(uint64_t size) {
    if (size == 0) return 0;
    if (size > SMALL_MAX) {
        void* p = big_alloc(size);
        if (p) {
            block_header_t* b = (block_header_t*)((uint64_t)p - HEADER_SIZE);
            total_allocated += b->size + HEADER_SIZE;
            allocation_count++;
        }
        return p;
    }
    int c = size_class(size);
    uint64_t stride = sizeof(small_header_t) + (16ULL << c);
    if (!small_free[c]) {                         /* a new chunk, cut into blocks of this size */
        uint8_t* chunk = big_alloc(CHUNK);
        if (!chunk) return 0;
        for (uint64_t off = 0; off + stride <= CHUNK; off += stride) {
            small_header_t* s = (small_header_t*)(chunk + off);
            s->tag = TAG_SMALL | (uint64_t)c;
            s->next = small_free[c];
            small_free[c] = s;
        }
    }
    small_header_t* s = small_free[c];
    small_free[c] = s->next;
    s->next = 0;
    total_allocated += stride;
    allocation_count++;
    return (void*)((uint64_t)s + sizeof(small_header_t));
}

void kfree(void* ptr) {
    if (!ptr) return;
    if ((uint64_t)ptr < heap_base + 16 || (uint64_t)ptr >= heap_end) return;  // Invalid pointer
    uint64_t tag = *(uint64_t*)((uint64_t)ptr - 8);
    if ((tag & ~0xFFULL) == TAG_SMALL) {
        int c = (int)(tag & 0xFF);
        if (c >= CLASSES) return;
        small_header_t* s = (small_header_t*)((uint64_t)ptr - sizeof(small_header_t));
        if (s->next || s == small_free[c]) return;   // on the free list already: freed twice
        s->next = small_free[c];
        small_free[c] = s;
        total_allocated -= sizeof(small_header_t) + (16ULL << c);
        allocation_count--;
        return;
    }
    if (tag != TAG_BIG) return;                   // not ours
    block_header_t* block = (block_header_t*)((uint64_t)ptr - HEADER_SIZE);
    if (block->is_free) return;
    total_allocated -= block->size + HEADER_SIZE;
    allocation_count--;
    big_free(block);
}

uint64_t heap_get_used() {
    return total_allocated;
}

uint64_t heap_get_free() {
    return heap_size - total_allocated;
}

uint64_t heap_get_total() {
    return heap_size;
}

uint64_t heap_get_allocations() {
    return allocation_count;
}
