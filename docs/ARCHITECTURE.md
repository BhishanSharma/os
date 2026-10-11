# Architecture

## Source layout

```
buildenv/Dockerfile            Build container (cross-gcc, nasm, grub, xorriso)
Makefile                       Build system  (`make help`)
build.ps1                      Windows one-shot: build + disk + QEMU
scripts/mkdisk.sh              Creates a FAT32 test disk without root
scripts/mkramdisk.sh           Builds the gzipped FAT32 RAM disk image (boot/ramdisk.img.gz)
user/                          User programs (ring 3): crt0, linker script, C library, programs/
targets/x86_64/
  linker.ld                    Kernel linked at 1 MiB, entry symbol `start`, exports kernel_start/kernel_end
  iso/boot/grub/grub.cfg       GRUB menu: multiboot2 /boot/kernel.bin, module2 /boot/ramdisk.img.gz
  ramdisk/                     Files copied into the RAM disk image
src/
  intf/                        Public headers  (-I src/intf, include as "drivers/fat32.h")
    core/  drivers/  lib/  sys/
  impl/
    kernel/main.c              kernel_main(): initialises everything, starts the shell
    x86_64/
      boot/                    header.asm (Multiboot2), main.asm (32->64 bit), main64.asm
      core/                    gdt.c (GDT+TSS), idt.c, exceptions.c (panic screen), isr.c,
                               exc_stubs.asm (vectors 0-31), irq.asm (IRQ stubs), gdt_load.asm, idt_load.asm
      drivers/                 ata, fat32, heap, keyboard, memory, paging, pci, pic, rtl8139, timer
      lib/                     print (VGA + kprintf), serial (COM1), string, string_utils, compiler, ports.h
      sys/                     shell, editor, script, system (reboot)
```

Headers live in `src/intf`, implementations in `src/impl`. A header such as
`src/intf/drivers/pic.h` is included as `#include "drivers/pic.h"`.
`src/impl/x86_64/lib/ports.h` (port I/O helpers `inb`/`outb`...) is private and
included with a relative path.

## Boot flow

```
UEFI -> TERMOS.EFI (loader/, 64-bit)    or    BIOS/UEFI -> GRUB (the legacy ISO)
  both load kernel.bin's segments at their physical addresses from 1 MiB
  start64 (main.asm, from TERMOS.EFI)      start (main.asm, 32-bit, from GRUB)
                                             check multiboot magic, CPUID, long mode
    save the multiboot2 info pointer in `multiboot_info`
    boot page tables, 2 MiB pages: the first 4 GiB at 0 (identity) and at the
    direct map, the first 1 GiB at KERNEL_VMA; long mode, a minimal GDT
  enter_high: jump to the kernel's own addresses (top 2 GiB)
  long_mode_start  (main64.asm, 64-bit)
    zero the data segment registers, call kernel_main
  kernel_main      (kernel/main.c)
```

`main.asm` lives in section `.boot`, linked at its load address; everything
else is linked at `KERNEL_VMA` + its load address (`targets/x86_64/linker.ld`),
so before paging it refers to kernel symbols as `PHYS(symbol)`.

`kernel_main` then, in order:

0. `serial_init()` (all output is mirrored to COM1 from here on). If GRUB provided a graphics
   framebuffer (header.asm asks for one; GRUB provides it under both UEFI and BIOS), the
   text grid moves to RAM, sized to fill the screen (`fbcon_grid_size`), and the framebuffer is registered for paging. Then `gdt_init()`.
1. Sets the colour theme, clears the screen, prints the banner.
2. `idt_init()` (which also installs the CPU exception handlers), `pic_remap()`; installs the IRQ0 (timer) and IRQ1 (keyboard) stubs.
3. `init_keyboard()`, `timer_init()` (PIT at 100 Hz). (`memory_init()` already ran in step 0:
   it copies the usable-RAM regions from the multiboot2 memory map, and the heap is placed in the
   largest one.)
