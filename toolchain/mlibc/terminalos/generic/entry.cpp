#include <bits/ensure.h>
#include <mlibc/elf/startup.h>
#include <stdint.h>
#include <stdlib.h>

extern "C" void __dlapi_enter(uintptr_t *);

extern char **environ;

// crt1.S: the program starts here with the kernel's stack (argc, argv, envp, auxv).
extern "C" void __mlibc_entry(int (*main_fn)(int argc, char *argv[], char *env[]), uintptr_t *entry_stack) {
	__dlapi_enter(entry_stack);
	auto result = main_fn(mlibc::entry_stack.argc, mlibc::entry_stack.argv, environ);
	exit(result);
}
