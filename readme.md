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
| Display        | VGA text mode, 8 colour themes, 2000-line scrollback; output mirrored to COM1    |
| Exceptions     | Handlers for CPU vectors 0-31, panic screen with register dump, stack guard page  |
| Input          | PS/2 keyboard, command history, arrow keys                                       |
| Memory         | Paging (identity mapped), first-fit heap allocator (`kmalloc`/`kfree`)           |
| Storage        | ATA PIO driver (primary master), FAT32 read/write, directories, 8.3 names        |
| Shell          | ~30 commands (files, memory, disk, themes), see [docs/SHELL.md](docs/SHELL.md)   |
| Editor         | Line-based text editor (`edit <file>`)                                           |
| Scripts        | Shell scripts with variables (`sh <file>`)                                       |
| C subset       | `compile <file.c>` runs a tiny C subset on a stack VM (`printf` of a literal, `return N`) |
| Networking     | RTL8139 driver: PCI detect, init, MAC, **send and receive raw frames**, `ifconfig`, `nettest`. No IP/TCP stack yet, see [docs/NETWORKING.md](docs/NETWORKING.md). |

See [docs/KNOWN-ISSUES.md](docs/KNOWN-ISSUES.md) for the honest list of rough edges.

## Documentation

| Doc                                      | What's in it                                              |
| ---------------------------------------- | --------------------------------------------------------- |
| [docs/SETUP.md](docs/SETUP.md)           | Set up a build environment (Windows / Linux / macOS)      |
| [docs/DEVELOPING.md](docs/DEVELOPING.md) | Daily workflow, debugging, adding commands and drivers    |
| [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) | Boot flow, memory map, interrupts, source layout       |
| [docs/SHELL.md](docs/SHELL.md)           | Every shell command, editor keys, scripting, C subset     |
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
```

## Hardware (real or virtual)

x86_64 CPU, BIOS boot (not UEFI), VGA text-mode display, PS/2 keyboard
(QEMU provides one), and for storage an IDE/ATA disk. QEMU is the supported way to run it.

---

Made with coffee and stubbornness.
