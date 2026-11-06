# The Terminal OS toolchain (C, C++, mlibc)

Besides the small built-in C library in `user/lib`, programs can be built with
a full toolchain: **GCC 15.2** and **binutils 2.45** for the target
`x86_64-pc-terminalos`, with **mlibc 7.0** as the C library and **libstdc++**.
That gives standard C and C++ (C++23): iostream, exceptions, `std::thread`,
`thread_local`, mutexes, `<chrono>`, `<filesystem>`-style file access, `mmap`.
Programs are linked statically.

## Building it

```sh
docker build -t myos-toolchain toolchain
```

The image is `myos-buildenv` plus `/opt/terminalos` (compilers in
`/opt/terminalos/bin`, headers and libraries in `/opt/terminalos/sysroot`).
The first build takes about an hour; it is layered so that changing the mlibc
port only rebuilds mlibc and libstdc++. `JOBS` (default 3) limits parallel
compiles: GCC's biggest files need about 1 GB each, and Docker Desktop's VM
may have only ~4 GB.

To build the OS with it, run make in that image instead of `myos-buildenv`:

```sh
docker run --rm -v "$PWD:/root/env" myos-toolchain sh -c "cd /root/env && make"
```

## Programs: `ports/NAME/`

Every folder `ports/NAME/` is one program: its `.c` and `.cpp` files are
compiled with `x86_64-pc-terminalos-gcc`/`g++` and linked into `NAME.elf` on
the RAM disk. In the plain `myos-buildenv` image (no such compiler) ports are
skipped. `ports/cxxtest` checks the toolchain: run `cxxtest` on the OS.

### Dynamic linking

Programs are linked statically unless they ask. In `myos-ladybird` (the
image `build.ps1` uses) mlibc is also built as shared libraries: the dynamic
linker `ld.so` and `libc.so` (plus the empty `libdl`, `libm`, `libpthread`,
`librt`), in the sysroot's `/usr/lib/dynamic` and on the RAM disk in `LIB`
(/SYS/LIB on an installed system, else /LIB). To link a program against
them, add `-specs=/opt/terminalos/dynamic.specs` (in `ports/NAME/LIBS`);
`-shared` with `-fPIC` objects makes a shared library.

