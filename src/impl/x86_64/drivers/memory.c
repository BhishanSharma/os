// src/impl/x86_64/drivers/memory.c
#include "memory.h"

#define MAX_PHYS_PAGES 65536 // Example: 256 MB RAM (adjust later)

static uint64_t memory_bitmap[MAX_PHYS_PAGES / 64]; // 64 pages per uint64_t
static uint64_t total_pages = 0;

void* memset(void* ptr, int value, uint64_t num) {
    uint8_t* p = (uint8_t*)ptr;
    for (uint64_t i = 0; i < num; i++) {
        p[i] = (uint8_t)value;
    }
    return ptr;
}

void memory_init(uint64_t mem_upper) {
    // mem_upper = memory in KB reported by BIOS
    total_pages = (mem_upper * 1024) / PAGE_SIZE;

    // Clear bitmap (all free)
    memset((void*)memory_bitmap, 0, sizeof(memory_bitmap));

    // Mark first few pages as used (kernel + bitmap)
    // Suppose kernel is loaded at 1 MB, bitmap occupies 64 KB
    uint64_t used_pages = (1024 + sizeof(memory_bitmap)) / PAGE_SIZE;
    for (uint64_t i = 0; i < used_pages; i++) {
        memory_bitmap[i / 64] |= (1ULL << (i % 64));
    }
}

void* alloc_frame() {
    for (uint64_t i = 0; i < total_pages; i++) {
        uint64_t idx = i / 64;
        uint64_t bit = i % 64;
        if ((memory_bitmap[idx] & (1ULL << bit)) == 0) {
            memory_bitmap[idx] |= (1ULL << bit);
            return (void*)(i * PAGE_SIZE);
        }
    }
    return 0;
}

void free_frame(void* frame) {
    uint64_t addr = (uint64_t)frame;
    uint64_t page = addr / PAGE_SIZE;
    uint64_t idx = page / 64;
    uint64_t bit = page % 64;
    memory_bitmap[idx] &= ~(1ULL << bit);
}

uint64_t get_total_memory() {
    return total_pages * PAGE_SIZE;
}
