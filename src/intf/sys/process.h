// process.h - running user programs (ring 3)
#ifndef PROCESS_H
#define PROCESS_H

#include <stdint.h>

/* User programs live in 1-2 GiB: no kernel memory is mapped there (the heap
 * stays below 1 GiB), and every page in it has the user bit. */
#define USER_BASE        0x40000000ULL
#define USER_TOP         0x80000000ULL

/* Exit codes for programs the kernel ends (as in Unix shells: 128 + signal). */
#define EXIT_INTERRUPTED 130   /* Ctrl+C */
#define EXIT_ILLEGAL     132   /* invalid instruction */
#define EXIT_ARITHMETIC  136   /* division by zero */
#define EXIT_SEGFAULT    139   /* page fault, protection fault */

/* Run the ELF executable `data` (the whole file) in user mode until it exits.
 * argv[0] is the program name. Returns its exit code, or a negative
 * PROC_ERR_* if it could not be started. */
int process_run(const uint8_t *data, uint32_t size, int argc, char **argv);

#define PROC_ERR_NOT_FOUND  -1
#define PROC_ERR_NOT_ELF    -2
#define PROC_ERR_NO_MEMORY  -3
#define PROC_ERR_BUSY       -4
#define PROC_ERR_NO_WINDOW  -5   /* kernel memory mapped in the user range */

const char *process_error_text(int err);

/* A CPU exception in user mode (from exception_handler): report it and end
 * the program. Does not return. */
void process_fault(uint64_t vector, const char *name, uint64_t rip, uint64_t address);

#endif
