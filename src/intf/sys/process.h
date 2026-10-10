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
#define EXIT_KILLED      137   /* `kill` */
#define EXIT_SEGFAULT    139   /* page fault, protection fault */

/* Start the ELF executable `data` (the whole file) as a new task in the
 * current directory, as the current user. argv[0] is the program name.
 * Returns its pid, or a negative PROC_ERR_*. It runs alongside everything
 * else until it exits. */
int process_spawn(const uint8_t *data, uint32_t size, int argc, char **argv);

/* Wait for program `pid` to end and return its exit code. With `foreground`,
 * it gets the keyboard meanwhile and Ctrl+C ends it. */
int process_wait(int pid, int foreground);

/* Ask program `pid` to end with exit code `code` (at its next safe point). */
int process_kill(int pid, int code);

int process_foreground(void);   /* pid with the keyboard, 0 for the kernel */

/* Find a program by name: `name` or `name.elf` in the current directory, else
 * in the root of the RAM disk (the system's program folder). Returns the
 * whole file, to kfree, or 0. */
uint8_t *process_find_program(const char *name, uint32_t *size);

/* Free programs that ended after their parent did (they belong to pid 1). */
void process_reap_orphans(void);

/* Logout: stop every program and free them. */
void process_end_all(void);

#define PROC_ERR_NOT_FOUND  -1
#define PROC_ERR_NOT_ELF    -2
#define PROC_ERR_NO_MEMORY  -3
#define PROC_ERR_BUSY       -4
#define PROC_ERR_NO_WINDOW  -5   /* kernel memory mapped in the user range */

const char *process_error_text(int err);

/* A CPU exception in user mode (from exception_handler): report it and end
 * the program. Does not return. */
void process_fault(uint64_t vector, const char *name, uint64_t rip, uint64_t address);

/* For waiting kernel calls (sockets): has Ctrl+C or `kill` asked the current
 * program (or, in the kernel shell, the command) to stop? */
int process_interrupted(void);

#endif
