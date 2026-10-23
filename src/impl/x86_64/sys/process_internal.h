// process_internal.h - what the system call files share with sys/process.c
// (not for the rest of the kernel)
#ifndef PROCESS_INTERNAL_H
#define PROCESS_INTERNAL_H

#include <stdint.h>
#include <stddef.h>

#define NO_LIMIT_MS 0x7FFFFFFu      /* a wait with "no limit": about 37 hours */

/* 1 if the running program may read (or, with `write`, write) [ptr, ptr + len);
 * missing pages of its regions are brought in. */
int user_range_ok(uint64_t ptr, uint64_t len, int write);
/* Copy a NUL-terminated string from the program; 0 on success. */
int copy_user_string(char *out, uint64_t ptr, size_t max);
/* End the running program here if it was killed (Ctrl+C, kill, a fault). */
void check_killed(void);

/* syscall_net.c: SYS_RESOLVE ... SYS_SOCKCLOSE. */
int64_t sys_net(uint64_t nr, uint64_t a, uint64_t b, uint64_t c, uint64_t d);

#endif