4. `paging_init()` builds the kernel's own page tables (the kernel image at `KERNEL_VMA`; in the direct map all RAM and ACPI memory, the page tables, the heap, the first MiB, and the registered ranges: framebuffer, boot modules, the multiboot information) and switches to them. The lower half is now empty: it belongs to programs. With a framebuffer, `fbcon_init()` then starts the console (`lib/fbcon.c`, font from `scripts/gen-font.py`). Then `heap_init()`.
5. `expand_scrollback()`: grows the scrollback buffer to 2000 lines.
6. `nic_probe_init()`: RTL8139 or RTL8168; installs its IRQ handler if it has a PIC line.
7. `mount_filesystem()`: FAT32 from the ATA disk if `ata_init()` finds one with a FAT32 volume,
   otherwise from the RAM disk; then `cd /`. Both go through `drivers/disk.c`, which routes
   `disk_read_sectors`/`disk_write_sectors` to the ATA driver or to the RAM disk in memory.
8. `sti` (enable interrupts), DHCP (`net_configure()`), then `shell_run()`, which never returns.

## Virtual memory (`src/intf/drivers/addr.h`)

The kernel lives in the upper half of every address space; the lower half is
the running program's own (128 TiB). A program's PML4 shares entries 256-511
with the kernel's, so kernel mappings are the same everywhere.

| Virtual address              | What                                                        |
| ---------------------------- | ----------------------------------------------------------- |
| `0x0000000000010000`-`0x00007FFFFFFFF000` | The program: code at 4 MiB (`user/user.ld`), heap after it, stack at the top |
| `0xFFFF800000000000` (`DIRECT_MAP`) | All of RAM: physical `X` is at `DIRECT_MAP + X` (`phys_to_virt`). The heap, page tables and programs' pages are reached here |
| `0xFFFFC00000000000` (`MMIO_WINDOW`) | Device registers, uncached (`mmio_map`) |
| `0xFFFFC08000000000` (`ACPI_WINDOW`) | ACPI tables being scanned (`acpi.c`) |
| `0xFFFFFFFF80000000` (`KERNEL_VMA`) | The kernel image: `KERNEL_VMA` + its physical address (`-mcmodel=kernel`) |

Devices see physical addresses: drivers hand them `virt_to_phys(buffer)`
(the heap and the kernel image both work), never the pointer. While the
other cores start, the kernel's own tables also map the SMP trampoline page at
`0x8000` to itself. Firmware calls (`efi.c`) run on a copy of the kernel's
tables plus identity mappings of the firmware's runtime memory.

## Physical memory

| Address                  | What                                                        |
| ------------------------ | ----------------------------------------------------------- |
| `0x000B8000`             | VGA text buffer                                             |
| `0x00100000`-`kernel_phys_end` | Kernel image (boot code, text/rodata/data/bss, 32 KiB boot stack with a guard page below it); `kernel_phys_end` comes from `linker.ld`, currently about `0x1EB000` |
| `0x00400000`-`0x00600000`| Page-table pool for `paging_init()` (`PAGE_TABLE_AREA`..`PAGE_TABLE_END`) |
| start of the heap region | RAM disk: GRUB loads `ramdisk.img.gz` (unpacked) wherever it likes; `kernel_main()` keeps the heap out of that range (`memory_reserve`), copies the image to the bottom of the chosen region and starts the heap at the next 2 MiB boundary |
| largest RAM region in 6 MiB-1 GiB | Kernel heap (`kmalloc`), 2 MiB aligned, at most 1 GiB, mapped with 2 MiB pages; falls back to 1 MiB at `0x600000` without a memory map |

The kernel image is mapped from the linker-provided `kernel_start`/`kernel_end`, so it can grow
freely up to the page-table pool at `0x400000`. `linker.ld` has an `ASSERT` that fails the link if
the image would reach it; `make size` prints the headroom. The heap stays below 4 GiB because the
NIC drivers hand `kmalloc` buffers to the card for DMA with 32-bit addresses. If the kernel ever
needs more than 3 MiB, move the pool (`PAGE_TABLE_AREA`/`PAGE_TABLE_END` in `paging.c`) and
`HEAP_LOW` in `kernel_main()`, then raise `KERNEL_LIMIT` in `linker.ld`.

## Interrupts

* IDT: 256 entries, kernel code selector `0x08`, interrupt-gate flags `0x8E`.
* The 8259 PICs are remapped so IRQ 0-7 -> vectors `0x20`-`0x27` and IRQ 8-15 -> `0x28`-`0x2F`.
  `pic_remap()` masks everything except IRQ1; drivers call `enable_irq(n)` to unmask theirs
  (for IRQ 8-15 it also unmasks the cascade line, IRQ2).
