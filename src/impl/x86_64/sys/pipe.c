// pipe.c - pipes: a 16 KiB ring, readers and writers wait on it (sys/pipe.h)
#include "sys/pipe.h"
#include "sys/task.h"
#include "sys/process.h"
#include "drivers/heap.h"
#include "lib/string.h"

#define PIPE_SIZE 16384

struct pipe {
    uint8_t buf[PIPE_SIZE];
    uint32_t head, count;                 /* read position, bytes in it */
    int readers, writers;
};

static volatile uint64_t io_gen;

void io_event(void) {
    io_gen++;
    task_wakeup((const void *)&io_gen);
}

uint64_t io_generation(void) { return io_gen; }

int io_wait(uint64_t gen, uint32_t ms) {
    if (io_gen != gen) return 1;
    task_block((const void *)&io_gen, ms);
    return io_gen != gen;
}

/* Wake whoever waits on the pipe, and every poll(). */
static void wake(pipe_t *p) {
    task_wakeup(p);
    io_event();
}

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
    wake(p);
    if (p->readers <= 0 && p->writers <= 0) kfree(p);
}

void pipe_state(pipe_t *p, uint32_t *waiting, uint32_t *room, int *no_writers, int *no_readers) {
    *waiting = p->count;
    *room = PIPE_SIZE - p->count;
    *no_writers = p->writers <= 0;
    *no_readers = p->readers <= 0;
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
    wake(p);                       /* room for the writer */
    return (int64_t)n;
}

int pipe_getc(pipe_t *p) {
    if (p->count == 0) return p->writers <= 0 ? -2 : -1;
    int c = p->buf[p->head];
    p->head = (p->head + 1) % PIPE_SIZE;
    p->count--;
    wake(p);
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
        wake(p);                   /* data for the reader */
    }
    return (int64_t)done;
}
