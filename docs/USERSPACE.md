# User programs

Terminal OS runs programs in **user mode (ring 3)**, separated from the kernel. A program
cannot read or write kernel memory, run privileged instructions or hang the machine: if it
tries, the kernel ends the program and the shell keeps running.

```
root@terminal-os:/# hello one two
Hello from user space!
...
root@terminal-os:/# fault kernel
Reading kernel memory at 0x100000...
[ERROR] fault: Segmentation fault (kernel or unmapped memory) at address 0x100000 ... - program terminated
```

## Running programs

| Command                 | What it does                                                      |
| ----------------------- | ----------------------------------------------------------------- |
| `<name> [args]`         | Runs `<name>` or `<name>.elf` (if `<name>` is not a shell command) |
| `run <file> [args]`     | Runs a file by name                                               |
| `programs`              | Lists the programs in the current directory and on the RAM disk   |
| Ctrl+C                  | Stops the running program                                         |

The shell looks in the current directory first, then in the root of the RAM disk, which
works as the system's program directory even when the files are on the ATA disk.

## Included programs

| Program               | Shows                                                         |
| --------------------- | ------------------------------------------------------------- |
| `hello [args]`        | Arguments, addresses in user space, the clock, `malloc`       |
| `guess`               | Keyboard input: guess a number between 1 and 100              |
| `snake`               | A real-time game: arrow keys or WASD, P pause, Q quit         |
| `primes [limit]`      | A sieve in a `malloc`'d table (default 1,000,000)             |
| `wc <file>...`        | Reading files: lines, words, bytes                            |
| `note [text]`         | Writing files: appends a time-stamped line to `NOTES.TXT`     |
| `fault <kind>`        | Breaks a rule on purpose: `kernel`, `write`, `null`, `div`, `cli`, `loop` |

## Writing your own

1. Create `user/programs/NAME.c` (8 characters at most, the FAT short-name limit):

   ```c
   #include <stdio.h>
   #include <os.h>

   int main(int argc, char **argv) {
       printf("Hi, %s!\n", argc > 1 ? argv[1] : "world");
       return 0;
   }
   ```

2. Build as usual (`.\build.ps1`). Every file in `user/programs/` becomes `NAME.elf` on the
   RAM disk.
3. In the OS, type `NAME`.

The C library (`user/include`, `user/lib`) has:

* `stdio.h`: `printf`, `snprintf`, `sprintf`, `puts`, `putchar`, `getchar`, `readline`
  (formats `%d %u %x %p %c %s`, widths and zero padding; no floating point)
* `stdlib.h`: `malloc`, `calloc`, `realloc`, `free`, `atoi`, `atol`, `rand`, `srand`, `exit`
* `string.h`, `ctype.h`: the usual string, memory and character functions
* `os.h`: who is running it (`getuser`), files (`open`, `read`, `write`, `close`, `unlink`, `readdir`), time (`sleep_ms`,
  `uptime_ms`, `gettime`), keys (`getkey`, which does not wait) and the screen (`clear_screen`,
  `gotoxy`, `set_color`, `reset_color`, `console_size`, `show_cursor`)

Programs are compiled with `-mgeneral-regs-only`: no `float`/`double` yet.

## How it works

* **Memory.** Programs are static ELF executables linked at 1 GiB (`user/user.ld`). The
  range 1-2 GiB belongs to the running program: its segments, then its heap (grown by
  `sbrk`, up to 256 MiB), and a 256 KiB stack ending at 2 GiB. These pages are the only ones
  with the *user* bit set; the kernel's identity-mapped memory below 1 GiB is not. The kernel
  heap is kept below 1 GiB for this. When the program ends, all its pages are freed.
* **Entering and leaving ring 3.** `core/gdt.c` has user code and data segments (DPL 3) and
  sets `TSS.rsp0`, the kernel stack the CPU switches to on an interrupt in user mode.
  `core/usermode.asm`: `user_enter` saves the kernel's registers and `iretq`s to the program;
  `user_return` (from `exit`, a fault or Ctrl+C) restores them, so `process_run()` returns
  the exit code.
* **System calls.** `int 0x80` (an IDT gate with DPL 3), number in `rax`, arguments in `rdi`,
  `rsi`, `rdx`, result in `rax`. The numbers are in `src/intf/sys/syscall_nums.h`, shared by
  the kernel (`sys/process.c`) and the library. Every pointer a program passes is checked
  against its page tables before the kernel touches it.
* **Users.** A program runs as the logged-in user. Opening a file for writing or deleting
  one outside that user's home folder fails with `SYSERR_PERM` (root may write anywhere);
  see [SHELL.md](SHELL.md#users-and-login).
* **Faults.** `exception_handler` checks the privilege level of the faulting code: in ring 3
  it prints what happened and ends the program instead of showing the kernel panic screen.
  Exit codes follow Unix shells: 139 segmentation fault, 136 division by zero, 132 invalid
  instruction, 130 Ctrl+C.
* **Ctrl+C.** The keyboard interrupt sets a flag. The timer interrupt, when it lands in user
  mode, ends the program if the flag is set, so even `for (;;) {}` can be stopped. Blocking
  calls (keyboard input, `sleep_ms`) check it too.

## Limits (next steps)

* One program at a time, in the foreground: no multitasking or background jobs yet.
* All programs share the same address range, so there is no `fork`/`exec` from a program.
* No floating point in user programs (the kernel does not save FPU/SSE state yet).
* Files are read whole into memory on `open` and written back on `close`.
