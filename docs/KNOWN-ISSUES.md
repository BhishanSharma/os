# Known issues and next steps

Found while reading the code for the documentation. Items that have been dealt with
move to the "Fixed" section below so you can see what changed and where.

## Open problems

1. **The "C compiler" is a stub.** It only understands `main` with `printf("...")` and
   `return N`. Tokenizing handles more than the code generator does.
2. **No networking stack.** The RTL8139 driver initialises the card and logs received
   packets (`[NET] RX pkt len=N`). There is no transmit path, ARP, IP or UDP/TCP.
3. **FAT32:** short (8.3) names only, long-file-name entries are skipped; `ls`/`tree`
   show at most 32 entries; `cat` refuses files over 4 KB.

## Fixed

### Outdated `help` (fixed)

`help` listed `exec`, `load` and `elfinfo` (not implemented) and omitted 16 real commands. It now lists all 37 commands the shell dispatches, grouped as in SHELL.md.

### Kernel image almost out of mapped memory (fixed)

`kernel_main()` used to map a hard-coded `0x100000-0x120000` (128 KiB) while the image was
about 120 KiB, so adding code silently pushed `.bss` or the boot stack outside the mapped
range (triple fault, no message). Now `linker.ld` places every section explicitly and exports
`kernel_start`/`kernel_end`; `kernel_main()` maps exactly that range. The linker also
`ASSERT`s that the image stays below the heap at 2 MiB, so overgrowth is a build error
instead of a boot loop, and `make size` reports the remaining headroom (about 900 KiB).
Verified by padding the kernel with 800 KB of data/bss: it boots, while the old code
triple-faulted.

### No CPU exception handlers (fixed)

Any fault used to be a triple fault: QEMU reset with no message. Now:

* `core/exc_stubs.asm` has stubs for vectors 0-31 (normalising the error code), and
  `core/exceptions.c` prints a red **panic screen**: vector, mnemonic, decoded cause (page
  fault flags and `CR2`, GP selector / "IDT vector N has no handler"), `RIP`/`RSP`/`RFLAGS`, all
  general registers, `CR0/CR3/CR4`, the top of the stack and the `addr2line` command to find
  the code. `#DB`/`#BP` just report and resume. `kpanic("msg")` gives the same screen for
  "can't happen" bugs in kernel code.
* The panic code writes straight to VGA and COM1 and uses neither `kprintf` nor the heap, so it
  works even when those are what crashed. All kernel output is now mirrored to COM1 (see
  `-Serial` / `make run`).
* A real GDT with a TSS (`core/gdt.c`) gives `#DF`, NMI and `#MC` their own IST stack.
* The boot stack now has an unmapped **guard page** below it, so a stack overflow faults
  (reported as "KERNEL STACK OVERFLOW") instead of silently corrupting memory. The guard page
  immediately exposed a real bug: `ls` and `tree` kept a `fat32_file_info_t files[32]` (8.6 KB)
  on the old 16 KB stack and had been overflowing it unnoticed. Those arrays now come from
  `kmalloc`, and the boot stack is 32 KB.
* `crash <div0|ud|gp|pf|null|stack|int3|irq|panic>` triggers each case on purpose. All were
  verified in QEMU. Not covered: a fault *before* `idt_init()` runs (first lines of
  `kernel_main`, and the 32-bit boot code, which prints its own `ERR:` codes) still resets;
  `build.ps1 -Log` remains the tool for those.

### Fixed in `0001-source-fixes.patch`

* **Out-of-bounds IDT write (fixed).** `idt_set_entry(0x20 + 0xFF, ...)` wrote vector
   287 into a 256-entry table. The NIC handler was never actually installed at its real
   vector. Now uses `0x20 + rtl8139_get_irq()`.
* **NIC interrupts could hang the PIC (fixed).** QEMU's RTL8139 uses IRQ 11 (slave PIC).
   The cascade line (IRQ2) was never unmasked and the NIC stub only sent EOI to the
   master. `enable_irq()` now unmasks the cascade for IRQ 8-15 and the stub EOIs both PICs.
* **Misaligned stack in interrupt stubs (fixed).** The stubs called C with `rsp`
   misaligned by 8, which can crash on SSE instructions emitted for varargs code such as
   `kprintf`. Each call is now wrapped in `sub rsp, 8` / `add rsp, 8`.
* **Implicit declarations (fixed).** `enable_irq` and `rtl8139_probe_init` were
    called without prototypes. Added `drivers/pic.h` and fixed includes.
* **Red zone (fixed in Makefile).** Interrupt handlers corrupt the 128-byte red zone
    below `rsp` that leaf functions may use. The build now passes `-mno-red-zone`.

## Smaller things

* `memory_init(512 * 1024)` is hard-coded rather than read from the Multiboot2 memory map,
  and the bitmap (65,536 frames = 256 MiB) is smaller than the 512 MiB it is told about.
  `alloc_frame()` can read past the bitmap. It is only used by the `alloc` test command.
* `pic_remap()`: variables `a1`/`a2` unused; `enable_irq()` lives in `keyboard.c`.
* Splash text says "Terminmal OS" in the source in this snapshot (typo).
* ATA driver polls the status register forever (no timeouts) and handles only the primary
  master drive.
* `isr_timer(registers_t regs)` takes a struct by value, which copies stack garbage.
  It should take no arguments.
* Linker warning "LOAD segment with RWX permissions": cosmetic for now. `linker.ld` now has
  separate `.text`/`.rodata`/`.data`/`.bss` sections, but they share one RWX segment; use
  `PHDRS` plus page-aligned sections and per-segment page flags in `paging.c` for real W^X.
* Remaining compiler warnings (`make` output): unused variables, signed/unsigned comparisons,
  a `const` discard at `shell.c:507`. Fix them, then enable `make WERROR=1` in CI.
* `buildenv/Dockerfile` runs `apt-get upgrade -y`, so images are not reproducible, and the
  base image is a third-party image. Pin it by digest, or build your own cross-compiler stage.

## Ideas, roughly in order of payoff

1. Read the memory size from the Multiboot2 info instead of hard-coding it.
2. Table-driven shell commands (`{name, help, handler}`), so `help` can never drift.
3. NIC transmit + ARP, then UDP, then a tiny TCP.
4. Make the compiler real: expressions, locals, `if`/`while`, functions.
5. Userspace: ring 3 segments in the GDT, a syscall gate, ELF loader (the `exec` placeholder in `help`).
