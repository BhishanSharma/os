# Developing Terminal OS

## The inner loop

```
edit code  ->  build in Docker  ->  boot in QEMU  ->  look at the screen  ->  repeat
```

| Platform | One command                       | What it does                                         |
| -------- | --------------------------------- | ---------------------------------------------------- |
| Windows  | `.\build.ps1`                     | build (Docker) + create `disk.img` if needed + QEMU  |
| Linux/mac| `docker run ... make build-x86_64` then `make run` | same, in two steps (see SETUP.md)    |
| VS Code  | `Ctrl+Shift+B`                    | build in Docker, warnings land in the Problems panel |

Incremental builds work: `make` tracks header dependencies (`-MMD`), so editing a
`.h` rebuilds exactly the files that include it. You only need `make clean` if you
change the Makefile or linker script and something looks stale.

`make` ends with a `kernel image: ... bytes of headroom` line. The kernel is mapped from
the linker's `kernel_end`, so it only matters if headroom reaches zero, and then the link
itself fails with an explanatory `ASSERT` message (see ARCHITECTURE.md, "Memory map").

## Build options

```text
make build-x86_64            normal build
make DEBUG=1 build-x86_64    -g -O0, for gdb
make WERROR=1 build-x86_64   warnings are errors
make CROSS= build-x86_64     use the host gcc instead of x86_64-elf-gcc (syntax check only)
```

## Debugging

### 1. A red "KERNEL PANIC" screen

A CPU exception (page fault, divide error, invalid opcode, ...) stops the machine on a panic
screen with the cause and all registers. Find the code with the `RIP` it prints:

```bash
x86_64-elf-addr2line -e dist/x86_64/kernel.bin 0x<RIP>   # needs a DEBUG=1 build; run in Docker
```

`CR2` is the faulting address for page faults. "KERNEL STACK OVERFLOW" means recursion or big
local arrays: move large buffers to `kmalloc`. Call `kpanic("why")` yourself for impossible
conditions. To see the panic screen on purpose, run `crash pf`, `crash div0`, `crash stack`, etc.

### 2. It still reboots / flickers with no message

That's a fault before `idt_init()` runs (very early boot) or a failure inside the panic code
itself. Ask QEMU to say what happened:

```powershell
qemu-system-x86_64 -m 512M -cdrom dist\x86_64\kernel.iso -drive file=disk.img,format=raw,index=0,media=disk -boot d `
  -no-reboot -d int,cpu_reset -D qemu.log
```

(or `.\build.ps1 -Log`). `-no-reboot` freezes at the fault instead of resetting; `qemu.log`
lists every exception with its vector, error code and `RIP`.

### 2b. Serial console

All kernel output (boot log, shell, panic reports) is mirrored to COM1. Add `-serial stdio`
to the QEMU command (`make run` and `.\build.ps1 -Serial` do) and it appears in your terminal,
where it survives a wiped screen and can be copied or logged.

### 3. gdb

```bash
make DEBUG=1 clean build-x86_64     # in Docker
make debug                          # host: QEMU waits on :1234
# second terminal, host (needs gdb with x86_64 support):
gdb dist/x86_64/kernel.bin -ex "target remote localhost:1234" -ex "hbreak kernel_main" -ex continue
```

Use `hbreak` (hardware breakpoints) because the CPU switches modes early in boot.

### 4. QEMU monitor

`Ctrl+Alt+2` in the QEMU window opens the monitor (`info registers`, `info pci`,
`xp /16x 0x200000`); `Ctrl+Alt+1` returns to the screen.

## Recipes

### Add a shell command

In `src/impl/x86_64/sys/shell.c`, add a branch to `shell_execute_command()`:

```c
else if (strncmp(line, "hello ", 6) == 0)
{
    kprintf("Hello, %s!\n", line + 6);
}
```

Then add it to `cmd_help()` and to `docs/SHELL.md`. (Commands are a hand-written
`if/else` chain today; converting them to a `{name, help, fn}` table is a good first refactor.)

### Add a driver

1. Header: `src/intf/drivers/mydev.h` with the public API.
2. Source: `src/impl/x86_64/drivers/mydev.c`; `#include "drivers/mydev.h"` and
   `"../lib/ports.h"` for `inb`/`outb`. The Makefile finds new `.c` files automatically.
3. Call `mydev_init()` from `kernel_main()` in `src/impl/kernel/main.c`.
4. If it uses an interrupt:
   * add an assembly stub in `core/irq.asm` (copy `irq_nic_stub`; it must `iretq`),
   * `idt_set_entry(0x20 + irq, your_stub, 0x8E);`
   * `enable_irq(irq);` (from `drivers/pic.h`),
   * send EOI: `0x20` to port `0x20`, and also to `0xA0` for IRQ 8-15.

### Add a source file in a new folder

Anything under `src/impl/x86_64/**` or `src/impl/kernel/**` is picked up; headers go
in `src/intf/<same area>/`.

## Conventions

* Include project headers relative to `src/intf` (`"drivers/fat32.h"`).
* Freestanding C: no libc. Use `kprintf`, `kmalloc`/`kfree` and the helpers in `lib/string.h`.
* There is one 32 KiB boot stack (with an unmapped guard page below it, so overflow panics
  instead of corrupting memory). Keep big buffers off the stack: use `kmalloc`.
* Line endings are LF (`.gitattributes`, `.editorconfig`). If you edit on Windows and see
  `^M` or odd `nasm`/`make` errors, run `git add --renormalize .`.
* Keep `make` warnings from growing; fix any you introduce.

## Applying `0001-source-fixes.patch`

The patch was made against the original snapshot of the source. Apply it from the
project root:

```powershell
git apply --check 0001-source-fixes.patch      # dry run
git apply 0001-source-fixes.patch
```

If it doesn't apply cleanly because you changed the same files since, use
`git apply --3way 0001-source-fixes.patch` and resolve any conflict markers, or
ask for the changes to be re-applied to your latest sources.

### Verify after applying the patch

The changes were compiled and linked but not run. Boot once and check:

1. The boot log still shows keyboard, timer, `[NET] ... init complete`, `[NET] NIC driver installed`,
   `[OK] FAT32 mounted from the ATA disk` (or `... from the RAM disk` without one), `Boot complete!`.
2. The shell responds to keys, `ls` lists `TEST.TXT`/`README.TXT`, `cat test.txt` works.
3. `uptime` increases (timer interrupts still work); `sleep 2` returns.
4. The NIC receive interrupt can't easily be triggered yet (the OS never transmits,
   so QEMU has nothing to answer). Until ARP/TX exist, treat the NIC IRQ fix as
   verified only by "boots and stays alive".

If step 1 fails, `git apply -R 0001-source-fixes.patch` reverts everything.
