# Terminal OS: user space, desktop, apps, ports, developer tooling, testing and project health (audit of the local repo, 7 Nov 2026)

How to read the citations: paths are relative to the repo root `C:\Users\hp\Desktop\os` at HEAD `3c9f14b` (branch main, clean). `file:N-M` means lines N to M. Commit hashes refer to that repo. Everything comes from reading the code and docs. Nothing was built or run, so "works" means the code or a doc says so, and nothing here was checked at run time.

Size of the user-space code (tracked files, `wc -l`): native libc and toolkit `user/lib` about 4,380 lines (13 files); `user/include` 22 headers; `user/programs` 50 programs plus `windowserver/` (3,891 + 200 + 22 lines); `apps/` 11 apps, about 4,900 lines in total; mlibc port `toolchain/mlibc/terminalos` about 3,000 lines; Ladybird overlay 1,307 lines plus `patch.py` 274 lines. The kernel-side shell, editor, script runner and C-subset VM are `src/impl/x86_64/sys/shell.c` (1,665), `sys/editor.c` (366), `sys/script.c` (231) and `lib/compiler.c` (676).

## 1. C library and runtime: native libc, mlibc port, C++, dynamic linking, pthreads, locale

### Takeaway
There are two C libraries. The native one (`user/lib` + `user/include`, about 4.4k lines, 22 headers) is a small, non-POSIX SDK libc. It has no FILE streams on files, no environment, no signal.h/setjmp.h/dirent.h/locale.h/wchar.h, and ASCII-only ctype, and every built-in app and `.tapp` package is built against it. The mlibc 7.0 port with GCC 15.2/libstdc++ (C++23) is the real POSIX layer: about 108 sysdeps are implemented and everything else returns ENOSYS. It runs bash, Vim, curl, OpenSSL, SQLite, Lua and a statically linked Ladybird. Dynamic linking is lean (ld.so plus libc.so; C++ stays static; no PIE; no shared pages). Locales are absent in both libraries.

### Cited Findings
**Native libc (the "SDK")**
- `fopen` and `fdopen` are declared "always 0: use open() (os.h)", and stdout and stderr both go to the console. So there are no stdio streams on files. — [user/include/stdio.h:22-34](user/include/stdio.h)
- printf supports `%d %i %u %x %X %o %p %c %s %f %e %g`, flags, width and precision. `getchar` reads "from the keyboard line; EOF never". — [user/include/stdio.h:9-20](user/include/stdio.h)
- `getenv` is "always 0: there is no environment". — [user/include/stdlib.h:37](user/include/stdlib.h)
- ctype is 8 inline ASCII functions (no `isxdigit`, `isprint`, `ispunct` or `iscntrl`). — [user/include/ctype.h:4-11](user/include/ctype.h)
- `sys/stat.h` is "only a file's size, for ported code": `struct stat` has `st_mode`, `st_size` and `st_mtime` only. — [user/include/sys/stat.h:1-14](user/include/sys/stat.h)
- `unistd.h` has 8 functions (read, write, close, lseek, unlink, getpid, usleep, sleep). — [user/include/unistd.h:5-12](user/include/unistd.h)
- `open()` can only read, or create/truncate for writing (or append). Writes are saved to disk at `close`. — [user/include/fcntl.h:1-2](user/include/fcntl.h); [user/include/os.h:33-34](user/include/os.h)
- pthreads cover create, join, self, attr stack size, plain mutexes, condition variables (which may wake spuriously) and once. There are no rwlocks, TLS keys, detach or cancel. — [user/include/pthread.h:1-41](user/include/pthread.h)
- math.h comes "from musl" (about 70 functions), but "programs built with -mgeneral-regs-only cannot use it". Only ported files such as FFmpeg and minimp3 get SSE. — [user/include/math.h:1-3](user/include/math.h); [docs/APPS.md:196-202](docs/APPS.md)
- The headers that exist are only: args, assert, ctype, door, errno, fcntl, image, inttypes, math, os, pthread, stdio, stdlib, string, sys/stat, sys/types, time, ui, unistd, view, ws. — `git ls-files user/include` (22 files)
- System calls go through `int 0x80` (rax plus rdi/rsi/rdx/r10). The API is Terminal-OS specific: `spawn`, `wait(pid,&st,flags)`, single-argument `kill(pid)`, `listdir`, `kcommand`, `mmap(len,prot,fd)`, `shm_map(name,size)`, Mach-style `port_*`, and `signal()` handlers that run only "when the program next makes a system call". — [user/include/os.h:10-28](user/include/os.h), [os.h:60-68](user/include/os.h), [os.h:117-159](user/include/os.h)
- Docs disagree with each other. USERSPACE.md still says "no float/double yet", "a 256 KiB stack", "Up to 14 programs at once... no pipes... no exec", and "Kernel code is never preempted... drivers and the file system need no locks". This contradicts ROADMAP (8 MiB growable stacks, pipes, exec, SMP) and APPS.md (`%f`, `strtod`, math.h). — [docs/USERSPACE.md:145](docs/USERSPACE.md), [USERSPACE.md:149-152](docs/USERSPACE.md), [USERSPACE.md:161-164](docs/USERSPACE.md), [USERSPACE.md:187-190](docs/USERSPACE.md); [docs/ROADMAP.md:6-10](docs/ROADMAP.md)

**mlibc port (toolchain/mlibc/terminalos)**
- Toolchain: GCC 15.2 and binutils 2.45 for `x86_64-pc-terminalos`, mlibc 7.0 and libstdc++. It gives "standard C and C++ (C++23): iostream, exceptions, std::thread, thread_local... mmap". Programs are linked statically by default. — [docs/TOOLCHAIN.md:3-8](docs/TOOLCHAIN.md)
- The port lists its implemented sysdeps explicitly, "everything else is ENOSYS". About 108 tags: files (Open, Openat, Read, Write, Seek, Pread/Pwrite, Stat, OpenDir/ReadEntries, Mkdir/Rmdir, Unlinkat, Rename(at), Chmod, Fchmodat, Fchownat, Ftruncate, Fsync, Flock, Readlink), process (Fork, Execve, Waitpid, Kill, Clone, PGID/SID), signals (Sigaction, Sigprocmask, Sigaltstack), TLS (TcbSet), futexes, mmap/mprotect/madvise, termios, rlimits, sockets (21 in net.cpp), Poll/Ppoll/Pselect, Fcntl, Ioctl, and Set/GetItimer. — [toolchain/mlibc/terminalos/include/mlibc/sysdeps.hpp:7-118](toolchain/mlibc/terminalos/include/mlibc/sysdeps.hpp)
- Not implemented (grep of `Sysdeps<...>` in `generic/*.cpp` returns 0 hits): Setuid, Setgid, Seteuid, Getgroups, Symlink, Link, Mknod, Utimensat (file times), Statvfs/Fstatvfs, Getrusage, Times, Setpriority, Msync, Mlock, Pause, Sigsuspend, Epoll, Eventfd, Timerfd, Inotify, Openpt/Ptsname (POSIX pty). — [toolchain/mlibc/terminalos/generic/sysdeps.cpp](toolchain/mlibc/terminalos/generic/sysdeps.cpp), [generic/net.cpp](toolchain/mlibc/terminalos/generic/net.cpp)
- Stubs and approximations:
  - `O_RDWR` on an existing file opens it read-only: "reading works; writes fail with EBADF". — [sysdeps.cpp:488-501](toolchain/mlibc/terminalos/generic/sysdeps.cpp)
  - `setitimer` and `alarm` are "accepted and never fire". — [sysdeps.cpp:1162-1175](toolchain/mlibc/terminalos/generic/sysdeps.cpp)
  - The `*at()` calls with a real directory fd (renameat, fchmodat, fchownat, unlinkat) return ENOSYS. — [sysdeps.cpp:836-876](toolchain/mlibc/terminalos/generic/sysdeps.cpp)
  - Directories are listed once at `opendir`, and `fstat` works by the path the file was opened with. — [sysdeps.cpp:1-8](toolchain/mlibc/terminalos/generic/sysdeps.cpp), [docs/TOOLCHAIN.md:135-138](docs/TOOLCHAIN.md)
- Sockets: BSD sockets run over the kernel's TCP. "There is no UDP for programs: a datagram socket only asks DNS questions" (IPv4 A records only). AF_UNIX stream sockets support SCM_RIGHTS, but there is no half-close and no datagram mode, and a write to a closed socket gives EPIPE, not SIGPIPE. — [docs/TOOLCHAIN.md:142-156](docs/TOOLCHAIN.md)
- `fsync` "returns at once". `stat("X")` also finds `X.elf`, so PATH searches find OS programs. Vim needed `fsync`, `umask`, `flock`, `fchdir`, `sigaltstack`, `sysinfo` and `ttyname` added. — [docs/TOOLCHAIN.md:85-91](docs/TOOLCHAIN.md)
- There is a glibc-compatibility shim (stat64, openat64, `syscall()`, `gnu_get_libc_version`, `__res_init`). Its comment mentions Rust falling back "the portable way". — [toolchain/mlibc/terminalos/generic/glibc_compat.cpp:5-72](toolchain/mlibc/terminalos/generic/glibc_compat.cpp)
- A ROADMAP cut is stale. ROADMAP says "the mlibc port still reports made-up modes until it is rebuilt with statx", but the port now calls `TOS_STATX` and returns real modes. — [docs/ROADMAP.md:65-66](docs/ROADMAP.md) vs [sysdeps.cpp:113-124](toolchain/mlibc/terminalos/generic/sysdeps.cpp)

