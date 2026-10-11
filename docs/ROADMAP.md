# Roadmap: a macOS-like architecture

Stage 0 is done (bda0bc7): `-O2`, higher-half kernel (programs own the lower
128 TiB), everyday commands as programs, `make test` / `make test-os`.

Stages 1-6 are done: demand paging, `mmap`, shared memory, copy-on-write `fork`
and `exec`, 8 MiB growable stacks; threads, futex mutexes, sleeping kernel
locks (`vfs_lock`); Mach-style ports with a name service; pipes, `dup2`,
signals and shell pipelines/redirection; the device registry (`ioreg`);
`launchd` starting `login` and keepalive services from `LAUNCHD.CFG`.
System test: `kernel`. Cuts left: no TLB shootdown across a program's threads
on other cores, signals delivered only on return from a system call, no TLS,
and the big kernel lock still covers most of the kernel.

Stage 7 is done, lean: `windowserver` (or `desktop`) composites shared-memory
windows, redrawing only damaged rectangles, with a menu bar (the Terminal OS
menu, the app's name, a clock), a Dock (Terminal, Files, About), and windows
that can be dragged, closed and minimised; Ctrl+Q leaves. `user/lib/ui.c` has
the drawing and widgets (button, text field, list). Terminal runs `shell -t`
over pipes (Ctrl+C interrupts its programs); Files browses folders and opens
pictures in `view`, videos in `video` (both draw in a window on the desktop)
and other files with `cat` in a Terminal. System test: `desktop`. Cuts: no
resizing, no per-app menus, clients poll for events (no cross-process futex),
and programs that read keys directly do not work in Terminal.

Desktop polish (after stage 7): anti-aliased Inter and JetBrains Mono text
pre-rendered into `UIFONT.BIN` (`scripts/gen-uifont.py`, fonts under SIL OFL
in `third_party/fonts`), anti-aliased round corners, soft shadows, a
translucent menu bar and Dock (which magnifies under the pointer), an
anti-aliased pointer, the whole screen (no kernel status bar), damage as a
list of rectangles, the framebuffer mapped write-combining (PAT), and apps
woken by a futex in the shared surface instead of polling.

Graphics and a graphical system (after that): the loader picks the screen's
native mode (EDID); on Intel Gen12/13 graphics (`igfx.c`) the pointer is the
hardware cursor and frames are page-flipped at the screen's refresh, which
also paces the desktop; a 1 kHz timer path moves the pointer with
acceleration; icons are Microsoft's Fluent UI Emoji (MIT, `ICONS.BIN` from
`scripts/gen-icons.py`). The console service is `loginwindow`, a graphical
login that starts the desktop as the user; Log Out returns to it, and the
user name `>console` gives the text login (the tests use it).

Each stage below is the *lean* version: the smallest design that gives the
macOS-style structure, with the cuts written down. One stage per working
session; it ends with `make test` + `make test-os` green, a USB stick, one
laptop check, and a commit. Every stage adds its own system test to
`tests/qemu/run_tests.py`.

| # | Stage | Depends on | Size |
|---|-------|-----------|------|
| 1 | Virtual memory | 0 | M |
| 2 | Threads, sleeping locks | 1 | M |
| 3 | Ports (Mach IPC) | 1, 2 | M |
| 4 | BSD layer: fds, pipes, signals, fork | 1, 3 | L |
| 5 | Device registry (IOKit-lite) | — | S |
| 6 | launchd | 3, 4 | S |
| 7 | WindowServer, UI library, Dock, Terminal, Finder | 1, 3, 6 | XL |
| 8 | App bundles and launcher | 7 | S |
| 9 | File system with permissions | 4 | L (optional) |
| 10 | Sandbox, sleep/wake, settings | 8 | M |

## 1. Virtual memory

* Per-process region list (`sys/vm.c`, `struct vm_region {start, end, prot,
  kind: ANON|FILE|SHARED, object, offset}`); `process.c` keeps one per task.
* Page-fault handler (`exceptions.c` -> `vm_fault`): a user fault inside a
  region maps a page (zeroed, or read from the file) and returns; outside
  every region it is a segmentation fault as today.
* Heap (`sbrk`) and stack become lazy regions; stack region 8 MiB, guard page
  below it ("stack overflow" message).
* `mmap(addr, len, prot, flags, fd, off)` / `munmap` / `mprotect`: anonymous,
  private file mappings (read on fault), and `MAP_SHARED` of a named shared
  memory object: `shm_open(name, size)` -> fd (`sys/shm.c`, refcounted
  physical pages).
* Copy-on-write bit in the PTE (software bit 9) and the fault path for it,
  used by fork in stage 4.
* Cut: no swap, no page cache for files (each mapping reads its own pages).
* Test `vm`: a program touches 512 MiB sparse (RSS stays low), maps a file,
  shares memory with a child, overflows its stack.

## 2. Threads and sleeping locks

* `task` = thread; `process_t` shared by threads (refcount). `thread_create(fn,
  arg, stack)`, `thread_exit`, `thread_join`; FS base register for TLS.
* `futex_wait(addr, val)` / `futex_wake(addr, n)`; user `mutex`/`cond` in
  `user/lib/thread.c`.
