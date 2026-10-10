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
| `<name> [args] &`       | Runs it in the background; the shell is free at once              |
| `jobs`                  | Running programs, and which one has the keyboard                  |
| `ps`                    | Every task (shell, idle, programs) with its CPU time              |
| `fg [pid]`              | Brings a background program to the foreground (waits for it)      |
| `kill <pid>`            | Stops a program (your own; root may stop any)                     |
| Ctrl+C                  | Stops the foreground program                                      |

Several programs run at the same time. The foreground one gets the keyboard; background
programs keep running and printing, and wait if they ask for keyboard input. When a
background program ends, the shell reports `[pid] Done` before its next prompt.

```
alice@terminal-os:/HOME/ALICE$ ticker 30 &
[2] ticker running in the background
alice@terminal-os:/HOME/ALICE$ spin 20 &
[3] spin running in the background
alice@terminal-os:/HOME/ALICE$ ps
    PID  PPID  USER      STATE           CPU  NAME
      1     0  kernel    ready         0.16s  login
      0     0  kernel    ready         3.40s  idle
      2     1  alice     running       0.64s  shell  (this shell)
      4     2  alice     sleeping      0.01s  ticker
      5     2  alice     ready         1.95s  spin
```

Programs are looked up in the current directory first, then in the root of the RAM disk,
which works as the system's program directory even when the files are on the ATA disk.

## The shell is a user program

Like `login` and `bash` on Linux, the kernel only logs you in: pid 1 (`login`) checks the
password, then starts `SHELL.ELF` (`user/programs/shell.c`) as you, in your home folder, and
waits for it. Everything you type goes to that ring-3 program. For each command it:

1. runs it itself if it is a shell command: `cd`, `pwd`, `jobs`, `ps`, `fg`, `kill`, `su`,
   `history`, `exit`;
2. otherwise asks the kernel to run it if it is one of the commands built into the kernel
   (`ls`, `cat`, `edit`, `ping`, `download`, ...) with the `kcommand` system call, which runs
   it as you, in the shell's directory;
3. otherwise starts the program of that name with `spawn` and `wait`s for it, or leaves it
   in the background for a trailing `&`.

It also has `a ; b`, `a && b`, "quoted arguments", `$?`, `$USER`, `$HOME`, `$PWD`, and
history on Up/Down. Ctrl+C clears the line you are typing (the shell asks for Ctrl+C as a
key with `ctrlc(CTRLC_KEY)`), and stops the program in the foreground.

`su [user]` starts a second shell as that user: `spawn("shell", argv, "root")` makes the
kernel ask for that user's password (unless you are root) and run the new program as them.
`exit` ends it and you are back in your own shell. `exit` in the login shell logs you out;
any programs still running are stopped.

If `SHELL.ELF` is missing or crashes, pid 1 falls back to the shell built into the kernel,
which has the same commands.

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
| `ticker [s] [every]`  | Prints the time every second; run it with `&`                  |
| `spin [s]`            | Keeps the CPU busy; the timer still shares it with everyone    |
| `shell`               | The command line itself: `spawn`, `wait`, `kcommand`, line editing |
| `paint`               | Draw with the mouse (`getmouse`): left paints, right erases, wheel picks the colour |

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
  `uptime_ms`, `gettime`), keys (`getkey`, which does not wait, and `waitkey`), the screen
  (`clear_screen`, `gotoxy`, `set_color`, `set_theme_color`, `reset_color`, `console_size`,
  `cursor_column`, `show_cursor`), the mouse (`getmouse`: pointer cell, buttons, wheel), and programs: `spawn(path, argv, as_user)`,
  `wait(pid, &status, flags)` (`WAIT_NOHANG`, `WAIT_FOREGROUND`; pid -1 = any child),
  `kill`, `taskinfo`, `chdir`, `getcwd`, `kcommand`, `ctrlc`, `uname`, `os_strerror`

Programs are compiled with `-mgeneral-regs-only`: no `float`/`double` yet.

## How it works

* **Memory.** Programs are static ELF executables linked at 1 GiB (`user/user.ld`). Each
  program has its own page tables: the range 1-2 GiB holds its segments, its heap (grown by
  `sbrk`, up to 256 MiB) and a 256 KiB stack ending at 2 GiB, while the kernel is mapped the
  same way in every program (`paging_create_address_space`). Only the program's pages have
  the *user* bit; the kernel heap stays below 1 GiB. When a program ends, its page tables and
  pages are freed.
* **Tasks and scheduling** (`sys/task.c`). Every program is a task with its own kernel
  stack; task 1 (`login`) logs users in and an idle task (pid 0) halts the CPU when nobody
  has work. Every program has a parent: the one that spawned it, which collects its exit
  code with `wait`. When a parent ends first, its programs are handed to pid 1, which frees
  them once they end.
  The timer interrupt switches programs every 5 ticks (50 ms), but only when it interrupts
  user code. Kernel code is never preempted: it gives up the CPU only where it waits
  (keyboard, `sleep`, waiting for a program), so drivers and the file system need no locks.
  Each program also has its own open files and current directory.
* **Entering and leaving ring 3.** `core/gdt.c` has user code and data segments (DPL 3);
  the scheduler points `TSS.rsp0` at the running program's kernel stack. A new program
  starts in `task_start_user` (`core/taskswitch.asm`), which `iretq`s into it. `exit`, a
  fault, Ctrl+C or `kill` frees the program and leaves a zombie task whose exit code the
  shell collects.
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
* **Ctrl+C and kill.** Both mark the program; it ends at its next safe point: the next
  timer tick in user mode (so even `for (;;) {}` stops), a system call, or while it waits
  for input or sleeps.

## Limits (next steps)

* Up to 14 programs at once (counting the shells). There are no pipes (`a | b`) or output
  redirection (`> file`) yet, and no `exec` (replacing the running program).
* No floating point in programs, and no priorities: every program gets the same time slice.
* Files are read whole into memory on `open` and written back on `close`.
