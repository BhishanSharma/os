// pipe.c - pipes: a 4 KiB ring, readers and writers wait on it (sys/pipe.h)
#include "sys/pipe.h"
#include "sys/task.h"
#include "sys/process.h"
#include "drivers/heap.h"
#include "lib/string.h"

#define PIPE_SIZE 4096

struct pipe {
    uint8_t buf[PIPE_SIZE];
    uint32_t head, count;                 /* read position, bytes in it */
    int readers, writers;
};

pipe_t *pipe_new(void) {
    pipe_t *p = kmalloc(sizeof(*p));
    if (!p) return 0;
    memset(p, 0, sizeof(*p));
    p->readers = p->writers = 1;
    return p;
}

void pipe_end_added(pipe_t *p, int writer) {
    if (writer) p->writers++;
    else p->readers++;
}

void pipe_end_closed(pipe_t *p, int writer) {
    if (writer) p->writers--;
    else p->readers--;
    task_wakeup(p);
    if (p->readers <= 0 && p->writers <= 0) kfree(p);
}

int64_t pipe_read(pipe_t *p, void *buf, uint64_t len) {
    while (p->count == 0) {
        if (p->writers <= 0) return 0;
        if (process_interrupted() || process_signal_pending()) return -1;
        task_block(p, 100);
    }
    uint64_t n = 0;
    uint8_t *out = buf;
    while (n < len && p->count) {
        out[n++] = p->buf[p->head];
        p->head = (p->head + 1) % PIPE_SIZE;
        p->count--;
    }
    task_wakeup(p);                       /* room for the writer */
    return (int64_t)n;
}

int pipe_getc(pipe_t *p) {
    if (p->count == 0) return p->writers <= 0 ? -2 : -1;
    int c = p->buf[p->head];
    p->head = (p->head + 1) % PIPE_SIZE;
    p->count--;
    task_wakeup(p);
    return c;
}

int64_t pipe_write(pipe_t *p, const void *buf, uint64_t len) {
    const uint8_t *in = buf;
    uint64_t done = 0;
    while (done < len) {
        if (p->readers <= 0) return -2;
        if (p->count == PIPE_SIZE) {
            if (process_interrupted() || process_signal_pending()) return done ? (int64_t)done : -1;
            task_block(p, 100);
            continue;
        }
        while (done < len && p->count < PIPE_SIZE) {
            p->buf[(p->head + p->count) % PIPE_SIZE] = in[done++];
            p->count++;
        }
        task_wakeup(p);                   /* data for the reader */
    }
    return (int64_t)done;
}