* Handlers are assembly stubs in `core/irq.asm` that save registers, call a C
  handler, send EOI and `iretq`:

  | Vector        | Stub            | C handler            |
  | ------------- | --------------- | -------------------- |
  | `0x20` (IRQ0) | `irq0_stub`     | `isr_timer`          |
  | `0x21` (IRQ1) | `irq1_stub`     | `isr_keyboard`       |
  | `0x20 + NIC IRQ` | `irq_nic_stub` | `rtl8139_handle_irq` |

### CPU exceptions

`idt_init()` installs a gate for every vector 0-31 (`core/exc_stubs.asm`). Each stub pushes a
fake error code when the CPU doesn't supply one, saves all registers and calls
`exception_handler()` in `core/exceptions.c` with a `struct exc_frame*`. `#DB` and `#BP` report
and return; everything else draws the panic screen (vector, decoded cause, registers, `CR0-CR4`,
stack words) on VGA and COM1, then halts. The panic code uses its own tiny writer, not
`kprintf`, so it still works if the print/heap code is what crashed.

`gdt_init()` builds a GDT (`0x08` code, `0x10` data, `0x18` TSS). The TSS has one IST stack
(8 KiB, `IST_FATAL`) used by NMI, `#DF` and `#MC`, so a corrupted kernel stack still produces a
readable report. The boot stack has an unmapped guard page below it (`stack_guard` in
`main.asm`, skipped by `paging_init()`): overflowing the stack raises `#PF`, then `#DF`, and the
panic screen says "KERNEL STACK OVERFLOW".

Faults before `idt_init()` (early `kernel_main`, the 32-bit boot code) are still triple faults;
use `build.ps1 -Log` for those.

## Drivers

| Driver     | File                       | Notes                                                           |
| ---------- | -------------------------- | --------------------------------------------------------------- |
| Display    | `lib/print.c`, `lib/fbcon.c` | Text grid: 80x25 in VGA memory or, on a framebuffer, screen-sized in RAM and drawn by `fbcon`; themes, scrollback, `kprintf` (mirrors to serial) |
| Serial     | `lib/serial.c`             | COM1 115200 8N1, polled; mirrors all kernel output              |
| Keyboard   | `drivers/keyboard.c`       | Scancode set 1, line input with history, shift/ctrl             |
| Timer      | `drivers/timer.c`          | PIT channel 0 at 100 Hz, `get_tick`, `sleep(ms)`                |
| PIC        | `drivers/pic.c`            | 8259 remap; `enable_irq()` lives in `keyboard.c` for historical reasons |
| Paging     | `drivers/paging.c`         | 4-level tables; the kernel in the upper half, a direct map of RAM, programs in the lower half |
| Frames     | `drivers/memory.c`         | Bitmap physical-frame allocator (barely used)                   |
| Heap       | `drivers/heap.c`           | First-fit free list, 8-byte alignment, `kmalloc`/`kfree`        |
| Disk       | `drivers/disk.c`           | Sector I/O for FAT32: the ATA disk or the RAM disk (`mount`)    |
| ATA        | `drivers/ata.c`            | PIO, LBA28, primary bus master drive, polling with ~1 s timeouts |
| FAT32      | `drivers/fat32.c`          | Long names (VFAT) with 8.3 aliases; names compared without case  |
| PCI        | `drivers/pci.c`            | Config-space access via ports `0xCF8`/`0xCFC`, device lookup    |
| RTL8139    | `drivers/rtl8139.c`        | Init, RX ring, interrupts; logs received packet lengths; no TX  |

## System layer

* `sys/shell.c`: one big `if/else` chain in `shell_execute_command()`.
* `sys/editor.c`: line-based editor on top of the FAT32 API.
* `sys/script.c`: runs a file line by line through the shell, with `VAR=value`, `$VAR`, `echo`, `sleep`, `exit`.
* `lib/compiler.c`: tokenizer + a code generator that currently understands only `main` containing `printf("literal")` and `return N`, executed on a small stack VM.
* `sys/system.c`: `reboot()` via the keyboard controller reset line.