The program then names `/lib/ld.so` as its interpreter (PT_INTERP). The
kernel loads ld.so from the system folder only (never a user's files), at
0x7E0000000000, and starts there with the program's headers in the
auxiliary vector (AT_PHDR, AT_ENTRY, AT_BASE). ld.so loads libc.so and the
rest from /SYS/LIB and /LIB (or `LD_LIBRARY_PATH`), each where mmap puts it,
then starts the program. `dlopen`/`dlsym` work. ld.so has its own small set
of system calls (`generic/rtld.cpp`: open, read, seek, close, mmap, futex,
thread pointer); the rest of the port needs the C library it is loading.

Cuts: C++ programs still link libstdc++ and libgcc statically (GCC is built
without shared libgcc_s; changing that rebuilds every image). No PIE.
Ladybird stays static. A library's pages are copied into each program, not
shared. `dyncheck` checks it (system test `dynamic`).

## Ports: other people's programs, `toolchain/ports/NAME/`

A port is a recipe, `toolchain/ports/NAME/port.sh`: where the source is
(`URL`, and its `SHA256`, checked before anything is built) and a `build`
function, run in the unpacked source with `CC`, `CXX`, `AR`, `RANLIB` and
`HOST` set for the OS (dynamically linked). What it copies into `$DEST/bin`
goes on the RAM disk, in the system folder. `build.ps1` runs
`toolchain/ports/build.sh` before the OS build when it uses `myos-ladybird`;
a source is downloaded once (`build/ports/cache`), a port is built again only
when its recipe changes, and one that cannot be downloaded is left out with a
warning. By hand: `sh toolchain/ports/build.sh lua` in the image.

| Port | What you get |
| --- | --- |
| `lua` | Lua 5.4.7: `lua`, `luac` (C modules load with `require`, dlopen) |
| `sqlite` | SQLite 3.47: `sqlite3` (no locking, no WAL: one user at a time) |
| `bash` | GNU bash 5.2: `bash`, with readline (line editing, history); no job control |
| `ncurses` | ncurses 6.5: the screen library (static; xterm and vt100 built in, no terminfo files) |
| `nano` | GNU nano 8.2: `nano`, the editor (needs `ncurses`: `NEEDS=ncurses`) |

A recipe may set `NEEDS` (ports built first; a library installs into its
`$DEST` and the next port finds it under `$PORTS_OUT/NAME`). GCC 15 compiles
C23 by default; older sources may want `CFLAGS=-std=gnu17` (ncurses, nano).
For a configure-based port, `/tc/config.sub` knows `terminalos`, and
`--host=$HOST` cross-compiles. The C library's gaps show up as configure
answers to give (`bash`'s recipe has the usual ones). `stat` and `access`
of `X` find the program `X.elf`, so shells that search `PATH` find the OS's
own programs. System test `ports` runs them.

## Terminals: pseudo-terminals and termios

The Terminal window gives its shell a pseudo-terminal (`USOCK_PTY`: the
window holds the master end, the shell and its programs have the slave as
0-2). `tcgetattr`/`tcsetattr` and the termios ioctls (`TCGETS`, `TCSETS`,
`TIOCGWINSZ`, `TIOCSWINSZ`) work on it, `isatty` says yes, and `TERM` is set
to `xterm` (a built-in `/etc/termcap` describes the window). In the cooked
mode the window edits the line (and keeps the history) and sends it whole;
Ctrl+D is an end of file. In raw mode (readline, an editor) every key goes
straight to the program, the arrows as escape sequences; Ctrl+C is SIGINT
for the program that set the mode. The window is a VT100 screen: cursor
movement, erasing, 16 colours, scroll regions, the alternate screen.
`ptytest` checks the kernel's side (system test `kernel`).

Cuts: no process groups or job control (Ctrl+Z), no UTF-8 on the screen
(bytes are the font's characters), 256 and true colours are approximated.
The kernel's text console has no modes: `tcsetattr` there does nothing.

## What is in `toolchain/`

| File | What it does |
| --- | --- |
| `Dockerfile` | binutils, mlibc headers, GCC + libgcc, mlibc, libstdc++ |
| `patch-sources.sh` | teaches binutils and GCC the OS name `terminalos` |
| `terminalos.h` | GCC's target description: crt files, static linking, predefined macros (`__terminalos__`, `__unix__`) |
| `specs`, `dynamic.specs` | the link rule: `-shared` makes a shared library; `-specs=dynamic.specs` links against the shared C library |
| `cross-file.txt` | Meson cross file (for mlibc and Meson-built ports) |
| `mlibc/terminalos/` | mlibc's port: `generic/sysdeps.cpp` maps the C library's needs to the kernel's system calls (`generic/rtld.cpp`: the dynamic linker's) |
| `mlibc/setup-port.sh` | puts the port into mlibc's source tree |

The ABI (errno numbers, `struct stat`, `O_*` flags, signal numbers) is
Linux's. Where the kernel has less than POSIX, the port fills in: `fstat` uses
the path the file was opened with, directories are listed at `opendir`,
`O_RDWR` on an existing file reads only. Missing pieces return `ENOSYS`.

## Networking, TLS, curl

`generic/net.cpp` in the port gives programs BSD sockets over the kernel's
TCP (`socket`, `connect`, `bind`/`listen`/`accept`, `send`/`recv`,
`sendmsg`/`recvmsg`, `getsockopt`, `getpeername`), `poll`/`ppoll`/`select`,
`fcntl` (`O_NONBLOCK`) and `ioctl` (`FIONBIO`, `FIONREAD`). There is no UDP
for programs: a datagram socket only asks DNS questions, answered by the
kernel's resolver, so `getaddrinfo` works unchanged (IPv4, A records).
`poll` sleeps in the kernel (`SYS_POLLWAIT`) until a pipe or local socket
changes; with TCP sockets in the set it also wakes at every interrupt.

Local (Unix-domain) stream sockets are kernel descriptors (`SYS_USOCK`):
`socket(AF_UNIX)`, `socketpair`, `bind`/`listen`/`connect`/`accept` by name
(a name is a key in the kernel's table, not a file), `O_NONBLOCK`, and files
passed with `SCM_RIGHTS` (up to 16 waiting each way). There is no half-close
and no datagram mode. Writing to a closed one gives `EPIPE`, not SIGPIPE.
`ipccheck` checks all of it: run it on the OS.

Built-in files: `/dev/null`, `/dev/zero`, `/dev/urandom`, `/dev/random`,
`/etc/services`, `/etc/hosts`, `/etc/resolv.conf`. The usual names of the
CA bundle (`/etc/ssl/cert.pem`, `/etc/ssl/certs/ca-certificates.crt`...) open
`CACERT.PEM` in the system folder (Mozilla's bundle, from the toolchain image).
`alarm`/`setitimer` are accepted but never fire (no kernel timers yet).

The image also builds **OpenSSL 3.5** (static, `/etc/ssl`, randomness from
`/dev/urandom`) and **curl 8.23** (HTTP/1.1 over TLS); `curl.elf` and
`CACERT.PEM` go on the RAM disk. On the OS: `curl -I https://www.wikipedia.org/`.

## Libraries: `myos-libs`

```sh
docker build -t myos-libs -f toolchain/libs/Dockerfile toolchain
```

`toolchain/libs/Dockerfile` builds, on top of `myos-toolchain`, static
libraries in the sysroot: zlib 1.3.2, libpng 1.6.59, libjpeg-turbo 3.2.0,
FreeType 2.15.0, HarfBuzz 14.6.0, nghttp2 1.70.0, ICU 78.3, and curl again
with HTTP/2 and gzip. CMake projects use `toolchain/cmake/TerminalOS.cmake`
and Meson projects use `libs/meson-cross.txt`. Build the OS in this image to
get the ports that need them. A port lists its link flags in
`ports/NAME/LIBS`, e.g. `-lharfbuzz -lfreetype -lpng16 -lz`.
The image's last step rebuilds the mlibc port, so a change to the port only
reruns that step, not every library.

ICU's data is not linked into programs. It is one file, `ICUDT78L.DAT`
(33 MB) on the RAM disk, which is why the RAM disk is 96 MB. Programs call
`u_setDataDirectory("/SYS")` or set `ICU_DATA`. `libcheck` checks every
library: run it on the OS.

## Kernel support it relies on

- `SYS_TCB_SET`: each thread's FS base (thread-local storage), kept across
  task switches and `fork`.
- The auxiliary vector at program start (`AT_PHDR`, `AT_PHNUM`, `AT_ENTRY`,
  `AT_RANDOM`, ...): mlibc finds the TLS template through it.
- `munmap(addr, len)`: any page range; regions shrink or split.
- `SYS_UPTIME` in milliseconds (1 ms resolution).
- `SYS_SOCKINFO` (a TCP socket: data waiting, writable, closed, ports, peer)
  and `SYS_FDINFO` (a pipe, file or the console: readable, writable) for
  `poll`; up to 64 TCP sockets.
