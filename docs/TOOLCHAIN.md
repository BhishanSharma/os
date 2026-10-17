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

## What is in `toolchain/`

| File | What it does |
| --- | --- |
| `Dockerfile` | binutils, mlibc headers, GCC + libgcc, mlibc, libstdc++ |
| `patch-sources.sh` | teaches binutils and GCC the OS name `terminalos` |
| `terminalos.h` | GCC's target description: crt files, static linking, predefined macros (`__terminalos__`, `__unix__`) |
| `cross-file.txt` | Meson cross file (for mlibc and Meson-built ports) |
| `mlibc/terminalos/` | mlibc's port: `generic/sysdeps.cpp` maps the C library's needs to the kernel's system calls |
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
`poll` looks at each descriptor about every 2 ms (no kernel wait yet).

Built-in files: `/dev/null`, `/dev/zero`, `/dev/urandom`, `/dev/random`,
`/etc/services`, `/etc/hosts`, `/etc/resolv.conf`. The usual names of the
CA bundle (`/etc/ssl/cert.pem`, `/etc/ssl/certs/ca-certificates.crt`...) open
`CACERT.PEM` in the system folder (Mozilla's bundle, from the toolchain image).
`alarm`/`setitimer` are accepted but never fire (no kernel timers yet).

The image also builds **OpenSSL 3.5** (static, `/etc/ssl`, randomness from
`/dev/urandom`) and **curl 8.23** (HTTP/1.1 over TLS); `curl.elf` and
`CACERT.PEM` go on the RAM disk. On the OS: `curl -I https://www.wikipedia.org/`.

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
