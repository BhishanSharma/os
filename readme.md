# Terminal OS

A tiny hobby operating system for x86_64, written from scratch in C and NASM.
It boots with GRUB, drops into 64-bit long mode and gives you a text-mode shell
with a FAT32 filesystem, a line editor, a simple script runner and an
RTL8139 network-card driver.

> Version 0.1. Terminal only, everything runs in ring 0, no userspace yet.

## Quick start

You need **Docker** (to build), **QEMU** (to run) and **Git**.
Full instructions for Windows, Linux and macOS are in [docs/SETUP.md](docs/SETUP.md).

```bash
git clone https://github.com/BhishanSharma/os.git
cd os

docker build buildenv -t myos-buildenv                  # once
docker run --rm -v "$PWD":/root/env myos-buildenv make build-x86_64
```

On Windows (PowerShell) one script does everything, including creating the
test disk and launching QEMU:

```powershell
.\build.ps1
```

The ISO ends up at `dist/x86_64/kernel.iso`.

## What works

| Area           | Status                                                                           |
| -------------- | -------------------------------------------------------------------------------- |
| Boot           | GRUB (Multiboot2) -> 32-bit stub -> long mode -> `kernel_main`                   |
| Display        | Framebuffer console (BIOS and UEFI) or VGA text, boot screen, status bar, 8 themes, scrollback; output mirrored to COM1 |
| Exceptions     | Handlers for CPU vectors 0-31, panic screen with register dump, stack guard page  |
| Input          | PS/2 keyboard, command history, arrow keys                                       |
| Memory         | Paging, heap sized from the bootloader memory map (`kmalloc`/`kfree`)            |
| User space     | Programs run in ring 3 with their own memory, system calls (`int 0x80`), a small C library, faults and Ctrl+C end only the program; see [docs/USERSPACE.md](docs/USERSPACE.md) |
| Storage        | ATA PIO disk or a RAM disk loaded by GRUB; FAT32 read/write, directories, 8.3 names |
| Shell          | ~30 commands (files, memory, disk, themes), see [docs/SHELL.md](docs/SHELL.md)   |
| Editor         | Line-based text editor (`edit <file>`)                                           |
| Scripts        | Shell scripts with variables (`sh <file>`)                                       |
| C subset       | `compile <file.c>` runs a tiny C subset on a stack VM (`printf` of a literal, `return N`) |
| Networking     | RTL8139 / RTL8168, DHCP, DNS, TCP, `ping`, and `download` over HTTP or HTTPS (BearSSL TLS 1.2); see [docs/NETWORKING.md](docs/NETWORKING.md) |

See [docs/KNOWN-ISSUES.md](docs/KNOWN-ISSUES.md) for the honest list of rough edges.

## Documentation

| Doc                                      | What's in it                                              |
| ---------------------------------------- | --------------------------------------------------------- |
| [docs/SETUP.md](docs/SETUP.md)           | Set up a build environment (Windows / Linux / macOS)      |
| [docs/DEVELOPING.md](docs/DEVELOPING.md) | Daily workflow, debugging, adding commands and drivers    |
| [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) | Boot flow, memory map, interrupts, source layout       |
| [docs/SHELL.md](docs/SHELL.md)           | Every shell command, editor keys, scripting, C subset     |
| [docs/USERSPACE.md](docs/USERSPACE.md)   | User programs: running them, writing your own, how ring 3 works |
| [docs/KNOWN-ISSUES.md](docs/KNOWN-ISSUES.md) | Bugs, limits and ideas for what to do next            |
| [docs/NETWORKING.md](docs/NETWORKING.md) | Network stack roadmap (towards `download <url>`)          |

## Everyday commands

```text
make help            list every target
make build-x86_64    build the ISO            (inside Docker)
make DEBUG=1 ...     build with debug symbols
make disk            create disk.img          (host: needs dosfstools + mtools)
make run             boot in QEMU             (host)
make debug           boot paused for gdb      (host)
make clean
make test-net        host-side unit test of the ARP/IP/ICMP code (no QEMU needed)
```

## Hardware (real or virtual)

x86_64 CPU, BIOS or UEFI boot, PS/2 keyboard (QEMU and most laptops provide one). Files live on
an IDE/ATA disk if there is one, otherwise on a 40 MiB FAT32 RAM disk that GRUB loads from the
boot medium (changes are lost at reboot; files for it go in `targets/x86_64/ramdisk/`). The ISO boots both ways; the console is drawn into the framebuffer GRUB
provides (Terminus 8x16 font, scaled up on large screens, as many columns and rows as fit), falling
back to 80x25 VGA text mode without one. Network: RTL8139 (QEMU) or RTL8168/8111 (most PCs; not yet tested on hardware).
QEMU is the supported way to run it; `.\build.ps1 -Uefi` boots it with UEFI firmware.

To try a real UEFI PC: write `dist\x86_64\kernel.iso` to a USB stick in DD/raw mode (e.g. Rufus
"DD Image"), turn off Secure Boot (GRUB here is unsigned) and boot from USB. Laptops
with NVMe/SATA-AHCI storage have no IDE disk, so the files are on the RAM disk.

---

Made with coffee and stubbornness.


### HTTPS downloads

The shell `download` command supports HTTPS using BearSSL TLS 1.2 with SNI and X.509 validation. The Docker build environment fetches upstream BearSSL and cross-builds its static library.