* Kernel `mutex_t` that sleeps (task blocks, BKL released). The FAT32/VFS
  layer gets one; NVMe goes back to waiting with `cpu_wait` (undo the stage-0
  spin). Network stack and sound get one each.
* Cut: the big kernel lock stays for everything else; per-CPU run queues
  later. That is the honest lean version: waits no longer hold the BKL.
* Test `threads`: 4 threads add to a counter under a mutex; result exact.

## 3. Ports (Mach-style IPC)

* `sys/port.c`: a port = kernel message queue. Each process has a name table
  (small integers) of rights: RECEIVE (one owner), SEND, SEND_ONCE.
* `port_create()`, `port_send(name, msg, len, reply_name)`,
  `port_receive(name, buf, max, timeout) -> sender reply right`, rights
  passed in messages; messages up to 4 KiB inline, larger ones as a shared
  memory object (stage 1).
* Bootstrap: `port_register(name)` / `port_lookup(name)` (a kernel name
  service; launchd takes it over in stage 6).
* User library `user/lib/ipc.c` with a request/reply helper.
* Test `ports`: an echo server program, 1000 round trips, a 1 MiB message.

## 4. BSD layer

* One fd table per process: files, pipes, sockets, console, ports. `dup2`,
  `pipe`, sockets become fds (keep the old calls as wrappers).
* Shell: `a | b`, `> file`, `< file`, `2>&1`.
* Signals: SIGINT (Ctrl+C), SIGTERM, SIGKILL, SIGCHLD, SIGSEGV; `signal()`
  handlers run on the user stack; `kill(pid, sig)`.
* `fork` with copy-on-write (stage 1), `exec` (replace image), `waitpid`.
* Cut: no job control (process groups) beyond today's fg/bg; permissions stay
  the home-folder rule until stage 9.
* Test `pipes`: `ls | wc`, `echo hi > f; cat < f`, fork/exec/wait, Ctrl+C.

## 5. Device registry (IOKit-lite)

* `drivers/registry.c`: a tree of nodes {name, class, parent, properties,
  driver}. PCI scan creates device nodes; drivers declare a match table
  (vendor/device or class) and get `probe/start`.
* Existing drivers register their nodes (NVMe -> partitions -> volumes,
  xHCI -> ports -> devices, HDA, Wi-Fi, NICs, touchpad, ACPI battery).
* `ioreg` program (via a `registry_list` syscall) prints the tree.
* Cut: no user-space drivers (DriverKit) yet.
* Test `ioreg`: the tree has PCI, NVMe and a volume.

## 6. launchd

* `user/programs/launchd.c` is pid 1: reads `/SYS/LAUNCHD.CFG` (`name
  program args keepalive`), starts services, restarts them when they die,
  owns the bootstrap name service (ports).
* `login` becomes a user program (auth via a `users` syscall); it starts the
  shell (or, after stage 7, the desktop). The kernel shell stays only as an
  emergency fallback when launchd is missing.
* Services: `logind`, `netd` (DHCP + Wi-Fi reconnect moved out of the idle
  hook), `soundd` later.
* Test `launchd`: kill a service, it comes back; boot reaches login.

## 7. WindowServer and the desktop

* `windowserver` program owns the framebuffer (gfx syscalls) and the mouse
  and keyboard (an input-events port from the kernel).
* Clients: `window_create(w, h)` via a port -> a shared memory surface
  (stage 1); `window_damage(rect)`; input events back on the client's port.
  Compositing: back-to-front, damaged rectangles only, a cursor.
* `user/lib/ui.c` (AppKit-lite): window, label, button, text field, list,
  menu; the 8x16 font scaled.
* Shell surfaces: menu bar (app menu, clock, battery, Wi-Fi), Dock (app
  icons), window drag/close/minimise.
* Apps: Terminal (runs a shell with pipes as stdin/stdout, draws text),
  Finder-lite (folders, open with), Viewer (images), Video (existing player
  drawing into a window).
* Cut: no GPU acceleration, no transparency effects, no resizing first round.
* Test `desktop`: windowserver starts, a test app opens a window, a
  screenshot shows it, a click reaches the app.

## 8. App bundles and launcher

* `/APPS/NAME.APP/` = `INFO.CFG` (name, binary, icon, permissions) +
  binary + `ICON.BMP`. The Dock and a launcher list the bundles.
* Cut: static linking stays (no dyld): shared libraries are not worth it
  at this size.
* Test `apps`: a bundle in /APPS appears in the Dock and opens.

## 9. File system with permissions (optional)

* Either Unix permissions kept in a side table on FAT32 (`/.PERMS`), or a
  small log-structured / copy-on-write file system on its own partition.
  Decide when stage 4 is done; FAT32 stays for Windows exchange.

## 10. Sandbox and polish

* Entitlements in `INFO.CFG`: which folders and ports an app may use; the
  kernel checks them in open/port_lookup.
* ACPI S3 sleep and wake (lid switch), a settings app (Wi-Fi, theme, users).

## Working rules (to keep sessions short)

* Read this file and the stage's files only; no repo-wide reviews.
* Write each module whole, build once, run the stage's test plus the suite
  once; fix; commit when the person says so.
* Laptop checks only at the end of a stage (Wi-Fi, touchpad, NVMe, battery
  cannot be tested in QEMU).