**C++, dynamic linking, threads**
- Dynamic linking: mlibc is also built as `ld.so` and `libc.so` (plus empty libdl, libm, libpthread and librt) in /SYS/LIB. Linking with `-specs=/opt/terminalos/dynamic.specs` sets PT_INTERP `/lib/ld.so`. The kernel loads ld.so "from the system folder only" at 0x7E0000000000, and `dlopen`/`dlsym` work. — [docs/TOOLCHAIN.md:36-54](docs/TOOLCHAIN.md); [toolchain/dynamic.specs](toolchain/dynamic.specs) (9 lines)
- Cuts: "C++ programs still link libstdc++ and libgcc statically (GCC is built without shared libgcc_s...). No PIE. Ladybird stays static. A library's pages are copied into each program, not shared." — [docs/TOOLCHAIN.md:55-58](docs/TOOLCHAIN.md); [docs/ROADMAP.md:87-96](docs/ROADMAP.md)
- The ld.so sysdeps are only open, read, seek, close, stat, mmap, mprotect, futex and TCB (15 tags). — [toolchain/mlibc/terminalos/generic/rtld.cpp](toolchain/mlibc/terminalos/generic/rtld.cpp) (132 lines)
- Check programs: `ports/cxxtest` (143 lines, toolchain check), `ports/libcheck` (175 lines, checks every library), `ports/dyncheck` (17 lines, system test `dynamic`), plus `ipccheck`, `netcheck`, `shmcheck` and `fontcheck`. — [docs/TOOLCHAIN.md:31-34](docs/TOOLCHAIN.md), [TOOLCHAIN.md:184-187](docs/TOOLCHAIN.md); `git ls-files ports`
- Kernel support the port relies on: `SYS_TCB_SET` (FS base, so TLS works and ROADMAP's earlier "no TLS" cut is stale), the auxv, partial munmap, a 1 ms uptime clock, and at most 64 TCP sockets. — [docs/TOOLCHAIN.md:189-199](docs/TOOLCHAIN.md); [docs/ROADMAP.md:11-13](docs/ROADMAP.md)

**Locale**
- The native libc has no `locale.h` (header list above). The mlibc port adds no locale code (a grep for locale, setlocale or LC_ in `toolchain/mlibc/terminalos` finds nothing), so mlibc's built-in defaults apply. ICU 78.3 is cross-built for ports, and its 33 MB data file sits on the RAM disk. — [docs/TOOLCHAIN.md:174-187](docs/TOOLCHAIN.md)

### Inferences
- The SDK is split. Native apps and `.tapp` packages use a non-POSIX libc, while real POSIX software needs the mlibc toolchain images. A third-party developer cannot write a `.tapp` against mlibc/C++, because the package docs describe only the native SDK ([docs/PACKAGES.md:61-72](docs/PACKAGES.md)). Converging on mlibc, with the UI toolkit ported to it, is the obvious fix.
- The mlibc coverage (about 108 sysdeps) is in the same range as other mid-stage mlibc hobby OSes. The missing setuid/setgid family, file times, statvfs, epoll/eventfd/timerfd and UDP will block many ports: daemons, sudo/su, rsync, make's timestamps, and event-loop software such as GLib, libuv and Node.
- `O_RDWR` being read-only plus "whole file written at close" ([docs/KNOWN-ISSUES.md:17-19](docs/KNOWN-ISSUES.md)) means databases and editors that update files in place are fragile. TOOLCHAIN.md admits SQLite runs with "no locking, no WAL" ([docs/TOOLCHAIN.md:75](docs/TOOLCHAIN.md)).

### Gaps
- I could not confirm at run time which mlibc locale functions work (no build or run). mlibc upstream's locale support was not checked against its repo.
- I did not count every libc function in `user/lib/libc2.c` and `stdio.c` one by one. The header list stands in for coverage.

## 2. Shell and command-line userland

### Takeaway
There are three shells. The native `shell` (user program, 574 lines) has pipes, `<`, `>`, `>>`, `2>&1`, `;`, `&&`, `&`, four fixed variables and a 32-entry history. It has no globbing, no `||`, no user variables and no control flow. The kernel shell (1,665 lines, about 70 commands, a hand-written if/else chain) remains the home of the hardware commands, reached via `kcommand`. GNU bash 5.2 is the real scripting shell, with readline and job control. About 25 coreutils-like programs exist. find, sed, awk, diff, less, tar, gzip, xargs, ln and du are missing. Vim and nano are ported. The kernel's line editor and C-subset "compiler" are toys.

### Cited Findings
- Native shell design: it runs builtins itself, else `kcommand()` for kernel commands, else `spawn` and `wait`. `;`, `&&`, `$?`, `$USER`, `$HOME` and `$PWD`. Limits: LINE_MAX 200, MAX_ARGV 16, HISTORY 32, MAX_JOBS 16. — [user/programs/shell.c:1-20](user/programs/shell.c)
- Variable expansion is only `$?`, `$USER`, `$HOME` and `$PWD`. — [user/programs/shell.c:184-195](user/programs/shell.c)
- Pipelines: "a | b | c, each with < file, > file, >> file, 2>&1. Only programs". They are built with `pipe` and `dup2` before `spawn`. — [user/programs/shell.c:381-470](user/programs/shell.c)
- There is no globbing, tab completion, `||`, `if`/`while` or `export` in the native shell (grep of shell.c finds no glob or `*` expansion or completion code). Docs list only `;`, `&&`, quotes and the four variables. — [docs/SHELL.md:6-9](docs/SHELL.md)
- The kernel script runner `sh FILE` allows 4,096-byte scripts, 256-character lines and 16 variables. "Any line containing `=` is treated as an assignment, even `echo a=b`". — [src/impl/x86_64/sys/script.c:8-12](src/impl/x86_64/sys/script.c); [docs/SHELL.md:233-249](docs/SHELL.md)
- The kernel shell's `help` lists about 70 commands in groups (files, programs, users, system, memory/disk debug, network, appearance). Dispatch is a hand-written `if/else` chain, and a table is suggested as "a good first refactor". — [src/impl/x86_64/sys/shell.c:188-260](src/impl/x86_64/sys/shell.c); [docs/DEVELOPING.md:114-126](docs/DEVELOPING.md); [docs/KNOWN-ISSUES.md:140](docs/KNOWN-ISSUES.md)
- User programs (`user/programs`, 50 files) include coreutils-like tools: cat, chmod, chown, cp (-r), date, echo, grep (-i -n -v -c -r; `^ $ . *` only), head, tail, hexdump, ls (-l -a), mkdir, mv, pwd, rm (-r -f), sleep, sort (-r -n -u), touch, tree, uname, uptime, wc, write, fileinfo and meminfo. Others are system pieces (launchd, login, loginwindow, logd, windowserver, tapp, crashrep, notify, view), demos and tests (snake, guess, primes, spin, ticker, forktest, vmtest, ptytest, threads, attack, fault, porttest) and network tools (fetch, httpd, echod). — `git ls-files user/programs`; [docs/SHELL.md:55-75](docs/SHELL.md)
- Ports give `bash` 5.2 (readline, job control: Ctrl+Z, `jobs`, `fg`, `bg`), `vim` 9.1 (no runtime folder, `:help` has no pages), `nano` 8.2, `ncurses` 6.5 (xterm/vt100 built in, no terminfo files), `lua` 5.4.7, `sqlite3` 3.47 and `curl` 8.23. — [docs/TOOLCHAIN.md:72-79](docs/TOOLCHAIN.md), [TOOLCHAIN.md:164-166](docs/TOOLCHAIN.md)
- Job control (process groups and sessions, SIGTSTP/SIGSTOP/SIGCONT, WUNTRACED/WCONTINUED, tcsetpgrp) works for bash. Cut: "no SIGTTIN when a background job reads the terminal". — [docs/TOOLCHAIN.md:107-120](docs/TOOLCHAIN.md); [docs/ROADMAP.md:103-108](docs/ROADMAP.md)
- The kernel `edit` is a line-based editor: up to 8,000 lines, Ctrl+E edits lines of up to 255 characters, Ctrl+N new line, Ctrl+D delete line. — [src/impl/x86_64/sys/editor.c:9-10](src/impl/x86_64/sys/editor.c); [docs/SHELL.md:215-230](docs/SHELL.md)
- The C-subset compiler is a stub. `generate_simple_code` handles only `printf("literal")` and `return N`, and the VM has push, arithmetic and syscall-print opcodes. — [src/impl/x86_64/lib/compiler.c:428-472](src/impl/x86_64/lib/compiler.c); [docs/KNOWN-ISSUES.md:8-9](docs/KNOWN-ISSUES.md); [docs/SHELL.md:250-264](docs/SHELL.md)
- There are no man pages or `--help` conventions beyond `help`, and no `/usr/share` layout (Vim's `:help` is empty). — [docs/TOOLCHAIN.md:79](docs/TOOLCHAIN.md)

### Inferences
- Compared with a standard Unix userland (POSIX utilities or GNU coreutils, about 100 tools), the native set covers roughly a quarter. With bash and Vim ported, a technical user can script and edit, but missing text tools (sed, awk, find, diff, less, tar, gzip) block most real workflows. Porting BusyBox or toybox through the existing `toolchain/ports` recipe system would close most of this in one step, if the mlibc gaps allow.
- Three overlapping shells (kernel, native, bash) and two command namespaces (kernel `kcommand` vs programs) are technical debt. Moving the remaining kernel commands into user programs, as ROADMAP stage 6 planned ([docs/ROADMAP.md:242-244](docs/ROADMAP.md)), is unfinished.

### Gaps
- The exact count of kernel-shell commands (the help text was read only to line 260) and whether every `help` entry is implemented were not checked one by one.

## 3. Desktop: window server, login, Rail/Launcher, toolkit, design system

### Takeaway
`windowserver` (3,891 lines, one file) is a capable single-screen compositor. Windows are shared-memory surfaces with damage rectangles, the server composites on all cores with up to 8 painter threads, frames are page-flipped at vsync with GPU blur and shadows on Intel Gen12, and clients wake through futexes. It has a polished DOOR design-token look (light/dark, 8 accents, contrast, reduce motion/transparency), a Rail, Launcher with app and file search, notifications, a lock screen, screenshots and a shared text clipboard. Window management is basic. There are at most 16 windows, the only resize is the maximise (zoom) button, and there is no Alt+Tab, snapping, virtual desktops, per-app menus, drag and drop, multi-monitor support or fractional scaling. Only left-button press/drag events reach apps.

### Cited Findings
- Architecture (from the file's own comment): programs ask the `windowserver` port for a window and draw into shared memory, and the server composites back to front "only where something changed (a list of damaged rectangles)". The UI is a Rail (Lobby, Apps, Files, Spaces, Create, Settings), a Status pill (search, notifications, Wi-Fi, sound, battery, clock, light/dark), a Launcher panel and glass blur at 1/4 resolution. Shortcuts: Win opens the Launcher; Win+W closes, Win+L locks, Win+Q logs out. — [user/programs/windowserver/main.c:1-30](user/programs/windowserver/main.c)
- Protocol: `WS_CREATE`, `WS_TITLE`, `WS_RESIZE`, `WS_RESIZABLE`, `WS_APPS`. The surface is a 4 KiB header plus 0x00RRGGBB pixels, with a 64-entry event ring and a dirty rectangle. Events: KEY, DOWN, UP, MOVE, WHEEL, CLOSE, RESIZE, PREFS. There are no focus-in/out, text-input/IME, drag-and-drop or per-button events. — [user/include/ws.h:13-45](user/include/ws.h), [ws.h:111-131](user/include/ws.h)
- Limits: `MAX_WINDOWS 16`, `MAX_DAMAGE 32`. Window creation fails with SYSERR_AGAIN when 16 are open. — [main.c:39-40](user/programs/windowserver/main.c), [main.c:1461-1462](user/programs/windowserver/main.c)
- Placement is a cascade from the top left, clamped to the screen. Resizing only goes through the maximise control (`zoom_window`), and only for apps that call `ui_set_resizable`. Each new size gets a new shared-memory surface. There is no edge or corner drag-resize. — [main.c:1479-1486](user/programs/windowserver/main.c), [main.c:1498-1525](user/programs/windowserver/main.c), [main.c:1527-1563](user/programs/windowserver/main.c), [main.c:3320-3346](user/programs/windowserver/main.c)
- Input routing:
  - `WS_EV_DOWN` is sent only on a left-button press. — [main.c:3349](user/programs/windowserver/main.c), [main.c:3391](user/programs/windowserver/main.c)
  - `WS_EV_MOVE` is sent only while the left button is held over a captured window. APPS.md instead says it is sent "while over the window", so apps get no hover events. — [main.c:3386-3388](user/programs/windowserver/main.c) vs [docs/APPS.md:146](docs/APPS.md)
  - Wheel events go to the window under the pointer. Keys go to the front window. Other Windows+ combinations are dropped. — [main.c:3414-3416](user/programs/windowserver/main.c), [main.c:3796-3810](user/programs/windowserver/main.c)
- A latent bug: Terminal pastes on `WS_EV_DOWN && (e->buttons & 2)` (right button), but the server never sends DOWN for a right-only press, so the advertised "a right click pastes" probably never fires. — [apps/terminal/main.c:17](apps/terminal/main.c), [terminal/main.c:724](apps/terminal/main.c) vs [main.c:3391](user/programs/windowserver/main.c)
- Frame pacing:
  - After presenting, the main loop waits for the frame to reach the screen (`gfx_wait`), then reads input. — [main.c:3750-3755](user/programs/windowserver/main.c)
  - `gfx_present` is "tear-free on Intel graphics" and runs without the lock. The comment there records an earlier bug: holding the lock across present "starved the thread that opens windows". — [main.c:3866-3871](user/programs/windowserver/main.c)
  - Without pacing the loop sleeps 5 ms. — [main.c:3889](user/programs/windowserver/main.c)
- Parallel compositing: painter threads, "one per core, up to 8", each paint a horizontal band. — [main.c:3699-3716](user/programs/windowserver/main.c), [main.c:2905-2917](user/programs/windowserver/main.c)
- Kernel interface: `gfx_surface` lets the GPU copy frames straight from the server's buffer. `gfx_gpu` and `gfx_gpu_draw` run frosted glass, fades, shadows and corners on Intel shaders with a CPU fallback. `gfx_cursor` gives a hardware or kernel-drawn pointer. — [user/include/os.h:71-93](user/include/os.h); [main.c:1871-1881](user/programs/windowserver/main.c) (after 8 GPU failures in a row it uses the CPU); [readme.md:47](readme.md)
- Scale and multi-monitor:
  - The display is one `gfx_info` screen (W, H). The scale `S` is an integer, 2 only on screens of at least 2560x1600, else 1. "Larger Text" forces 2x only on screens at least 1600 px wide. — [main.c:3659-3671](user/programs/windowserver/main.c), [main.c:3685-3690](user/programs/windowserver/main.c)
  - There is no multi-monitor code (one framebuffer) and no fractional scaling.
- Look and theming: two hard-coded themes (light, dark) derived from `design/tokens.json`, with Increase Contrast raising rim and text contrast and 8 accent presets. Apps get semantic colour tokens (`ui_color(UI_C_*)`) and a `WS_EV_PREFS` redraw event. — [main.c:56-123](user/programs/windowserver/main.c); [user/include/ws.h:47-58](user/include/ws.h); [user/include/ui.h:15-49](user/include/ui.h); [design/tokens.json](design/tokens.json) (258 lines); [user/include/door.h:1-56](user/include/door.h)
- Session features:
  - A shared settings segment with System Settings (wallpaper gradient, colour or picture, Dock size, magnify, autohide, clock format, pointer, large text), saved to `DESKTOP.CFG`. — [ws.h:60-81](user/include/ws.h)
  - A clipboard of text only, at most 64 KiB, per user. — [ws.h:83-92](user/include/ws.h)
  - Notifications in a 16-entry ring (title 48 bytes, body 120 bytes), shown as banners and kept under the bell. — [ws.h:94-109](user/include/ws.h); [main.c:1120-1203](user/programs/windowserver/main.c)
  - A lock screen ([main.c:966-1010](user/programs/windowserver/main.c)) and Print Screen saving a PNG to ~/Pictures ([main.c:1014-1118](user/programs/windowserver/main.c)).
  - Launcher search indexes home-folder files. — [main.c:583-606](user/programs/windowserver/main.c)
- "Spaces" is the panel listing open windows, not virtual desktops. — [main.c:572-574](user/programs/windowserver/main.c), [main.c:2719-2731](user/programs/windowserver/main.c)
- File associations are hard-coded by extension in two places (Files and the Launcher): pictures go to `view`, videos to `video`, songs to `music`, programs to `terminal`, text to `textedit`, `.tapp` to `tapp`. There is no registry of handlers or "Open with". — [apps/finder/main.c:121-143](apps/finder/main.c), [finder/main.c:258-259](apps/finder/main.c); [main.c:617](user/programs/windowserver/main.c)
- Known cuts in the docs: "no per-app menus" ([docs/ROADMAP.md:23](docs/ROADMAP.md)), "no drag and drop between apps" ([ROADMAP.md:121-122](docs/ROADMAP.md)), and shared memory and ports not limited by the sandbox ([ROADMAP.md:84-85](docs/ROADMAP.md)).
- Possible security issue: window surfaces are shared-memory objects named `ws.<id>`, and the server trusts a client-supplied `pid` in requests. — [main.c:1466](user/programs/windowserver/main.c), [main.c:1533](user/programs/windowserver/main.c); [ws.h:21-27](user/include/ws.h)
- loginwindow (579 lines):
  - It shows an animated "door" boot-to-desktop transition, a sign-in screen only if a PIN or password is set, a profile picker for several accounts, and a first-start Setup Assistant (name only). Typing `>console` gives the text login. — [user/programs/loginwindow.c:1-19](user/programs/loginwindow.c)
  - launchd starts it as the `console` service. — [user/programs/launchd.c:1-29](user/programs/launchd.c)
- Toolkit (`ui.h` immediate drawing plus `view.h` retained tree):
  - Primitives: anti-aliased rounded rectangles, circles, shadows, clipping, icons, text. Widgets are drawn by the app and hit-tested with `ui_inside`. — [user/include/ui.h:51-169](user/include/ui.h)
  - `view.h` adds a flexbox-like layout and minimal redraw. Its components are box, label, text, button, icon, table, field, slider and custom, plus `v_alert` and `v_on_close`. — [user/include/view.h:21-41](user/include/view.h), [view.h:84-122](user/include/view.h)
  - Limits: at most 8 table columns, 512 rows and 128-byte text per view. There are no checkbox, radio, popup/menu, tab, scroll view, tree, multi-line text widget or file open/save dialog, and apps hand-draw these as custom views. — [view.h:31](user/include/view.h), [view.h:40-41](user/include/view.h), [view.h:56](user/include/view.h)
  - The library is `user/lib/ui.c` (1,141 lines) and `user/lib/view.c` (864 lines). — `wc -l`
- App code may not use floating point (`-mgeneral-regs-only`). — [docs/APPS.md:196-203](docs/APPS.md)

### Inferences
- For visual polish, the compositor and design system look closer to macOS/GNOME than to typical hobby OSes. GPU-offloaded blur, vsync pacing, damage tracking and multi-core painting are more than SerenityOS's or Haiku's servers do on the GPU. Breadth is far behind: SerenityOS/Haiku/Redox all have arbitrary resizing, window menus, many more widgets, drag and drop, and (Haiku/Serenity) multi-monitor or virtual-desktop support. I place C10 around 4.
- Cheap, high-value fixes in the window server: edge resize, Alt+Tab, right/middle button and hover events, raising MAX_WINDOWS, and access control on surface shm names.

### Gaps
- Whether the GPU path is exercised on real hardware beyond the author's laptop is unknown. docs/GPU.md was not read in detail.
- I did not check whether `ui_list` or tables scroll with keyboard focus for all widgets ("full keyboard access" claim, [docs/ROADMAP.md:119-120](docs/ROADMAP.md)).

## 4. Text, internationalisation and accessibility

### Takeaway
Text is anti-aliased but static. Inter and JetBrains Mono are pre-rendered at build time with Pillow into `UIFONT.BIN` (938 code points, integer scales 1-4, no kerning or shaping, '?' for anything missing). This covers Latin, Greek, Cyrillic and Vietnamese only, with no CJK, RTL, complex scripts or emoji text. Input is US-QWERTY only, with no layouts, dead keys, compose or IME. The time zone is compiled in as IST (UTC+05:30), and there are no locales or translations. Accessibility is Increase Contrast, Reduce Motion, Reduce Transparency, a 2x "Larger Text" (from next login, wide screens only) and Tab focus. There is no screen reader, zoom, sticky keys or on-screen keyboard. FreeType, HarfBuzz and ICU exist only as cross-built libraries for ports (Ladybird).

### Cited Findings
- `scripts/gen-uifont.py` pre-renders Inter (weights 430 and 640, scales 1-4) and JetBrains Mono (scales 1 and 2) at 13 px per 16 px line into UIFONT.BIN. Ranges: Latin-1, Latin Ext-A, Greek, Cyrillic, Vietnamese, punctuation, currency, arrows, maths, box drawing, blocks, U+FFFD. A glyph missing from one face is taken from the other. — [scripts/gen-uifont.py:1-63](scripts/gen-uifont.py)
- That gives 938 characters. Cuts: "no CJK or right-to-left text, no rich text". — [docs/ROADMAP.md:113-115](docs/ROADMAP.md), [ROADMAP.md:121-122](docs/ROADMAP.md)
- The renderer looks up a glyph or draws '?', applies tracking per face (macOS-style values) and has no kerning pairs or shaping. — [user/lib/ui.c:366-381](user/lib/ui.c), [ui.c:470-505](user/lib/ui.c); [user/include/ui.h:101-117](user/include/ui.h)
- FreeType 2.15.0, HarfBuzz 14.6.0 and ICU 78.3 are cross-built static libraries in the `myos-libs` image (for ports). `ports/fontcheck` is 48 lines. — [docs/TOOLCHAIN.md:168-187](docs/TOOLCHAIN.md)
- Keyboard: the PS/2 driver has only `kbdus`/`kbdus_shift` tables. USB HID usages are translated to PS/2 set-1 scancodes, so USB keyboards are also US-only. — [src/impl/x86_64/drivers/keyboard.c:41](src/impl/x86_64/drivers/keyboard.c), [keyboard.c:189](src/impl/x86_64/drivers/keyboard.c); [src/impl/x86_64/drivers/xhci.c:1289-1331](src/impl/x86_64/drivers/xhci.c)
- Time zone: `RTC_LOCAL_OFFSET_MIN 330` and `RTC_LOCAL_TZ_NAME "IST"` are compile-time constants. VMs with a UTC clock show times "5:30 ahead". — [src/intf/drivers/rtc.h:30-31](src/intf/drivers/rtc.h); [src/impl/x86_64/sys/process.c:992](src/impl/x86_64/sys/process.c); [readme.md:103-104](readme.md)
- No locale or translation infrastructure exists. There is no `locale.h` in the SDK, and a grep for setlocale, gettext, LC_ALL, iconv or bidi in user/, apps/ and the mlibc port finds nothing. All UI strings are English literals in C.
- FAT32 long names are ASCII only (other characters show as `?`). — [docs/KNOWN-ISSUES.md:10](docs/KNOWN-ISSUES.md)
- Text input: TextEdit and Terminal handle UTF-8. Input events carry a key code, not text, so non-ASCII input depends on the keymap. — [apps/textedit/main.c:1](apps/textedit/main.c); [apps/terminal/main.c:18-19](apps/terminal/main.c); [user/include/ws.h:40-45](user/include/ws.h)
- Accessibility settings: "Increase contrast", "Reduce motion", "Reduce transparency", "Larger text (from the next login)". — [apps/settings/main.c:389-393](apps/settings/main.c)
  - Larger Text means "everything twice as large... on a screen at least 1600 pixels wide". — [user/include/ws.h:79-80](user/include/ws.h)
  - The focus ring follows the HIG rule and is fully opaque under contrast. — [user/include/ui.h:143-146](user/include/ui.h)
  - "Full keyboard access (Tab between fields and buttons)". — [docs/ROADMAP.md:119-120](docs/ROADMAP.md)
- There is no screen reader, TTS, magnifier, sticky or slow keys, on-screen keyboard or captions. A grep for screen reader, speech, tts, narrator or a11y finds only the settings above. There is no accessibility tree in `view.h` (no roles or labels on the `struct view`). — [user/include/view.h:47-82](user/include/view.h)

### Inferences
- C15 is low: about 1.5-2 for internationalisation and about 2 for accessibility. The retained `view_t` tree is a natural place to hang an accessibility tree and text-input/IME events. The ported FreeType, HarfBuzz and ICU could replace the pre-rendered atlas, which would give runtime sizes, fractional scaling, shaping, RTL/CJK and fallback fonts.
- Hard-coded IST and a US keymap make the system region-locked to the author's locale. Settings has no Keyboard, Language & Region or Date & Time panes ([apps/settings/main.c:18-22](apps/settings/main.c)).

### Gaps
- Whether Tab focus reaches every control in every app was not checked at run time.

## 5. Applications (apps/ and notable user programs)

### Takeaway
There are 11 desktop apps in about 4.9k lines (the largest are Terminal at 782 lines and Settings at 776). Each is a credible first version: Files (browse and search, no file operations), Terminal (pty, VT100, UTF-8), TextEdit (plain-text editing with undo/redo and find), Settings (10 panes), Music, Videos (FFmpeg), Paint, Calendar, Calculator, Activity Monitor and Console. All are well below their Windows/macOS/GNOME counterparts. There is no web browser among the native apps; Ladybird is a port (section 6). There is no mail, PDF/document viewer, archive manager, office suite, software store or printing.

### Cited Findings
- **Files** (`finder`, 474 lines): a sidebar (Home, Temporary, System Disk), Back/Forward, a filter-as-you-type search, a Name/Size/Kind list, a path bar and type-ahead. Opening is by extension. It has no copy, move, rename, delete, new folder, trash or drag and drop (a grep for rename, delete, unlink, copy, mkdir, trash or drag finds no file-operation code). — [apps/finder/main.c:1-21](apps/finder/main.c), [finder/main.c:121-143](apps/finder/main.c), [finder/main.c:258-259](apps/finder/main.c)
- **Terminal** (782 lines): a pty with cooked or raw mode, a VT100 screen (cursor, erase, 16 colours, scroll regions, alternate screen), scrollback, selection to the clipboard and UTF-8 cells. It has no tabs, splits or profiles. "256 and true colours are approximated". — [apps/terminal/main.c:1-23](apps/terminal/main.c); [docs/TOOLCHAIN.md:93-121](docs/TOOLCHAIN.md)
- **TextEdit** (450 lines): UTF-8 plain text, Shift-selection, word moves, mouse selection, clipboard, undo/redo implemented as "whole copies of the text before each change", Find and Find Next, and Save. An unsaved-changes warning arrived in the latest commit. Missing: rich text, tabs, line numbers, syntax highlighting, encodings other than UTF-8, and printing. — [apps/textedit/main.c:1-14](apps/textedit/main.c); commit `3c9f14b`; [user/include/view.h:114-116](user/include/view.h)
- **System Settings** (776 lines): panes for Wi-Fi, Sound, Battery, Appearance, Wallpaper, Dock & Menu Bar, Mouse & Trackpad, Sign-in (PIN or password), Display and About. Missing panes: keyboard, language and region, date and time, users and groups, updates, notifications, privacy and sandbox, default apps, Bluetooth, printers, network (Ethernet/proxy/VPN). — [apps/settings/main.c:1-22](apps/settings/main.c), [settings/main.c:389-393](apps/settings/main.c), [settings/main.c:527-640](apps/settings/main.c)
- **Videos** (423 + 583 lines `ff.c`): pl_mpeg for MPEG-1, FFmpeg for MP4, MOV, MKV, WebM and AVI (H.264, HEVC, VP9, AAC, Opus). "Decoding runs on one core". There is no hardware decode. — [apps/video/main.c:1-12](apps/video/main.c); [docs/APPS.md:241](docs/APPS.md)
- **Music** (421 lines): plays one MP3, MP2 or WAV file (minimp3, CC0) resampled to 48 kHz. It has no library, playlists, tags, gapless playback, FLAC, AAC or Ogg. — [apps/music/main.c:1-8](apps/music/main.c)
- **Paint** (210 lines): twelve colours, three brush sizes, an eraser and BMP save. — [apps/paint/main.c:1-7](apps/paint/main.c)
- **Calendar** (306 lines): a month view, events in `~/Calendar/events.txt`, and reminders "while Calendar is open" only. It has no recurrence, ICS or CalDAV. — [apps/calendar/main.c:1-10](apps/calendar/main.c); [apps/calendar/app.cfg](apps/calendar/app.cfg)
- **Calculator** (198 lines): + - x / with percent, sign and repeat-equals, plus the keyboard and clipboard. There is no scientific or programmer mode. — [apps/calculator/main.c:1-6](apps/calculator/main.c)
- **Activity Monitor** (125 lines): a process table with CPU and memory and a quit button. **Console** (128 lines): the live system log. — [apps/activity/main.c:1-6](apps/activity/main.c); [apps/console/main.c:1-9](apps/console/main.c)
- Notable user programs:
  - `tapp`: the package installer, with a window or a CLI. — [user/programs/tapp.c:1-12](user/programs/tapp.c)
  - `crashrep`: the "quit unexpectedly" report. — [user/programs/crashrep.c:1-4](user/programs/crashrep.c)
  - `view`: an image viewer for PNG, JPEG, GIF, BMP, TGA, PSD and PPM. — [user/programs/view.c:1-3](user/programs/view.c); [user/include/image.h:1-34](user/include/image.h)
  - `notify`: posts a notification. — [user/programs/notify.c:1-4](user/programs/notify.c)
  - `httpd`, `fetch` and `snake`. — `git ls-files user/programs`
- The app test opens every app at once and fails if one crashes. — [docs/APPS.md:224-233](docs/APPS.md)

### Inferences
- Measured against SerenityOS (about 40+ apps, including a browser, spreadsheet, PDF viewer, IDE and games) or Haiku (a full suite plus a package ecosystem), the native app set is thin but coherent. Every app follows one design system and one toolkit, which is unusual for a hobby OS this young. C11 (apps and ecosystem) is held up mostly by ported CLI software and Ladybird, not by native apps.
- The highest-leverage native app work is Files operations (copy, move, rename, delete, trash), a document/PDF viewer and a richer TextEdit. Users notice these first.

### Gaps
- I could not check app behaviour at run time or compare performance.

## 6. Ported software, cross-building and packaging (including Ladybird)

### Takeaway
The toolchain is a six-image Docker chain: buildenv, then toolchain, libs, browser, skia and ladybird. It cross-builds GCC 15.2 with mlibc 7.0, about 30 checksummed libraries (zlib, libpng, libjpeg-turbo, FreeType, HarfBuzz, ICU, nghttp2, OpenSSL, curl, brotli, woff2, libwebp, dav1d, libavif, libxml2, SQLite, simdutf, fmt, mimalloc and others), and CMake/Meson cross files. Ported programs that run: bash, ncurses, nano, Vim, Lua, SQLite, curl, FFmpeg (inside Videos) and a statically linked Ladybird with a custom window, toolbar and audio backend. Ladybird has no JIT, no WebGL, one tab, HTTP/1.1 or HTTP/2 only and its SQL storage disabled. The commits, build.ps1 and ROADMAP say it builds and runs in QEMU, but no automated test covers it, and its source revision is not pinned in the repo. About 10 third-party programs run in total. Packaging (`.tapp`) is per-user, unsigned and offline only. There is no repository, and ported programs are baked into the RAM disk at build time.

### Cited Findings
- Image chain:
  - `toolchain/Dockerfile` FROM myos-buildenv ([toolchain/Dockerfile:9](toolchain/Dockerfile))
  - `libs` FROM myos-toolchain ([toolchain/libs/Dockerfile:5](toolchain/libs/Dockerfile))
  - `browser` FROM myos-libs ([toolchain/browser/Dockerfile:7](toolchain/browser/Dockerfile))
  - `skia` FROM myos-browser ([toolchain/skia/Dockerfile:6](toolchain/skia/Dockerfile))
  - `ladybird` FROM myos-skia ([toolchain/ladybird/Dockerfile:5](toolchain/ladybird/Dockerfile))
  - The first toolchain build takes "about an hour" ([docs/TOOLCHAIN.md:18-21](docs/TOOLCHAIN.md)), and Ladybird "hours the first time" ([build.ps1:26-27](build.ps1)).
- Upstream sources are pinned with `ADD --checksum=sha256`:
  - binutils 2.45, GCC 15.2.0, mlibc v7.0.0, OpenSSL 3.5.9, curl 8.23.0 and the Mozilla CA bundle of 2026-09-25 ([toolchain/Dockerfile:25-29](toolchain/Dockerfile), [Dockerfile:84-88](toolchain/Dockerfile))
  - zlib 1.3.2, libpng 1.6.59, libjpeg-turbo 3.2.0, FreeType 2.15.0, HarfBuzz 14.6.0, nghttp2 1.70.0 and ICU 78.3 ([toolchain/libs/Dockerfile:32-44](toolchain/libs/Dockerfile))
  - For the browser: brotli 1.2.0, woff2 1.0.2, libwebp 1.6.0, dav1d 1.5.4 and more. "All static". mlibc is rebuilt first "with its Linux option: memfd_create, madvise, eventfd". ([toolchain/browser/Dockerfile:1-30](toolchain/browser/Dockerfile))
- Cross files: CMake `toolchain/cmake/TerminalOS.cmake` and `Platform/TerminalOS.cmake`; Meson `toolchain/cross-file.txt` and `libs/meson-cross.txt`; and `/tc/config.sub` knows `terminalos`. — [docs/TOOLCHAIN.md:168-182](docs/TOOLCHAIN.md), [TOOLCHAIN.md:81-91](docs/TOOLCHAIN.md)
- Ports framework:
  - Each recipe is `toolchain/ports/NAME/port.sh`: URL, SHA256 checked, `build()`, and `NEEDS` for dependencies. It is rebuilt only when the recipe changes. "One that cannot be downloaded is left out with a warning". — [toolchain/ports/build.sh:1-20](toolchain/ports/build.sh)
  - Recipes: bash 5.2.37, Lua 5.4.7, nano 8.2, ncurses 6.5, SQLite 3.47.2 and Vim 9.1.0000. Vim 9.1.0000 is the first 9.1 tag, from January 2024. — [toolchain/ports/*/port.sh](toolchain/ports)
  - System test `ports`: "Lua, SQLite, nano, Vim and bash run, dynamically linked". — [tests/qemu/run_tests.py:609](tests/qemu/run_tests.py)
- FFmpeg 7.1 (LGPL) is decoders-only (H.264, HEVC, VP8/9, AAC, MP3, Opus, Vorbis, FLAC and others; `--disable-network`, no hwaccel). It links against the **native** libc (`--extra-libs=$ROOT/build/user/libc.a -lgcc`), not mlibc. — [buildenv/Dockerfile:19-22](buildenv/Dockerfile); [scripts/build-ffmpeg.sh:19-32](scripts/build-ffmpeg.sh)
- Ladybird port:
  - **Build:** `build.sh` runs `patch.py`, copies `overlay/`, then CMake with `-DTERMINALOS=ON -DBUILD_SHARED_LIBS=OFF -DENABLE_JS_JIT=OFF -DENABLE_CRANELIFT_JIT=OFF -DBUILD_TESTING=OFF`. It links with `--allow-multiple-definition` because "two Ladybird libraries each carry a copy of a Rust crate". — [toolchain/ladybird/build.sh:7-43](toolchain/ladybird/build.sh)
  - **Source not pinned:** the source is a git checkout in a Docker volume (`ladybird-src`, "see README.md"), but no README.md exists in `toolchain/ladybird/` and no commit hash is pinned anywhere. Skia is pinned to "the commit Ladybird's vcpkg uses". — [build.sh:3-6](toolchain/ladybird/build.sh); `git ls-files toolchain/ladybird`; [toolchain/skia/Dockerfile:1](toolchain/skia/Dockerfile)
  - **Patches:** `patch.py` (274 lines) makes exact-match text edits to upstream:
    - It adds `AK_OS_TERMINALOS` and drops libedit, fontconfig and SDL3. [patch.py:23-49](toolchain/ladybird/patch.py)
    - It disables HTTP/3 (Alt-Svc) because "No HTTP/3 in Terminal OS's curl". [patch.py:96-101](toolchain/ladybird/patch.py)
    - It adds the TerminalOS audio backend. [patch.py:118-122](toolchain/ladybird/patch.py)
    - It sets `disable_sql_database = true`. [patch.py:139-145](toolchain/ladybird/patch.py)
    - It builds the Rust crates for `x86_64-unknown-linux-gnu`, because "the OS's C library answers the calls they make". [patch.py:244-249](toolchain/ladybird/patch.py)
    - It sends a Linux user agent. [patch.py:227-236](toolchain/ladybird/patch.py)
  - **WebGL off:** only ANGLE headers are present, and every EGL/GLES call goes to a stub that fails, so "pages get no WebGL". The headers are fetched with curl and no checksum. — [toolchain/ladybird/Dockerfile:24-38](toolchain/ladybird/Dockerfile), [Dockerfile:57-60](toolchain/ladybird/Dockerfile)
  - **Rust:** Rust 1.98.0 is installed via `curl https://sh.rustup.rs | sh`. — [toolchain/ladybird/Dockerfile:9-11](toolchain/ladybird/Dockerfile)
  - **Overlay UI:**
    - It is one binary for the browser and its helper processes (selected by argv[0]). [overlay/UI/TerminalOS/main.cpp](toolchain/ladybird/overlay/UI/TerminalOS/main.cpp)
    - The toolbar is drawn with Skia (back, forward, reload, downloads, address), on a single tab (`initialize_tab` once). It has no tabs, bookmarks or history UI. [overlay/UI/TerminalOS/WebViewTOS.cpp:1-8](toolchain/ladybird/overlay/UI/TerminalOS/WebViewTOS.cpp), [WebViewTOS.cpp:51](toolchain/ladybird/overlay/UI/TerminalOS/WebViewTOS.cpp), [WebViewTOS.cpp:181-256](toolchain/ladybird/overlay/UI/TerminalOS/WebViewTOS.cpp)
    - Audio goes through `SYS_SOUND`. [overlay/Services/AudioServer/PlaybackStreamTerminalOS.cpp:1-10](toolchain/ladybird/overlay/Services/AudioServer/PlaybackStreamTerminalOS.cpp)
    - It reports no gamepads. [GamepadManagerNone.cpp:1-6](toolchain/ladybird/overlay/UI/TerminalOS/GamepadManagerNone.cpp)
  - **Fonts:** Liberation and Noto Emoji ship with it. — [toolchain/ladybird/install.sh:7-17](toolchain/ladybird/install.sh)
  - **Size and launch:** the RAM disk grows by 224 MB with Ladybird. It needs about 2 GB of RAM ("give the machine 2 GB of memory or more for the browser"), and is started as `windowserver /ladybird/bin/Ladybird:https://example.com/`. — [Makefile:58-62](Makefile); [build.ps1:3-7](build.ps1), [build.ps1:25-33](build.ps1)
- Evidence that Ladybird runs:
  - Commits: `f96b92f` (2026-10-17, mlibc and Skia), `ce3e18c` (2026-10-18, "Implement Terminal OS support for Ladybird browser", 29 files, +738), `8bdbbce` (sound backend, UA, downloads) and `51f1058` (Dock icon, "Pointer" entry in DOCK.CFG, [Makefile:302](Makefile)). — `git log`
  - ROADMAP's kernel-lock work was "measured with `bklstat`... under the browser in QEMU". — [docs/ROADMAP.md:124-131](docs/ROADMAP.md)
  - No system test exercises Ladybird (the only toolchain-gated tests are `dynamic` and `ports`). — [tests/qemu/run_tests.py:1085-1087](tests/qemu/run_tests.py)
- Packaging: see section 10 (PACKAGES.md). Ports are built into the image rather than packaged, and there is "no package download on the OS itself". — [docs/TOOLCHAIN.md:62-70](docs/TOOLCHAIN.md); [docs/ROADMAP.md:107-108](docs/ROADMAP.md)

### Inferences
- Getting Ladybird (with Skia, Rust crates, ICU and curl) to build and render on a month-old mlibc port is the project's most impressive ecosystem result, and is comparable to what SerenityOS and Haiku spent years on. It is fragile, though:
  - The revision is unpinned.
  - Rust crates are built for linux-gnu and resolved against mlibc.
  - Many libraries carry a copy of the C runtime (static linking).
  - There is no regression test.
  - The renderer is CPU-only, with no JIT.
  Pinning the Ladybird commit and adding a smoke test (load a local page, screenshot) are cheap insurance.
- The count of third-party programs that actually run is small: about 10 (bash, nano, Vim, Lua/luac, sqlite3, curl, Ladybird, plus FFmpeg and minimp3/pl_mpeg/stb as embedded libraries). SerenityOS ships hundreds of ports and Haiku thousands. A repository and installer for ports is the main missing lever for C11.

### Gaps
- I could not confirm from the repo which Ladybird commit was last built, or which sites render (no logs or screenshots committed).

## 7. Developer experience: build, SDK, docs, onboarding

### Takeaway
Onboarding for app writers is good: one command (`.\build.ps1`), an app template (`scripts/new-app.py`), a worked example (`examples/hello`), a clear APPS.md, `.tapp` packaging and VS Code tasks. Real POSIX/C++ development needs the six-image chain, which takes hours, and the workflow is Windows-first (PowerShell build and USB-writing scripts; macOS "untested"). The native SDK forbids floating point in app code by policy, not necessity. There is no on-target debugger (no ptrace) and user-space debugging is printf plus crash reports. The 13 docs are unusually thorough but have drifted from the code in several places.

### Cited Findings
- Build entry points:
  - `build.ps1` (225 lines) builds in Docker, then boots QEMU, with flags `-NoRun -Clean -NewDisk -Gdb -Serial -Log -Usb -UsbOnly -Browser -NoBrowser -Package`. It writes USB sticks with checks (USB disks only, at most 64 GB, read-back). — [build.ps1:1-45](build.ps1)
  - Linux/macOS use `docker run ... make build-x86_64`, `make run`; "On Apple Silicon... This path is untested". — [docs/SETUP.md:76-98](docs/SETUP.md)
  - `make help`, `DEBUG=1`, `WERROR=1`, `CROSS=` and incremental `-MMD` builds. — [docs/DEVELOPING.md:15-30](docs/DEVELOPING.md); [Makefile:189-229](Makefile)
- App SDK:
  - `python scripts/new-app.py clock "Clock"` creates `apps/clock/` from a template. [scripts/new-app.py:1-25](scripts/new-app.py)
  - `examples/hello` (28 lines, views) is built with `.\build.ps1 -Package examples\hello`. [examples/hello/main.c:1-28](examples/hello/main.c); [docs/PACKAGES.md:61-72](docs/PACKAGES.md)
  - Apps need no Makefile edits (folder discovery, DOCK.CFG generated). [docs/APPS.md:1-5](docs/APPS.md), [APPS.md:39-40](docs/APPS.md)
- No floating point in apps: apps are built with `-mgeneral-regs-only` ([Makefile:69-71](Makefile)). Yet the Makefile allows SSE for decoder files because "the kernel saves every task's SSE registers" ([Makefile:327-328](Makefile)), so the ban is a convention. — [docs/APPS.md:196-203](docs/APPS.md)
- The native SDK and the full toolchain are separate. `.tapp` packages and apps compile against `user/include` and `user/lib`; C++ and POSIX ports need `myos-toolchain` and up. — [docs/PACKAGES.md:67-72](docs/PACKAGES.md); [docs/TOOLCHAIN.md:1-27](docs/TOOLCHAIN.md)
- Debugging:
  - Kernel debugging is good: a panic screen with registers, `addr2line`, QEMU `-d int`, a serial mirror, gdb with `hbreak`, the QEMU monitor. [docs/DEVELOPING.md:48-110](docs/DEVELOPING.md)
  - User programs get crash reports (instruction and address in the log, the "quit unexpectedly" window), plus logs saved to the USB stick (`logd`, `scripts/usb-logs.ps1`). [docs/DEVELOPING.md:76-88](docs/DEVELOPING.md)
  - There is no ptrace sysdep (absent from [sysdeps.hpp:7-118](toolchain/mlibc/terminalos/include/mlibc/sysdeps.hpp)), so no gdb or strace on the OS.
  - Ladybird crash backtraces are frame-pointer walks for `addr2line`. [toolchain/ladybird/patch.py:251-253](toolchain/ladybird/patch.py)
- Editor integration: `.vscode/tasks.json` (build in Docker, problem matcher), `.editorconfig`, `.gitattributes` (LF). — [docs/SETUP.md:70-74](docs/SETUP.md); [docs/DEVELOPING.md:151-152](docs/DEVELOPING.md)
- CONTRIBUTING.md (51 lines): start with an app, run tests before sending, one topic per PR. It says "No new dependencies without asking... (MIT, BSD, zlib, public domain)... No GPL code". — [CONTRIBUTING.md:1-51](CONTRIBUTING.md)
- Documentation drift, with examples:
  - USERSPACE.md's "no pipes/exec/float, 256 KiB stack, 14 programs, kernel never preempted" (section 1 above).
  - The readme still describes "a 40 MiB FAT32 RAM disk that GRUB loads" and "Shell ~30 commands", while the Makefile sets `RAMDISK_MB ?= 112` (+224 with Ladybird), TOOLCHAIN.md says "the RAM disk is 96 MB", and `help` lists about 70 commands. — [readme.md:52](readme.md), [readme.md:93-99](readme.md); [Makefile:55](Makefile); [docs/TOOLCHAIN.md:184-185](docs/TOOLCHAIN.md)
  - DEVELOPING.md still explains applying `0001-source-fixes.patch`, which is not in the repo, and gives a PIC/IRQ-stub driver recipe from before MSI. — [docs/DEVELOPING.md:128-181](docs/DEVELOPING.md)
  - SHELL.md says `tree` is "one level, no recursion yet", while the `help` text says "a folder and everything in it". — [docs/SHELL.md:72](docs/SHELL.md); [src/impl/x86_64/sys/shell.c:203](src/impl/x86_64/sys/shell.c)
  - APPS.md says MOVE events arrive "while over the window", but the server sends them only during a left-button drag (section 3).
  - SETUP.md does not mention the toolchain image chain at all; that is only in TOOLCHAIN.md and build.ps1.
- Generated binaries are committed (`targets/x86_64/ramdisk/UIFONT.BIN` 4.4 MB, `ICONS.BIN` 3.9 MB, `ax101.fw` 1.5 MB, `src/intf/drivers/gpu_shaders.h` 3,929 lines), and contributors must regenerate them ("commit ICONS.BIN with it"). — `git ls-files` sizes; [docs/APPS.md:213-222](docs/APPS.md)

### Inferences
- C12 is about 4 out of 10. For a newcomer writing a GUI app this is better than most hobby OSes (template, example, packaging, a test for apps). It falls short of SerenityOS/Haiku/Redox on POSIX completeness of the default SDK, on-target debugging, cross-platform host support, and reproducible, quickly obtainable toolchains (prebuilt toolchain images or a downloadable SDK would remove the hour-long first build).

### Gaps
- None of the build paths were run (read-only audit), so build times, warning counts and whether `make WERROR=1` passes are unknown.

## 8. Testing, CI and quality engineering

### Takeaway
There is a meaningful QEMU system-test suite: 25 tests that boot real images and drive them over the QEMU monitor and serial, including GUI tests that take screenshots. There are 4 host unit tests (network, TCP, WPA2, ext2/ext3 with e2fsck), three of them under ASan+UBSan, plus an `attack` program that checks security refusals. But CI only builds the ISO: no unit tests, no system tests, no `WERROR`. There is no fuzzing, static analysis or coverage, no unit tests for any user-space library (libc, ui/view, tapp, shell, window server), and builds are not reproducible.

### Cited Findings
- The system tests (`tests/qemu/run_tests.py`, 1,134 lines) boot their own QEMU (with OVMF), type commands and wait for expected serial text ("nothing depends on fixed delays"), saving logs and screenshots to `build/test/qemu/`. — [tests/qemu/run_tests.py:1-8](tests/qemu/run_tests.py)
- The 25 tests, by area:
  - Base: boot, files, fault, panic, kernel (stages 1-6).
  - Desktop: desktop, polish, gui, apps, zoom.
  - System: logs, user, video, bundle (`.tapp`), storage (ext3, crash replay, e2fsck), sleep, dynamic, ports, install.
  - Hardware: sata, hub, tablet, usbnet.
  - Network and boot: network (HTTPS), bios.
  - `network` needs the internet. `dynamic` and `ports` are skipped without the toolchain build.
  - Sources: [run_tests.py:334-1076](tests/qemu/run_tests.py) (docstrings), [run_tests.py:1081-1087](tests/qemu/run_tests.py)
- The GUI tests check startup, a click reaching an app, the Setup Assistant flow, every app starting and quitting, and the maximise button. They do not check pixel output beyond saving screenshots. — [run_tests.py:876-1056](tests/qemu/run_tests.py); [docs/APPS.md:224-233](docs/APPS.md)
- The window server prints test hooks ("windowserver: click at", "zoom ... to") for the tests to match. — [user/programs/windowserver/main.c:3294](user/programs/windowserver/main.c), [main.c:1521](user/programs/windowserver/main.c)
- Host unit tests:
  - `test-net`, `test-tcp` and `test-ext2` are built with `-fsanitize=address,undefined`. `test-wpa` has no sanitizer. ASLR is disabled via `setarch -R`. — [Makefile:510-540](Makefile); [Makefile:4](Makefile)
  - Sizes: `tests/net_host_test.c` 402, `tcp_host_test.c` 303, `wpa_host_test.c` 192, `ext2_host_test.c` 313 lines.
  - A known issue: ASan dies under Docker Desktop. — [docs/KNOWN-ISSUES.md:20-22](docs/KNOWN-ISSUES.md)
- Security regression program: `attack` covers elf, sigreturn, tramp, port and sandbox, and each must print "refused". — [user/programs/attack.c:1-14](user/programs/attack.c)
- CI (`.github/workflows/build.yml`, 20 lines) runs on push and PR. It builds `myos-buildenv`, runs `make build-x86_64` and uploads `kernel.iso`. It runs no tests and does not build the mlibc toolchain, ports or Ladybird. — [.github/workflows/build.yml:1-20](.github/workflows/build.yml)
- The project itself says to fix compiler warnings, then "enable `make WERROR=1` in CI". It also says `buildenv` "runs `apt-get upgrade -y`, so images are not reproducible, and the base image is a third-party image". — [docs/KNOWN-ISSUES.md:132-135](docs/KNOWN-ISSUES.md); [buildenv/Dockerfile:1-4](buildenv/Dockerfile)
- More reproducibility gaps:
  - BearSSL is cloned with `--depth 1` and no pinned commit. [buildenv/Dockerfile:17](buildenv/Dockerfile)
  - Rust comes via rustup piped to sh. [toolchain/ladybird/Dockerfile:10](toolchain/ladybird/Dockerfile)
  - ANGLE headers are fetched without checksums. [toolchain/ladybird/Dockerfile:28-34](toolchain/ladybird/Dockerfile)
  - The Ladybird source is unpinned (section 6).
  - On the positive side, most toolchain and library sources are SHA-256 pinned (section 6).
- No fuzzing, static analysis (cppcheck, clang-tidy, scan-build, CodeQL), coverage, or sanitizer builds of user programs: a grep of the Makefile and workflows finds only the host-test sanitizers. — [Makefile:510-540](Makefile); [.github/workflows/build.yml](.github/workflows/build.yml)
- The commit messages describe manual hardware checks ("User test, first round...", `1e7c70e`). ROADMAP's working rule: each stage ends with "`make test` + `make test-os` green, a USB stick, one laptop check". — [docs/ROADMAP.md:150-154](docs/ROADMAP.md)

### Inferences
- C13 is about 3.5. The test design (event-driven serial matching, one VM per test, hardware emulation for USB hubs, tablets, SATA and CDC-ECM) is better than many hobby OSes. Because CI never runs it, regressions are caught only when the author runs about 5 minutes of tests locally. The cheapest big win is running `make test-unit` plus a TCG-only subset of `run_tests.py` in GitHub Actions. The next is fuzzing the parsers that face untrusted input: `.tapp` tar parsing, the PNG/JPEG decoders in `user/lib/image.c`, the ELF loader and the ws/port message handlers.

### Gaps
- Pass/fail history is unknown (no CI test runs, no committed test logs).

## 9. Project health: history, velocity, contributors, licensing, community

### Takeaway
This is effectively a one-person project. Its history has two bursts: a week of early commits in October 2025, then 12 months of silence, then about 119 commits in 31 days (8 Oct to 7 Nov 2026) that produced almost all of today's roughly 72k lines. The commits are very large and often written in an AI-assistant style. The licence is MIT and third-party code is mostly well documented, but the shipped image includes GPLv3 (bash, nano) and LGPL (FFmpeg, statically linked) binaries with no licence texts or source offer, which contradicts CONTRIBUTING's "No GPL code". There is no community yet: 3 stars, 0 forks, 0 issues, 1 closed PR, no releases or tags.

### Cited Findings
- Commits per month: 27 in 2025-10, **none from 2025-11 to 2026-09**, 104 in 2026-10, 15 in 2026-11 (to the 7th). — `git log --date=format:%Y-%m`
- Authors:
  - `BhishanSharma <bhishansharma2@gmail.com>`: 27 commits, 2025-10-02 to 2025-10-07.
  - `bhishan-genoma <bhishan.builder@gmail.com>`: 111 commits, 2026-10-08 to 2026-11-07.
  - `alan-w-arch <himanshu.12322132@lpu.in>`: 8 commits, 2026-10-13 to 2026-10-19 (loginwindow, window scaling, design tokens and themes; e.g. `6355ac6`, `b433b9a`, `1775904`). This is a different email domain and name, so it may be a second person. The local repo's configured git user is also alan-w-arch.
  - Sources: `git shortlog -sne`; `git log --author`
- Commit sizes (insertions plus deletions, 145 non-merge commits): median 432, p90 2,113, maximum 34,675 (`997d25c`, "Power: ACPI shutdown, power button and battery; native screen resolution", 81 files). Other big ones: `1df4579` 16,002, `a1c53a0` 8,921, `3124546` 5,686 ("NVMe, VFS, TCP sockets, MSI interrupts and SMP" in one commit). — `git log --shortstat`
- Some messages carry little information ("Implement code changes to enhance functionality and improve performance", `e0844ec`, 1,837 changed lines in one file). Others are bullet lists in a generated style (`ce3e18c`, `f96b92f`, `8bdbbce`). — `git show`
- The tracked text, excluding vendored code, firmware, RAM-disk binaries, trust anchors and generated shaders, is about 71.8k lines. — `git ls-files | xargs wc -l`
- There are no tags or releases. The only branch is `main`, mirrored on `origin/main`. — `git tag`, `git branch -a`
- Licence: MIT, "Copyright (c) 2026 Bhishan Sharma and the Terminal OS contributors". Third-party files "keep their own licences". — [LICENSE:1-24](LICENSE)
- Third-party code, all with licence files or headers:
  - uACPI: MIT, pinned to commit `2cb5e71e...`. [third_party/uacpi/VERSION](third_party/uacpi/VERSION), [third_party/uacpi/LICENSE](third_party/uacpi/LICENSE)
  - pl_mpeg: MIT, `c871f2be...`. [third_party/pl_mpeg/VERSION](third_party/pl_mpeg/VERSION), [pl_mpeg.h:3](third_party/pl_mpeg/pl_mpeg.h)
  - minimp3: CC0 / public domain. [third_party/minimp3/minimp3.h:5](third_party/minimp3/minimp3.h)
  - stb_image v2.30: public domain. [third_party/stb/stb_image.h:1](third_party/stb/stb_image.h)
  - Inter and JetBrains Mono: SIL OFL. [third_party/fonts/OFL-*.txt](third_party/fonts)
  - Fluent UI Emoji icons: MIT, Microsoft. [third_party/icons/LICENSE-fluentui-emoji.txt](third_party/icons/LICENSE-fluentui-emoji.txt)
  - BearSSL (MIT) is fetched at build time, and `trust_anchors.c` is generated (8,380 lines).
- Firmware: `ax101.fw` is Intel's `iwlwifi-so-a0-hr-b0-77.ucode`, unchanged, "redistributed under Intel's licence". The licence permits binary redistribution without modification, with the notice reproduced. — [firmware/README.md](firmware/README.md); [firmware/LICENCE.iwlwifi_firmware:1-12](firmware/LICENCE.iwlwifi_firmware)
- Licence compliance risks:
  - CONTRIBUTING says "No GPL code". [CONTRIBUTING.md:45-48](CONTRIBUTING.md)
  - Yet the image ships GNU bash and nano (GPLv3) and links FFmpeg 7.1 (LGPL 2.1) statically into Videos. [toolchain/ports/bash/port.sh:4](toolchain/ports/bash/port.sh), [toolchain/ports/nano/port.sh:3](toolchain/ports/nano/port.sh); [scripts/build-ffmpeg.sh:11](scripts/build-ffmpeg.sh), [build-ffmpeg.sh:26](scripts/build-ffmpeg.sh)
  - No licence texts or NOTICE files are copied onto the RAM disk. A grep for licen, copying or notice in the Makefile and ports scripts finds only the FFmpeg comment.
  - The image also carries Ladybird (BSD-2), OpenSSL (Apache-2.0), curl, ICU, Liberation/Noto fonts (OFL) and others. — [Makefile:293-305](Makefile)
- GitHub (github.com/BhishanSharma/os, fetched 7 Nov 2026): public; "No description, website, or topics provided"; 3 stars, 0 forks, 1 watcher; 146 commits (the same as local HEAD); 0 open issues; 0 open and 1 closed pull request; no releases shown. — [GitHub repo](https://github.com/BhishanSharma/os); [issues](https://github.com/BhishanSharma/os/issues); [pulls](https://github.com/BhishanSharma/os/pulls)
- Governance documents: CONTRIBUTING.md only. There is no CODE_OF_CONDUCT, SECURITY.md, issue or PR templates, CHANGELOG or MAINTAINERS file. — `git ls-files`

### Inferences
- Velocity is very high: about 2,300 lines a day sustained for a month, with whole subsystems landing per commit. That is not typical of solo hand-written code, and together with ROADMAP's "working rules" ([docs/ROADMAP.md:287-293](docs/ROADMAP.md)) it suggests heavy AI-assisted development. This raises review-depth and maintainability risk: very large commits, terse or generic messages, docs drifting from the code, and a 3.9k-line single-file window server. It also suggests the project could keep its pace if the method holds.
- Bus factor 1, no releases and no community put C17 around 1.5. Fixing licence compliance (shipping licence texts, a written source offer or source tarballs for GPL/LGPL parts, and updating CONTRIBUTING to say how ports differ from vendored code) is quick and should come before any public release.

### Gaps
- The closed pull request's title and author were not visible on GitHub. Whether alan-w-arch is a separate person cannot be determined from the repo.

## 10. The project's own plans, item by item (ROADMAP.md, KNOWN-ISSUES.md, PACKAGES.md)

### Takeaway
ROADMAP.md is a log of finished "lean" stages, each with explicit "Cuts", plus an older 10-stage sketch. Every numbered stage (0-10) is marked done in some form, and the open work is entirely in the listed cuts. KNOWN-ISSUES.md has 5 open problems, a long Fixed log, "Smaller things" and an "Ideas" list that is partly stale. PACKAGES.md describes a per-user, unsigned, sandboxed `.tapp` tar format with no repository, dependencies or updates.

### Cited Findings
**ROADMAP.md, done items and their remaining cuts**
- Stage 0 (`bda0bc7`): -O2, higher-half kernel, commands as programs, `make test`/`make test-os`. **Done.** — [docs/ROADMAP.md:3-4](docs/ROADMAP.md)
- Stages 1-6: demand paging, mmap, shm, COW fork/exec, 8 MiB stacks, threads/futex, sleeping locks, Mach-style ports and name service, pipes, dup2, signals, pipelines, device registry, launchd. **Done.** — [ROADMAP.md:6-13](docs/ROADMAP.md)
  - Open cuts: signals delivered only on syscall return; the big kernel lock covers most of the kernel.
  - Stale cuts: "no TLB shootdown" (fixed per [docs/KNOWN-ISSUES.md:35-41](docs/KNOWN-ISSUES.md)); "no TLS" (SYS_TCB_SET exists, [docs/TOOLCHAIN.md:191-192](docs/TOOLCHAIN.md)).
- Stage 7, WindowServer and UI: **done, lean.** — [ROADMAP.md:15-24](docs/ROADMAP.md)
  - Open: no free resizing (zoom only), no per-app menus.
  - Fixed later: clients no longer poll (futex, [ROADMAP.md:31-32](docs/ROADMAP.md)); key-reading programs now work in Terminal (pty, [ROADMAP.md:100-103](docs/ROADMAP.md)).
- Desktop polish 1 (AA fonts, round corners, shadows, translucent bar, damage list, PAT write-combining, futex wake): **done.** — [ROADMAP.md:26-32](docs/ROADMAP.md)
- Graphics (EDID native mode, Intel Gen12/13 hardware cursor and page flip, 1 kHz pointer, Fluent icons, graphical loginwindow): **done.** — [ROADMAP.md:34-41](docs/ROADMAP.md)
- Stage 8, bundles: **done as `.tapp`** in `~/Apps/ID`. — [ROADMAP.md:43-49](docs/ROADMAP.md)
  - Open: no system-wide /APPS.
  - The permissions cut was later partly covered by the sandbox.
- Stage 9, ext2/ext3 with journal, owners and modes, statx/chmod/chown/rename: **done.** — [ROADMAP.md:51-66](docs/ROADMAP.md)
  - Open: the installer still makes FAT32; no symlinks or hard links created; one group per user; a file deleted while open reads stale blocks.
  - Possibly stale: "FAT32... cannot rename" (commit `1e7c70e` mentions "FAT32 rename"); "made-up modes" (fixed in sysdeps.cpp:113-124).
- Sleep and wake (S0ix-style): **done.** Open: no ACPI S3, no D3, no touchpad wake. — [ROADMAP.md:68-78](docs/ROADMAP.md)
- Stage 10, sandbox: **done, lean.** Open: ports and shm not limited; built-in apps unsandboxed; no settings UI for it. — [ROADMAP.md:80-85](docs/ROADMAP.md)
- Dynamic linking: **done, lean.** Open: static C++ runtime, no PIE, Ladybird static, no page sharing. — [ROADMAP.md:87-96](docs/ROADMAP.md)
- Ports system and pty/termios and job control: **done.** Open: no SIGTTIN; "no package download on the OS itself". — [ROADMAP.md:98-108](docs/ROADMAP.md)
- Desktop polish 2 (clipboard, notifications, UTF-8 with 938 characters, Calculator, Calendar, TextEdit, lock screen, screenshots, file search, accessibility): **done.** Open: no CJK/RTL, rich text, drag and drop, or per-app notification settings. — [ROADMAP.md:110-122](docs/ROADMAP.md)
- Big kernel lock, first steps (40% fewer acquisitions, about 14% of time spent waiting): **in progress.** Next: separate scheduler and allocator locks. — [ROADMAP.md:124-136](docs/ROADMAP.md)
- Hardware breadth (AHCI, USB hubs, CDC-ECM, HID tablets, HWP/MWAIT C6): **done.** Open: port multipliers, CD/DVD, 4Kn, AMD CPPC, tickless idle. — [ROADMAP.md:138-148](docs/ROADMAP.md)
- The stage table (1-10 with S/M/L/XL sizes) and the per-stage sketches (lines 169-285) are historical design notes. Stage 10's sketch still lists "ACPI S3 sleep" and "a settings app (Wi-Fi, theme, users)"; the users part is not done. — [ROADMAP.md:156-167](docs/ROADMAP.md), [ROADMAP.md:281-285](docs/ROADMAP.md)
- The working rules say "Read this file and the stage's files only; no repo-wide reviews... commit when the person says so". — [ROADMAP.md:287-293](docs/ROADMAP.md)

**KNOWN-ISSUES.md**
- Open problems:
  1. The C compiler is a stub. Still true. — [docs/KNOWN-ISSUES.md:8-9](docs/KNOWN-ISSUES.md)
  2. FAT32: ASCII long names, `ls` at most 128 entries, `tree` 32, `cat` refuses files over 1 MB, no rename, no owners. Partly stale: USERSPACE.md says `cat` shows "a text file of any size", and commit `1e7c70e` adds FAT32 rename. — [KNOWN-ISSUES.md:10-12](docs/KNOWN-ISSUES.md); [docs/USERSPACE.md:98](docs/USERSPACE.md)
  3. System calls dereference user pointers directly; SMAP is open for the whole syscall window. — [KNOWN-ISSUES.md:13-16](docs/KNOWN-ISSUES.md)
  4. ext3 delete-while-open; whole-file writes at close, so the largest file is 4 GB. — [KNOWN-ISSUES.md:17-19](docs/KNOWN-ISSUES.md)
  5. AddressSanitizer in host tests dies under Docker Desktop ASLR, which needs `setarch -R` and seccomp unconfined. — [KNOWN-ISSUES.md:20-22](docs/KNOWN-ISSUES.md)
- Fixed log: SMAP and TLB shootdown, a syscall buffer unmapped mid-call, networking (DHCP/DNS/TCP/HTTPS), the RTL8139 ring, `help` drift, the kernel image overflowing its mapping, exception handlers, and `0001-source-fixes.patch` items (IDT out-of-bounds write, PIC cascade, stack alignment, red zone). — [KNOWN-ISSUES.md:24-120](docs/KNOWN-ISSUES.md)
- "Smaller things", all apparently still open: unused vars in `pic_remap`; the "Terminmal OS" splash typo; ATA polling with no timeouts; `isr_timer` taking a struct by value; RWX LOAD segment (no kernel W^X); compiler warnings, with "Fix them, then enable `make WERROR=1` in CI"; `buildenv` running `apt-get upgrade` on a third-party base image, so it is "not reproducible". — [KNOWN-ISSUES.md:121-135](docs/KNOWN-ISSUES.md)
- "Ideas", mostly stale: memory size from Multiboot2; a table-driven shell (still open); NIC TX/ARP/UDP/TCP (done); a real compiler (open); ring 3, syscalls and an ELF loader (done). — [KNOWN-ISSUES.md:137-143](docs/KNOWN-ISSUES.md)

**PACKAGES.md (`.tapp`)**
- Install by double-clicking in Files or with `tapp install/list/remove`. Apps go to the user's own `~/Apps/ID/` with no admin, and the same id replaces (updates) the old one. **Done.** — [docs/PACKAGES.md:8-25](docs/PACKAGES.md)
- "Packages are not signed yet". An installed app "runs as you". **Open: signing.** — [PACKAGES.md:27-29](docs/PACKAGES.md)
- `app.cfg` keys: id (1-8 characters), name, version, author, color, icon, `files =` (up to 5 home folders) and `network = yes`. — [PACKAGES.md:43-52](docs/PACKAGES.md)
- Sandbox: the app's own folder plus the named folders; the network only if declared; system files readable; inherited; cannot be widened; built-in apps unsandboxed. **Done, lean.** — [PACKAGES.md:54-59](docs/PACKAGES.md), [PACKAGES.md:100-104](docs/PACKAGES.md)
- Build with `.\build.ps1 -Package DIR` or `make tapp SRC=...` against `user/include` and `user/lib` (native SDK only). — [PACKAGES.md:61-72](docs/PACKAGES.md)
- Format: a flat ustar tar (no folders or links), at most 64 MB, validated before install (tar, id, ELF, no path escape). — [PACKAGES.md:74-89](docs/PACKAGES.md); [user/programs/tapp.c:20-21](user/programs/tapp.c)
- Not designed at all (absent from the doc): a repository or index, download on the OS, dependencies, version comparison or rollback, delta updates, system-wide installs, package signing keys, or packaging for the ported mlibc programs (those are baked into the RAM disk at build time, [docs/TOOLCHAIN.md:62-70](docs/TOOLCHAIN.md)).

### Inferences
- The project plans in short, single-session "lean stages" and writes the cuts down honestly. The cuts lists are effectively the roadmap's backlog, and the final roadmap should be built from them.
- The working rules ("no repo-wide reviews", "commit when the person says so") read like instructions to an AI coding assistant. Together with the very large, multi-subsystem commits (section 9), this suggests most code is written with AI help under one person's direction. This is an inference, not stated explicitly.

### Gaps
- None of the "open" items were checked at run time. Status is taken from the docs plus code reading.

## 11. Preliminary scores (0-10, shared scale) and prioritised gaps

### Takeaway
Preliminary scores: C10 Graphics/windowing/desktop UX **4**; C11 Applications and ecosystem **3**; C12 Developer experience and SDK **4**; C13 Reliability/testing/QE **3.5**; C15 Accessibility and i18n **1.5**; C16 Installation/updates/recovery/administration **2.5** (user-space view); C17 Community/governance/licensing/sustainability **1.5**. The user-space stack is a polished early hobby OS: very good looks and an unusual port (Ladybird), with thin breadth, no CI testing, near-zero i18n and accessibility, and a one-person community.

### Cited Findings (justifications; evidence is in sections 1-10)
- **C10 = 4.**
  - For: compositor with damage, vsync and multi-core painting, and GPU blur and shadows on Intel ([user/programs/windowserver/main.c:1-30](user/programs/windowserver/main.c), [main.c:3699-3716](user/programs/windowserver/main.c), [main.c:3866-3871](user/programs/windowserver/main.c)); a design-token theming system with dark mode, contrast and accent ([design/tokens.json](design/tokens.json), [user/include/ui.h:15-49](user/include/ui.h)); clipboard, notifications, lock screen, screenshots and Launcher search.
  - Against: 16 windows at most, zoom-only resizing, no Alt+Tab, snapping or virtual desktops, no per-app menus, no drag and drop, text-only clipboard, left-button-only events, one monitor, integer 1x/2x scaling, and a small widget set ([main.c:39](user/programs/windowserver/main.c), [main.c:1498-1525](user/programs/windowserver/main.c), [main.c:3391](user/programs/windowserver/main.c), [user/include/view.h:31](user/include/view.h); [docs/ROADMAP.md:23](docs/ROADMAP.md), [ROADMAP.md:121-122](docs/ROADMAP.md)).
  - Comparison: below SerenityOS and Haiku (5-6), above most hobby OSes.
- **C11 = 3.**
  - For: 11 coherent native apps (about 4.9k lines); ports of bash, Vim, nano, Lua, SQLite and curl; FFmpeg-backed video; Ladybird builds and runs in QEMU ([docs/TOOLCHAIN.md:72-79](docs/TOOLCHAIN.md); [docs/ROADMAP.md:124-131](docs/ROADMAP.md)).
  - Against: about 10 third-party programs in total; Files cannot copy, move, rename or delete; no document, PDF or archive apps; no store, repository or on-OS download ([docs/ROADMAP.md:107-108](docs/ROADMAP.md); [docs/PACKAGES.md:27-29](docs/PACKAGES.md)).
  - Comparison: SerenityOS, Haiku and Redox have hundreds to thousands of ports and broader native suites.
- **C12 = 4.**
  - For: a one-command build, app template and example, `.tapp` packaging, thorough docs, GCC 15/mlibc/C++23 with CMake/Meson cross files ([build.ps1:1-45](build.ps1); [scripts/new-app.py:1-25](scripts/new-app.py); [docs/TOOLCHAIN.md:1-27](docs/TOOLCHAIN.md)).
  - Against: a non-POSIX default SDK with no floating point in apps ([docs/APPS.md:196-203](docs/APPS.md)); the full toolchain takes hours to build; Windows-first scripts; no on-target debugger; visible doc drift ([docs/USERSPACE.md:145-190](docs/USERSPACE.md)).
- **C13 = 3.5.**
  - For: 25 QEMU system tests including GUI, USB and storage crash-replay; 4 host unit tests (3 under ASan/UBSan); a security `attack` regression ([tests/qemu/run_tests.py:1081-1087](tests/qemu/run_tests.py); [Makefile:510-540](Makefile); [user/programs/attack.c:1-14](user/programs/attack.c)).
  - Against: CI only builds the ISO ([.github/workflows/build.yml:1-20](.github/workflows/build.yml)); no fuzzing, static analysis or coverage; no tests for user-space libraries; non-reproducible base image ([docs/KNOWN-ISSUES.md:132-135](docs/KNOWN-ISSUES.md)).
- **C15 = 1.5.**
  - For: Increase Contrast, Reduce Motion, Reduce Transparency, 2x Larger Text and Tab focus ([apps/settings/main.c:389-393](apps/settings/main.c)).
  - Against: no screen reader or accessibility tree; US keymap only ([src/impl/x86_64/drivers/keyboard.c:41](src/impl/x86_64/drivers/keyboard.c)); time zone hard-coded to IST ([src/intf/drivers/rtc.h:30-31](src/intf/drivers/rtc.h)); no locales or translations; 938 pre-rendered glyphs and no CJK/RTL ([docs/ROADMAP.md:113-122](docs/ROADMAP.md)).
- **C16 = 2.5 (user-space view; installer and boot details are another researcher's).**
  - For: `install` to the SSD with a boot entry; the USB stick keeps files and accounts across re-imaging; a first-start Setup Assistant; logs saved to the stick; the kernel shell as fallback; safe mode ([readme.md:110-122](readme.md); [user/programs/loginwindow.c:14-15](user/programs/loginwindow.c); [docs/DEVELOPING.md:76-88](docs/DEVELOPING.md); [docs/USERSPACE.md:74-75](docs/USERSPACE.md)).
  - Against: no OS update mechanism beyond rewriting the stick; no package repository or updates; users managed only through CLI (`useradd`, `passwd`, `su`) with no Settings pane; no backup or recovery environment ([docs/SHELL.md:38-47](docs/SHELL.md); [apps/settings/main.c:18-22](apps/settings/main.c)).
- **C17 = 1.5.**
  - For: MIT licence; a CONTRIBUTING guide; vendored code licensed and documented ([LICENSE](LICENSE); [CONTRIBUTING.md](CONTRIBUTING.md)).
  - Against: effectively one developer; a 12-month gap then a one-month burst; no releases or tags; 3 stars, 0 forks, 0 issues ([GitHub](https://github.com/BhishanSharma/os)); GPL/LGPL binaries shipped without licence texts or source offer, contrary to CONTRIBUTING ([CONTRIBUTING.md:45-48](CONTRIBUTING.md); [scripts/build-ffmpeg.sh:11](scripts/build-ffmpeg.sh)).

**Top gaps, in priority order (rough effort for one experienced developer; observed AI-assisted velocity may shorten these)**

| # | Gap | Fix | Effort (person-weeks) | Categories |
|---|-----|-----|-----------------------|------------|
| 1 | CI runs nothing but a build | Run `make test-unit`, a TCG subset of `run_tests.py`, and `WERROR=1` in GitHub Actions. Pin the base image, BearSSL commit, Ladybird commit (add the missing README) and ANGLE checksums. | 2-3 | C13, C12 |
| 2 | Licence compliance | Ship licence texts and NOTICE on the image; give a source offer or tarballs for bash, nano and FFmpeg (LGPL relinking); make CONTRIBUTING's "No GPL" match practice. | 0.5-1 | C17 |
| 3 | Window-management basics | Edge and corner resize, Alt+Tab, right/middle/hover/focus events, more than 16 windows, context menus, a per-app menu bar, access control on `ws.<id>` surfaces. | 4-6 | C10, C5 |
| 4 | Split SDK | One SDK: port `ui`/`view` to mlibc and C++, allow floating point in apps (the kernel already saves SSE state, [Makefile:327-328](Makefile)), build `.tapp` with the full toolchain, publish a prebuilt toolchain image or SDK. | 4-6 | C12, C11 |
| 5 | POSIX gaps (kernel plus mlibc) | setuid/setgid, symlinks and hard links, utimensat, statvfs, real O_RDWR in-place writes, alarm/setitimer/timerfd, epoll/eventfd, UDP sockets, ptrace. | 8-12 | C11, C12 |
| 6 | Toolkit completeness | Checkbox, radio, popup/menu, scroll view, multi-line text, tabs, file open/save dialogs, MIME-typed clipboard, drag and drop. | 8-12 | C10, C11 |
| 7 | Package manager v1 | Signed index over HTTPS, dependencies, system and per-user installs, ported programs as packages, an OS update channel. | 6-10 | C11, C16 |
| 8 | Userland breadth | BusyBox/toybox or GNU sed, awk, find, diff, less, tar, gzip and xargs, plus man pages (after #5). | 2-4 | C11 |
| 9 | Files operations | Copy, move, rename, delete, trash, new folder; an "Open with" file-type registry; drag and drop to apps. | 3-5 | C11, C10 |
| 10 | Internationalisation | Runtime FreeType/HarfBuzz text with Noto fallback and fractional sizes; ICU bidi and line breaking (CJK/RTL); keyboard layouts with dead keys and compose; tzdata time zones and locales; Settings panes. IME is a further 4-8. | 10-16 | C15 |
| 11 | Accessibility | Accessibility tree on `view_t`, a screen reader (for example espeak-ng), zoom or magnifier, sticky and slow keys, a keyboard-navigation audit. | 8-12 | C15 |
| 12 | Fuzzing and static analysis | Fuzz the `.tapp` parser, image decoders, ELF loader and ws/port messages; cppcheck or clang-tidy in CI; host unit tests for `user/lib`. | 2-4 | C13, C5 |
| 13 | Multi-monitor and fractional scaling in the window server | Needs kernel display enumeration too. | 4-8 | C10 |
| 14 | Documentation refresh | USERSPACE, readme RAM-disk and command counts, DEVELOPING patch and driver sections, SHELL `tree`, APPS event semantics. | about 1 | C12 |
| 15 | Project hygiene | Tagged releases with changelog, issue and PR templates, CoC, SECURITY.md, smaller reviewable commits. | about 1, then ongoing | C17 |
| 16 | Native app breadth | PDF/document viewer, archive manager, Music library, Calendar ICS/recurrence, Paint undo, shapes and PNG. | 10-20 | C11 |

### Inferences
- Items 1, 2 and 14 are cheap and protect what already exists, so they should come first. Items 4 and 5 unblock nearly everything in C11 and C12. Items 3 and 6 are the visible UX gap against SerenityOS and Haiku. Items 10 and 11 are the largest single deficit (C15) and need the most new design.
- The total for these gaps is about 75-125 person-weeks at conventional pace.

### Gaps
- The scores are preliminary and come from code and docs only. Nothing was run. C16 especially depends on the boot, storage and installer research covered by other researchers.
