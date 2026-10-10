// shell.h
#ifndef SHELL_H
#define SHELL_H

void shell_run(void);
int shell_execute_command(const char* line);

/* A command built into the kernel (ls, cat, ping, ...), for kcommand().
 * Returns 1 if `line` was one, 0 otherwise. */
int shell_kernel_command(const char *line);

#endif
