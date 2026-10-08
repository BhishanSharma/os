# Known issues and next steps

Found while reading the code for the documentation. Items that have been dealt with
move to the "Fixed" section below so you can see what changed and where.

## Open problems

1. **No CPU exception handlers.** Only IRQ0, IRQ1 and the NIC have IDT entries.
   Any page fault, general-protection fault or divide error triple-faults and
   QEMU resets. Install handlers for vectors 0-31 that print the vector, error
   code and `rip`; it will save hours.
2. **`help` is out of date.** It lists `exec`, `load` and `elfinfo` (not implemented)
   and omits 16 real commands (`edit`, `sh`, `compile`, `hexdump`, `diskinfo`, ...).
   The authoritative list is in SHELL.md.
3. **The "C compiler" is a stub.** It only understands `main` with `printf("...")` and
   `return N`. Tokenizing handles more than the code generator does.
4. **No networking stack.** The RTL8139 driver initialises the card and logs received
   packets (`[NET] RX pkt len=N`). There is no transmit path, ARP, IP or UDP/TCP.
5. **FAT32:** short (8.3) names only, long-file-name entries are skipped; `ls`/`tree`
   show at most 32 entries; `cat` refuses files over 4 KB.

## Fixed

### Kernel image almost out of mapped memory (fixed)

`kernel_main()` used to map a hard-coded `0x100000-0x120000` (128 KiB) while the image was
about 120 KiB, so adding code silently pushed `.bss` or the boot stack outside the mapped
range (triple fault, no message). Now `linker.ld` places every section explicitly and exports
`kernel_start`/`kernel_end`; `kernel_main()` maps exactly that range. The linker also
`ASSERT`s that the image stays below the heap at 2 MiB, so overgrowth is a build error
instead of a boot loop, and `make size` reports the remaining headroom (about 900 KiB).
Verified by padding the kernel with 800 KB of data/bss: it boots, while the old code
triple-faulted.

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

1. Exception handlers with register dump (#1) and a panic screen.
2. Read the memory size from the Multiboot2 info instead of hard-coding it.
3. Table-driven shell commands (`{name, help, handler}`), so `help` can never drift.
4. `printf`-style `-d` logging to the QEMU serial port (`-serial stdio`) so output survives a crash.
5. NIC transmit + ARP, then UDP, then a tiny TCP.
6. Make the compiler real: expressions, locals, `if`/`while`, functions.
7. Userspace: GDT with ring 3, a syscall gate, ELF loader (the `exec` placeholder in `help`).
