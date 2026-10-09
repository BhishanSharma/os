// stdlib.c - malloc on top of sbrk, number parsing, rand
#include <stdlib.h>
#include <string.h>
#include <os.h>

/* First-fit free list. Every block starts with a header; free blocks are
 * merged with a free neighbour that follows them. */
typedef struct block {
    size_t size;            // bytes after the header
    int free;
    struct block *next;
} block_t;

#define ALIGN16(x)  (((x) + 15) & ~(size_t)15)
#define HEADER      ALIGN16(sizeof(block_t))
#define GROW_MIN    (64 * 1024)

static block_t *heap_head, *heap_tail;

static void split(block_t *b, size_t size) {
    if (b->size >= size + HEADER + 32) {
        block_t *rest = (block_t *)((char *)b + HEADER + size);
        rest->size = b->size - size - HEADER;
        rest->free = 1;
        rest->next = b->next;
        b->next = rest;
        b->size = size;
        if (heap_tail == b) heap_tail = rest;
    }
}

void *malloc(size_t size) {
    if (size == 0) size = 1;
    size = ALIGN16(size);
    for (block_t *b = heap_head; b; b = b->next) {
        if (b->free && b->size >= size) {
            split(b, size);
            b->free = 0;
            return (char *)b + HEADER;
        }
    }
    size_t grow = size + HEADER < GROW_MIN ? GROW_MIN : size + HEADER;
    block_t *b = sbrk((long)grow);
    if (b == (void *)-1) return 0;
    b->size = grow - HEADER;
    b->free = 0;
    b->next = 0;
    if (heap_tail) heap_tail->next = b; else heap_head = b;
    heap_tail = b;
    split(b, size);
    return (char *)b + HEADER;
}

void free(void *ptr) {
    if (!ptr) return;
    block_t *b = (block_t *)((char *)ptr - HEADER);
    b->free = 1;
    // Merge with following free blocks that are adjacent in memory.
    while (b->next && b->next->free && (char *)b + HEADER + b->size == (char *)b->next) {
        if (heap_tail == b->next) heap_tail = b;
        b->size += HEADER + b->next->size;
        b->next = b->next->next;
    }
}

void *calloc(size_t count, size_t size) {
    size_t total = count * size;
    if (size && total / size != count) return 0;
    void *p = malloc(total);
    if (p) memset(p, 0, total);
    return p;
}

void *realloc(void *ptr, size_t size) {
    if (!ptr) return malloc(size);
    block_t *b = (block_t *)((char *)ptr - HEADER);
    if (b->size >= size) return ptr;
    void *p = malloc(size);
    if (p) {
        memcpy(p, ptr, b->size);
        free(ptr);
    }
    return p;
}

long atol(const char *s) {
    while (*s == ' ' || *s == '\t') s++;
    int neg = *s == '-';
    if (*s == '-' || *s == '+') s++;
    long v = 0;
    while (*s >= '0' && *s <= '9') v = v * 10 + (*s++ - '0');
    return neg ? -v : v;
}

int atoi(const char *s) { return (int)atol(s); }
int abs(int x) { return x < 0 ? -x : x; }

static unsigned long rand_state = 1;

void srand(unsigned int seed) { rand_state = seed; }

int rand(void) {
    rand_state = rand_state * 6364136223846793005UL + 1442695040888963407UL;
    return (int)((rand_state >> 33) & RAND_MAX);
}
