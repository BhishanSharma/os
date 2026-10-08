# Known issues and next steps

Found while reading the code for the documentation. Items marked **fixed** were
changed in `0001-source-fixes.patch`; those changes compile and link but were
**not boot-tested** (see "Verify after applying the patch" in DEVELOPING.md).

## Things that will bite you

1. **The kernel image is almost out of mapped memory.** `kernel_main()` maps only
   `0x100000-0x120000` (128 KiB) for the kernel. The current image is about
   120 KiB (code, data, bss and the boot stack), i.e. ~8 KiB of headroom. Adding
   code can silently push the stack or `.bss` outside the mapped range and you
   get a triple fault with no message. `make size` now warns when you cross the
   limit. Proper fix: export `kernel_start`/`kernel_end` symbols from `linker.ld`
   and pass them to `paging_init()`.
2. **No CPU exception handlers.** Only IRQ0, IRQ1 and the NIC have IDT entries.
   Any page fault, general-protection fault or divide error triple-faults and
   QEMU resets. Install handlers for vectors 0-31 that print the vector, error
   code and `rip`; it will save hours.
3. **`help` is out of date.** It lists `exec`, `load` and `elfinfo` (not implemented)
   and omits 16 real commands (`edit`, `sh`, `compile`, `hexdump`, `diskinfo`, ...).
   The authoritative list is in SHELL.md.
4. **The "C compiler" is a stub.** It only understands `main` with `printf("...")` and
   `return N`. Tokenizing handles more than the code generator does.
5. **No networking stack.** The RTL8139 driver initialises the card and logs received
   packets (`[NET] RX pkt len=N`). There is no transmit path, ARP, IP or UDP/TCP.
6. **FAT32:** short (8.3) names only, long-file-name entries are skipped; `ls`/`tree`
   show at most 32 entries; `cat` refuses files over 4 KB.

## Fixed in the patch (verify these)

7. **Out-of-bounds IDT write (fixed).** `idt_set_entry(0x20 + 0xFF, ...)` wrote vector
   287 into a 256-entry table. The NIC handler was never actually installed at its real
   vector. Now uses `0x20 + rtl8139_get_irq()`.
8. **NIC interrupts could hang the PIC (fixed).** QEMU's RTL8139 uses IRQ 11 (slave PIC).
   The cascade line (IRQ2) was never unmasked and the NIC stub only sent EOI to the
   master. `enable_irq()` now unmasks the cascade for IRQ 8-15 and the stub EOIs both PICs.
9. **Misaligned stack in interrupt stubs (fixed).** The stubs called C with `rsp`
   misaligned by 8, which can crash on SSE instructions emitted for varargs code such as
   `kprintf`. Each call is now wrapped in `sub rsp, 8` / `add rsp, 8`.
10. **Implicit declarations (fixed).** `enable_irq` and `rtl8139_probe_init` were
    called without prototypes. Added `drivers/pic.h` and fixed includes.
11. **Red zone (fixed in Makefile).** Interrupt handlers corrupt the 128-byte red zone
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
* Linker warning "LOAD segment with RWX permissions": cosmetic for now. Add `.rodata`,
  `.data`, `.bss` sections to `linker.ld` when you want W^X.
* Remaining compiler warnings (`make` output): unused variables, signed/unsigned comparisons,
  a `const` discard at `shell.c:507`. Fix them, then enable `make WERROR=1` in CI.
* `buildenv/Dockerfile` runs `apt-get upgrade -y`, so images are not reproducible, and the
  base image is a third-party image. Pin it by digest, or build your own cross-compiler stage.

## Ideas, roughly in order of payoff

1. Exception handlers with register dump (#2) and a panic screen.
2. Export kernel bounds from the linker script (#1).
3. Read the memory size from the Multiboot2 info instead of hard-coding it.
4. Table-driven shell commands (`{name, help, handler}`), so `help` can never drift.
5. `printf`-style `-d` logging to the QEMU serial port (`-serial stdio`) so output survives a crash.
6. NIC transmit + ARP, then UDP, then a tiny TCP.
7. Make the compiler real: expressions, locals, `if`/`while`, functions.
8. Userspace: GDT with ring 3, a syscall gate, ELF loader (the `exec` placeholder in `help`).
