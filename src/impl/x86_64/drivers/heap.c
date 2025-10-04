// heap.c
#include "heap.h"
#include "memory.h"
#include <stdint.h>

typedef struct block_header {
    uint64_t size;
    uint8_t is_free;
    struct block_header* next;
} block_header_t;

static uint64_t heap_base;
static uint64_t heap_end;
static uint64_t heap_size;
static block_header_t* free_list;
static uint64_t total_allocated;
static uint64_t allocation_count;

#define HEADER_SIZE sizeof(block_header_t)
#define ALIGN(size) (((size) + 7) & ~7)  // 8-byte alignment

void heap_init(uint64_t start, uint64_t size) {
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
}

void* kmalloc(uint64_t size) {
    if (size == 0) return 0;
    
    size = ALIGN(size);
    
    block_header_t* current = free_list;
    block_header_t* prev = 0;
    
    // First-fit allocation
    while (current) {
        if (current->is_free && current->size >= size) {
            // Found a suitable block
            if (current->size > size + HEADER_SIZE + 64) {
                // Split the block if there's enough space left
                block_header_t* new_block = (block_header_t*)((uint64_t)current + HEADER_SIZE + size);
                new_block->size = current->size - size - HEADER_SIZE;
                new_block->is_free = 1;
                new_block->next = current->next;
                
                current->size = size;
                current->next = new_block;
            }
            
            current->is_free = 0;
            total_allocated += current->size + HEADER_SIZE;
            allocation_count++;
            
            return (void*)((uint64_t)current + HEADER_SIZE);
        }
        
        prev = current;
        current = current->next;
    }
    
    return 0;  // Out of memory
}

void kfree(void* ptr) {
    if (!ptr) return;
    
    block_header_t* block = (block_header_t*)((uint64_t)ptr - HEADER_SIZE);
    
    // Sanity check
    if ((uint64_t)block < heap_base || (uint64_t)block >= heap_end) {
        return;  // Invalid pointer
    }
    
    if (block->is_free) {
        return;  // Double free protection
    }
    
    block->is_free = 1;
    total_allocated -= (block->size + HEADER_SIZE);
    allocation_count--;
    
    // Coalesce with next block if it's free
    if (block->next && block->next->is_free) {
        block->size += HEADER_SIZE + block->next->size;
        block->next = block->next->next;
    }
    
    // Coalesce with previous block
    block_header_t* current = free_list;
    while (current && current->next != block) {
        current = current->next;
    }
    
    if (current && current->is_free && 
        (uint64_t)current + HEADER_SIZE + current->size == (uint64_t)block) {
        current->size += HEADER_SIZE + block->size;
        current->next = block->next;
    }
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