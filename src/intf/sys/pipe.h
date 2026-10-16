// pipe.h - a byte stream from one program to another (`a | b`)
#ifndef PIPE_H
#define PIPE_H

#include <stdint.h>

typedef struct pipe pipe_t;

pipe_t *pipe_new(void);                   /* one reader and one writer end, 0 if no memory */
void pipe_end_added(pipe_t *p, int writer);
void pipe_end_closed(pipe_t *p, int writer);   /* the last end frees it */

/* Read up to `len` bytes, waiting for some. 0: every writer closed it (end of
 * file); -1: interrupted (Ctrl+C, a signal). */
int64_t pipe_read(pipe_t *p, void *buf, uint64_t len);
/* The next byte without waiting: -1 none yet, -2 every writer closed it. */
int pipe_getc(pipe_t *p);
/* Write all of it, waiting for room. -2: no reader any more; -1: interrupted. */
int64_t pipe_write(pipe_t *p, const void *buf, uint64_t len);

#endif
