# Setting up a development environment

The kernel is built by a cross-compiler (`x86_64-elf-gcc`), `nasm` and
`grub-mkrescue`. Instead of installing those on your machine, they live in a
Docker image defined in `buildenv/Dockerfile`. You build **inside Docker** and
run **on your machine with QEMU**.

```
  your editor  ->  Docker (make build-x86_64)  ->  dist/x86_64/kernel.iso  ->  QEMU
```

## Windows

### 1. Install the tools (once)

| Tool             | How                                                                  |
| ---------------- | -------------------------------------------------------------------- |
| WSL 2            | Admin PowerShell: `wsl --install`, then reboot                        |
| Docker Desktop   | https://www.docker.com/products/docker-desktop (WSL 2 backend)       |
| QEMU             | `winget install SoftwareFreedomConservancy.QEMU`                     |
| Git              | https://git-scm.com/download/win                                     |
| VS Code          | optional, https://code.visualstudio.com                              |

Then:

1. Add `C:\Program Files\qemu` to your **PATH**, open a *new* terminal and check
   `qemu-system-x86_64 --version`.
2. Tell Git not to rewrite line endings (the repo's `.gitattributes` also
   enforces LF):
   `git config --global core.autocrlf input`

> **Do not download QEMU's `.tar.xz` source.** It has to be compiled on Linux.
> Use the installer / winget package above.

### 2. Build and run

From the project folder in PowerShell:

```powershell
.\build.ps1             # build + create disk.img if missing + run in QEMU
.\build.ps1 -NoRun      # build only
.\build.ps1 -Clean      # clean rebuild
.\build.ps1 -NewDisk    # recreate disk.img (wipes it)
.\build.ps1 -Gdb        # debug symbols, QEMU waits for gdb on localhost:1234
.\build.ps1 -Log        # QEMU logs CPU exceptions to qemu.log (-no-reboot -d int,cpu_reset)
```

If PowerShell refuses to run the script:

```powershell
Set-ExecutionPolicy -Scope CurrentUser RemoteSigned
Unblock-File .\build.ps1
```

### Doing it by hand

```powershell
docker build buildenv -t myos-buildenv                       # once
docker run --rm -v ${PWD}:/root/env myos-buildenv make build-x86_64

# once: create the FAT32 test disk (no mount/root needed)
docker run --rm -v ${PWD}:/w debian:stable-slim sh -c "apt-get update -qq && apt-get install -y -qq dosfstools mtools && cd /w && sh scripts/mkdisk.sh disk.img"

qemu-system-x86_64 -cdrom dist\x86_64\kernel.iso `
  -drive file=disk.img,format=raw,index=0,media=disk -boot d `
  -device rtl8139,netdev=n0 -netdev user,id=n0
```

### VS Code

`.vscode/tasks.json` defines tasks. Press **Ctrl+Shift+B** to build in Docker
(compiler warnings show up in the Problems panel), or **Terminal > Run Task...**
for *Run in QEMU*, *Build + Run* and *Clean*.

## Linux

```bash
sudo apt install qemu-system-x86 docker.io dosfstools mtools   # Debian/Ubuntu
docker build buildenv -t myos-buildenv                          # once
docker run --rm -v "$PWD":/root/env myos-buildenv make build-x86_64
make disk      # creates disk.img
make run       # boots in QEMU
```

Add yourself to the `docker` group (`sudo usermod -aG docker $USER`, log out and
in) to avoid `sudo` on every Docker command.

## macOS

```bash
brew install --cask docker
brew install qemu dosfstools mtools
```

then the same commands as Linux. On Apple Silicon the build image is probably
x86-only and will run under emulation (add `--platform linux/amd64`), so expect
a slow first build. This path is untested.

## Without Docker (advanced)

`make` only needs a compiler prefix and `nasm`/`grub-mkrescue` on `PATH`. If you
have an `x86_64-elf` cross toolchain installed you can run `make build-x86_64`
directly. A plain Linux `gcc` also compiles and links the kernel
(`make CROSS= build-x86_64`), which is handy for a quick syntax check, but use
the cross-compiler for anything you intend to boot.

## Troubleshooting

| Symptom                                           | Fix                                                                                 |
| ------------------------------------------------- | ----------------------------------------------------------------------------------- |
| `qemu-system-x86_64 is not recognized`            | QEMU is not on `PATH`; add `C:\Program Files\qemu` and open a new terminal           |
| `Could not open 'disk.img'`                       | Run `.\build.ps1 -NewDisk` (or `make disk`)                                          |
| `Image format was not specified ... guessed raw`  | Use `-drive file=disk.img,format=raw,...` instead of `-hda`                          |
| `docker: error during connect` / pipe not found   | Docker Desktop isn't running                                                         |
| Weird `make`/`nasm` errors, `^M` in output        | CRLF line endings; run `git add --renormalize .` and rebuild                         |
| `Makefile: *** missing separator`                 | Recipe lines must start with a TAB                                                   |
| `kernel is N bytes; kernel_main() only maps ...`  | The kernel outgrew its mapped region, see KNOWN-ISSUES.md #1                         |
| Pulling `randomdude/gcc-cross-x86_64-elf` fails   | It's a third-party image; if it disappears, build a cross-compiler image instead      |
| `grub-mkrescue: xorriso not found`                | Rebuild the image: `docker build buildenv -t myos-buildenv --no-cache`               |
