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

static void *raw_malloc(size_t size) {
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

static void raw_free(void *ptr) {
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

/* Memory at a multiple of `align` (a power of two): a block with room to
 * spare, whose front part becomes a free block of its own. free() takes it. */
static void *raw_memalign(size_t align, size_t size) {
    if (align <= 16) return raw_malloc(size);
    size = ALIGN16(size ? size : 1);
    char *p = raw_malloc(size + align + HEADER + 32);
    if (!p || ((size_t)p & (align - 1)) == 0) return p;
    block_t *b = (block_t *)(p - HEADER);
    char *a = (char *)(((size_t)p + HEADER + 32 + align - 1) & ~(align - 1));
    block_t *n = (block_t *)(a - HEADER);
    n->size = (size_t)((char *)b + HEADER + b->size - a);
    n->free = 0;
    n->next = b->next;
    b->next = n;
    b->size = (size_t)((char *)n - p);
    if (heap_tail == b) heap_tail = n;
    raw_free(p);                                  /* the front part: free (merges with what follows only) */
    return a;
}

/* The allocator is shared by a program's threads: one at a time. */
static mutex_t heap_lock;

void *malloc(size_t size) {
    mutex_lock(&heap_lock);
    void *p = raw_malloc(size);
    mutex_unlock(&heap_lock);
    return p;
}

void free(void *ptr) {
    if (!ptr) return;
    mutex_lock(&heap_lock);
    raw_free(ptr);
    mutex_unlock(&heap_lock);
}

void *memalign(size_t align, size_t size) {
    mutex_lock(&heap_lock);
    void *p = raw_memalign(align, size);
    mutex_unlock(&heap_lock);
    return p;
}

int posix_memalign(void **out, size_t align, size_t size) {
    void *p = memalign(align, size);
    if (!p) return 12;                        /* ENOMEM */
    *out = p;
    return 0;
}

void *aligned_alloc(size_t align, size_t size) { return memalign(align, size); }

static void *raw_calloc(size_t count, size_t size) {
    size_t total = count * size;
    if (size && total / size != count) return 0;
    void *p = raw_malloc(total);
    if (p) memset(p, 0, total);
    return p;
}

static void *raw_realloc(void *ptr, size_t size) {
    if (!ptr) return raw_malloc(size);
    block_t *b = (block_t *)((char *)ptr - HEADER);
    if (b->size >= size) return ptr;
    void *p = raw_malloc(size);
    if (p) {
        memcpy(p, ptr, b->size);
        raw_free(ptr);
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

void *calloc(size_t count, size_t size) {
    mutex_lock(&heap_lock);
    void *p = raw_calloc(count, size);
    mutex_unlock(&heap_lock);
    return p;
}

void *realloc(void *ptr, size_t size) {
    mutex_lock(&heap_lock);
    void *p = raw_realloc(ptr, size);
    mutex_unlock(&heap_lock);
    return p;
}
