# Compatibility and application-ecosystem paths for Terminal OS (as of 7 November 2026)

*Scope: external technical routes to application-ecosystem parity (framework categories C4, C10, C11, C12, C15, C16, C17), tailored to Terminal OS (from-scratch x86_64, C/NASM, one main developer, started 2025-10-02; own UEFI loader, monolithic kernel under a big kernel lock, int 0x80 syscalls, own compositing window server with Intel Gen12 GPU effects, mlibc and Ladybird ports in progress). Citations marked "(bk)" point to well-known primary pages cited from background knowledge that were not re-fetched in this session; verify exact wording before quoting. Effort estimates in "Inferences" are the researcher's estimates derived from the cited precedents, not sourced facts.*

Project-specific facts used below come from the brief and from the repo: the mlibc port copies `sysdeps/terminalos`, symlinks mlibc's Linux ABI headers "like most mlibc ports: link the same headers Astral uses", and sets `MLIBC_MMAP_ALLOCATE_DSO` because "the kernel maps nothing fixed below 4 GiB" ([toolchain/mlibc/setup-port.sh](C:\Users\hp\Desktop\os\toolchain\mlibc\setup-port.sh)). The toolchain directory also contains `ladybird/` (build.sh, patch.py), `skia/args.gn`, `gpu/` (zebin.py, genheader.py: an Intel GPU binary toolchain), `libs/meson-cross.txt`, `cmake/TerminalOS.cmake` and `ports/build.sh` (local repo listing).

## Q1. POSIX via mlibc: what an mlibc port needs from the kernel, who uses it, what runs, and which kernel features are prerequisites

### Takeaway
mlibc's mandatory port surface is tiny (16 sysdeps get a static hello world), so the real work is kernel *semantics* behind the optional sysdeps: futex, full mmap/mprotect/mremap behaviour, poll/epoll/eventfd/timerfd/signalfd, AF_UNIX sockets with fd passing, PTYs, POSIX signals, fork/exec details, and /dev, /proc and /sys files. Two mlibc OSes set the calibration. Astral (one developer, C, x86_64, the same Linux ABI headers Terminal OS links) went from a 2023 rewrite to X.org, GCC, Wine and OpenJDK/Minecraft by late 2025. Managarm (team, 12 years) runs Weston, KDE Plasma 6, GTK2-4/GNOME apps and, in October 2026, Epiphany.

### Cited Findings
- **mlibc basics.** mlibc is MIT-licensed ("Copyright (C) 2015-2025 mlibc Contributors"; the file also carries musl's MIT terms) — [mlibc LICENSE](https://raw.githubusercontent.com/managarm/mlibc/master/LICENSE). It supports x86-64, AArch64, RISC-V, IA-32, m68k and LoongArch64 and "provides a clean syscall abstraction layer for new operating system ports". Option sets can be switched per port: POSIX APIs such as pthread sit behind the `posix` option and Linux APIs such as epoll behind `linux`, and `linux_kernel_headers` is "required if the linux option is enabled". The README says "We recommend that new ports do not build from master" because internal changes break out-of-tree sysdeps. It also states mlibc is "capable enough to run a range of software, including Xorg, several Wayland compositors, Mesa, and web browsers like WebKitGTK" — [mlibc README](https://github.com/managarm/mlibc).
- **Kernel prerequisites (mlibc book).** The book lists only four: text output to a screen or serial port, basic paging and virtual memory, loading an ELF and jumping to its entry point with a mapped stack, and handling syscalls. A filesystem is not strictly necessary at first — [mlibc book: kernel prerequisites](https://docs.managarm.org/mlibc-book/porting/kernel_prerequisites.html).
- **Port setup (part 1).** A port adds `sysdeps/<os>`, registers it in the top-level `meson.build`, declares `sysdep_supported_options` (for example `'posix': true`), symlinks ABI headers from `abis/linux`, and does a headers-only build (`-Dheaders_only=true`) to bootstrap the cross toolchain — [mlibc book part 1](https://docs.managarm.org/mlibc-book/porting/implementing_sysdeps_p1.html).
- **The 16 mandatory sysdeps (part 2).** Exit, FutexWait, FutexWake, Open, Read, Write, Seek, Close, ClockGet, LibcLog, LibcPanic, AnonAllocate, AnonFree, VmMap, VmUnmap and TcbSet are "enough for running a basic statically-linked hello world program". The book recommends getting static binaries working before dynamic ones and warns: "People often forget to implement things like auxiliary vectors which are required by the spec" — [mlibc book part 2](https://docs.managarm.org/mlibc-book/porting/implementing_sysdeps_p2.html).
- **Astral (mlibc-based, MIT, single developer).** The README lists an "SMP capable preemptible kernel" and "Ports: X.org, wine, fvwm3, gcc, bash, quake, vim and more", with xbps packages and sndiod audio. Networking covers TCP, UDP, DHCP and 802.11; filesystems are tmpfs, devfs, ext2 and FAT12/16/32; ACPI comes from uACPI. Hardware: NVMe, AHCI, virtio, rtl8188eu Wi-Fi with WPA2, rtl8169, xHCI with HID and hubs, Intel HDA — [Astral README](https://github.com/mathewnd/astral). The author says "this iteration I have been working on since 2023" (initial commit April 2023, reusing 2022-23 code) and that "modern minecraft (1.20) and even modded (GTNH) is working". The HN thread mentions an mlibc bug that affected OpenJDK (char values above 127 treated as negative) — [HN, Oct/Nov 2025](https://news.ycombinator.com/item?id=45958988).
- **Managarm (mlibc's parent project).** It is microkernel-based with fully asynchronous I/O and claims "POSIX and Linux API compatibility". It supports Linux APIs such as epoll, timerfd, signalfd and tmpfs, and pseudo filesystems (/sys, /proc). It runs "Weston (the Wayland reference compositor), kmscon (a system console), KDE Plasma 6, GNU Coreutils, Bash, nano and others". Graphics drivers: generic VBE, Intel G45, virtio GPU, Bochs VBE, VMware SVGA and nVidia (Turing+) — [Managarm README](https://github.com/managarm/managarm). Dated news posts: "Porting Software to Managarm" (Apr 2020), "Implementing pthread cancellation in mlibc" (Jan 2023), "Managarm now available on os-test" (Nov 2025), "Managarm receives NGI Zero grant to enhance its web capabilities" (Jun 2026), "Porting Epiphany to Managarm" (13 Oct 2026) and "Status Update and 12th Anniversary" (18 Oct 2026) — [managarm.org](https://managarm.org/).
- **Managarm's GNOME port.** Its "Porting GNOME to Managarm, Part 1" post covers GTK4 and gnome-calculator, noting that GTK2 and GTK3 ports already existed. Running gobject-introspection binaries built for Managarm proved infeasible, and an ABI difference between the Linux and Managarm ports forced a rebuild — [managarm.github.io PR #11](https://github.com/managarm/managarm.github.io/pull/11/files). A Managarm developer reported Xwayland running, after epoll bugs that took effort to track down — [OSDev forum](https://forum.osdev.org/viewtopic.php?p=307769).
- **Which syscalls come first.** Tsai et al. give an order for adding syscalls to a prototype OS:
  - Stage I (40 syscalls, 1.12% weighted completeness): e.g. mmap, vfork, exit, read, fcntl, kill, dup2.
  - Stage II (81 syscalls, 10.68%): e.g. mremap, ioctl, access, socket, poll, recvmsg.
  - Stage III (145 syscalls, 50.09%): e.g. shutdown, symlink, alarm, listen, shmget, pread64.
  - Stage IV (202 syscalls, 90.61%): e.g. flock, semget, ppoll, mount, brk, pause, clock_gettime.
  - Stage V (272 syscalls, 100%).

  System V message queues outrank POSIX ones (msgget 100% API importance vs mq_open 5%). 12,039 binaries hard-code pseudo-file or device paths, with 5,846 unique paths, and 3,324 hard-code /dev/null — [Tsai et al., ;login: Fall 2016](https://www.cs.unc.edu/~porter/pubs/login-fall16.pdf).
- **Loupe measurements.**
  - Prominent modern syscalls are futex, the epoll family, openat and prlimit64.
  - Of arch_prctl only ARCH_SET_FS is needed.
  - Common ioctls are TCGETS, FIONBIO and FIOASYNC.
  - A hello world's start-up uses 28 syscalls with dynamic glibc 2.28 against 11 with dynamic musl, and 11 against 6 when static — [Loupe, ASPLOS'24](https://arxiv.org/html/2309.15996v1).
- **PTYs are a hard prerequisite for terminals.** On Haiku's Xlibe effort, xterm compiled but did not start because it could not allocate PTYs — [OSNews on Xlibe](https://www.osnews.com/story/134362/xlibe-an-xlib-x11-compatibility-layer-for-haiku/).
- **Linux-specific fd types are needed for the desktop stack.** FreeBSD needed epoll-shim (epoll on kqueue, plus emulated timerfd, signalfd and eventfd) to port libinput, libevdev and Wayland. Shimmed fds "cannot be shared between processes", are closed on fork(), and return EOPNOTSUPP when passed over sendmsg — [epoll-shim](https://github.com/jiixyj/epoll-shim).
- **A conformance yardstick exists.** os-test (Sortix) is a POSIX.1-2024-derived suite covering 16 operating systems, with test suites including basic, io, limits, malloc, paths, process, pty, signal, stdio and udp — [FOSDEM 2026 talk](https://archive.fosdem.org/2026/schedule/event/CMCWY9-os-test_measuring_posix_compliance_on_every_single_os/). Published scores: Redox 73% (2424/3313, Sept 2025) — [Redox os-test results](https://doc.redox-os.org/test-results/os-test/os-test-20250922/); Genode 57% (842/1454, Aug 2026) — [Genode os-test results](https://genode.org/files/jws/os-test/20260820/html/index.html). An NLnet-funded "os-test expansion" started in 2026-06 — [NLnet](https://nlnet.nl/project/os-test/).

### Inferences
- **Terminal OS is on the proven path.** It reuses Astral's Linux abi-bits pattern, which means errno values, `struct stat`, O_* flags and ioctl numbers match Linux. Ports then need fewer patches, and a later Linux-binary personality (Q2) shares the same structures.
- **The "nothing fixed below 4 GiB" mmap policy should be revisited early.** Wine (Q7: its preloader reserves the low 8 GB on x86-64), JITs (LLVM/llvmpipe, Java, JavaScriptCore), emulators and some sanitizers expect MAP_FIXED or MAP_FIXED_NOREPLACE at arbitrary addresses, including low ones, plus large PROT_NONE reservations and MAP_NORESERVE.
- **Prerequisite checklist, ordered by unlock value:**
  1. futex (wait/wake, private, bitset optional).
  2. Correct mmap semantics: shared/private file maps, MAP_ANONYMOUS|MAP_SHARED across fork, mprotect (incl. PROT_EXEC for JITs), mremap, msync, madvise stubs.
  3. TLS via FS base, a TcbSet equivalent (an arch_prctl(ARCH_SET_FS) analogue).
  4. Correct auxv/stack layout for ld.so.
  5. fork/vfork/posix_spawn/execve with O_CLOEXEC, waitpid/wait4, process groups and sessions.
  6. Signals: SA_SIGINFO, sigaltstack, ucontext, restartable syscalls, signal masks per thread.
  7. poll/ppoll/select, then epoll + eventfd + timerfd + signalfd (Wayland, GLib, Qt, libinput and Ladybird's event loops depend on them).
  8. pipes and socketpair.
  9. AF_UNIX stream and datagram with SCM_RIGHTS fd passing and SO_PEERCRED (Wayland, D-Bus, Ladybird IPC).
  10. PTYs (/dev/ptmx plus termios; shells, terminal apps, ssh).
  11. shm_open/memfd_create and ftruncate.
  12. /dev/null, /dev/zero, /dev/urandom, getrandom, /proc/self/exe, /proc/self/maps, /proc/cpuinfo and /proc/meminfo.
  13. clock_gettime monotonic and realtime, nanosleep.
  14. flock/fcntl locks.
  15. SysV shm/sem (Tsai shows these are surprisingly important).
- **Effort, inferred from Astral.** Astral reached GCC, X.org, Wine and OpenJDK in about 2.5 years with comparable hardware breadth. Terminal OS already has the drivers (NVMe/AHCI, xHCI, HDA, Wi-Fi, TCP/IP), so for one developer the estimate is:
  - static hello world: days;
  - dynamic linking plus bash/coreutils: about 1-2 months;
  - native binutils/GCC or LLVM, Python and Git: about 2-4 more months;
  - X11 or Wayland with toolkit apps: about 3-9 more months.
- **The big kernel lock is a throughput limit, not a correctness blocker.** Native builds (GCC/LLVM/Ladybird) and heavily threaded apps (Skia, llvmpipe, Ladybird) will suffer; plan finer-grained locking before chasing performance parity.
- **Run os-test monthly** as the C4 scoreboard, the way Managarm, Redox and Genode publish results. Pin mlibc to a release, per the mlibc README advice.

### Gaps
- The current mlibc README no longer names its OS ports, so a definitive list (Ironclad, Vinix, Lyre, Aero, Keyronex, Menix and others) was not verified this session.
- Astral's X.org graphics path (fbdev vs a DRM emulation) and how much of its Wine works were not found.
- No per-milestone timeline for Managarm was found (only the 12-year span and dated posts).

## Q2. Linux binary compatibility: precedents, syscall counts, hardest parts, trade-offs against native ports

### Takeaway
The Linux ABI is a well-measured but long road. Any application needs about 40 syscalls; half of Debian/Ubuntu, weighted by popularity, needs 145; 90% needs 202. Mature layers took teams years: gVisor (274 of 350), Asterinas (230+), FreeBSD's Linuxulator. Microsoft replaced WSL1's syscall translation with a real kernel in a VM (WSL2) to get "full system call compatibility". Desktop binaries add surfaces far beyond syscalls (ioctls including DRM, /proc and /sys formats, D-Bus, udev, Wayland/X11). For Terminal OS a Linux personality is a later accelerator, but it is cheap to keep possible because the ABI headers already match Linux.

### Cited Findings
- **Tsai et al. (EuroSys 2016 Best Paper; Ubuntu/Debian 15.04 binaries).**
  - Linux 3.19 x86-64 defines 318 syscalls, and 224 have 100% API importance ("if one of these 224 calls is missing, at least one" application fails on every installation).
  - Of 635 ioctl opcodes, only 52 are indispensable and 447 are never used. Of 18 fcntl opcodes, 11 are indispensable; of 44 prctl opcodes, 9 are.
  - Of 1,274 glibc 2.21 functions, 42.8% have 100% importance.
  - Graphene "offers support for 143 out of 318".
  - Weighted completeness: User-Mode-Linux and L4Linux over 90% with more than 280 syscalls; FreeBSD's Linux emulation 62.3% ("missing some less important system calls (e.g., inotify_init)"); Graphene 0.42%, rising to 21.1% after adding two scheduling syscalls — [;login: Fall 2016](https://www.cs.unc.edu/~porter/pubs/login-fall16.pdf); [project page](https://oscarlab.github.io/api-compat-study); [full paper](https://www.cs.unc.edu/~porter/pubs/syspop16.pdf).
- **Loupe (ASPLOS 2024, dynamic analysis of 116 apps).**
  - Redis: static analysis finds 103 syscalls, but "only 42 are actually needed to reliably pass its entire test suite, and just 20 to run redis-benchmark".
  - Across all 116 apps, 148 syscalls need real implementations.
  - On average 46% (test suites) and 60% (benchmarks) of invoked syscalls can be stubbed or faked.
  - The 25 most commonly required syscalls appear in more than 80% of apps.
  - Example plans start from Kerla (58 syscalls), Fuchsia (152) and Unikraft (174), and over 80% of plan steps need only 1-3 new syscalls — [Loupe](https://arxiv.org/html/2309.15996v1).
- **gVisor.** "Of 350 syscalls, 274 syscalls have a full or partial implementation"; 76 are unsupported. Most runtimes have fallbacks for unimplemented ones — [gVisor amd64 compatibility](https://gvisor.dev/docs/user_guide/compatibility/amd64/).
- **Asterinas** (Rust framekernel, about 4 years old) supports "over 230 Linux system calls" on x86-64, with per-call flag-coverage tables. An older copy of the page said 177 of 336. Release 0.16.0 (Aug 2025) added memfd_create and pidfd_open, among others. Its 2026 priority is production readiness for standard and confidential VMs, and its `sctrace` tool checks an app's syscalls against Asterinas — [Asterinas book](https://asterinas.github.io/book/kernel/linux-compatibility/index.html); [0.16.0 announcement](https://asterinas.github.io/2025/08/04/announcing-asterinas-0.16.0.html).
- **WSL.** WSL 1 used "a translation layer that was built by the WSL team". WSL 2's goals were "to increase file system performance and add full system call compatibility" by running a real Linux kernel under virtualization. It was up to 20x faster unpacking a tarball and 2-5x faster for git clone, npm install and cmake — [Microsoft Learn: WSL versions](https://learn.microsoft.com/windows/wsl/compare-versions).
- **Fuchsia Starnix.** RFC-0082 was accepted in 2021. Starnix runs "unmodified Linux binaries" without a VM by implementing the Linux UAPI, and has been tested with glibc- and bionic-based binaries — [RFC-0082](https://fuchsia.dev/docs/contribute/governance/rfcs/0082_starnix); [Starnix docs](https://fuchsia.googlesource.com/fuchsia/+/HEAD/docs/concepts/components/v2/starnix.md).
- **illumos LX brand.** It runs unmodified 32- and 64-bit Linux user software with emulated /proc, cgroup and sysfs, and a configurable `kernel-version` string. It does not support Linux kernel modules, and some syscalls are unimplemented — [OmniOS lx(7)](https://man.omnios.org/man7/lx); [SmartOS lx(7)](https://smartos.org/man/7/lx).
- **FreeBSD Linuxulator** provides Linux binary compatibility in-kernel, without a VM — (bk) [FreeBSD Handbook: Linux binary compatibility](https://docs.freebsd.org/en/books/handbook/linuxemu/).
- **Managarm** offers Linux compatibility "at the user space level" through mlibc (source compatibility). Its binaries are not Linux binaries: an ABI difference forced rebuilds of gobject-introspection tooling — [Managarm README](https://github.com/managarm/managarm); [GNOME porting post](https://github.com/managarm/managarm.github.io/pull/11/files).
- **Sandboxed browsers lean on Linux-only security APIs.** Ladybird's sandbox uses "seccomp/Landlock on Linux" — [Ladybird June 2026 newsletter](https://ladybird.org/newsletter/2026-06-30).

### Inferences
- **The hardest Linux surfaces for a desktop:**
  - the DRM/KMS and GPU-vendor ioctl uAPIs (Mesa's drivers are built against them; Q3);
  - ptrace (gdb, strace, crash reporters);
  - namespaces and seccomp-bpf (Flatpak/bubblewrap and the Chromium, Firefox and Ladybird sandboxes);
  - io_uring (increasingly used by runtimes, though most fall back);
  - clone3, rseq and futex_waitv (newer glibc and Wine/Proton fsync);
  - the vDSO;
  - exact /proc and /sys text formats parsed by glibc, systemd-era libraries and browsers;
  - netlink (network configuration tools);
  - udev/evdev (input).

  None of these were measured this session. The ranking is inference.
- **Trade-off.** Native ports through mlibc need source and rebuilds, but they are what every independent desktop OS that reached a usable app set has done (Astral, Managarm, SerenityOS, Haiku). Binary compatibility is the only route to proprietary Linux apps (Steam, Electron apps, JetBrains IDEs). Those apps also need the whole Linux desktop userland (glibc, X11/Wayland, D-Bus, PipeWire/PulseAudio, fontconfig, Vulkan), so binary compatibility is effectively "ship a Linux distro userland on your kernel".
- **Cheap option for Terminal OS.** Add a second entry point (the x86-64 `syscall` instruction with Linux numbers) beside int 0x80, sharing implementations. Target statically linked musl binaries first: Loupe shows 6 start-up syscalls for static musl. Use Loupe or sctrace-style tracing to order the rest. With the Linux abi-bits already in place, the estimate is 1-3 months for static CLI binaries, 6-12 months for many dynamic glibc CLI programs, and multi-year for GUI binaries (they depend on Q3 graphics plus D-Bus/udev).
- **Recommendation:** native ports are the primary path; keep the Linux personality as a stretch goal after Wayland and Mesa exist.

### Gaps
- No modern (2024-2026) measurement of the syscalls and ioctls needed by Linux *desktop GUI* apps was found; Tsai's data is from Ubuntu 15.04.
- No current FreeBSD Linuxulator coverage figure, no final WSL1 syscall count, and no published effort data for ptrace or io_uring emulation were found.
- Starnix's 2026 completeness is not published.

## Q3. Graphics stack: porting Mesa, software rendering, Intel/AMD acceleration routes, and what a Wayland layer needs (GTK, Qt, SDL, XWayland)

### Takeaway
Use two tiers:
1. **Software rendering first.** Mesa llvmpipe for GL and lavapipe for Vulkan need a working LLVM JIT, threads and an EGL/winsys glue.
2. **Hardware later, through a DRM-like uAPI.** GEM buffers, command submission, sync objects, dma-buf sharing and KMS, with libdrm on top.

Precedents differ on cost. A minimal own render driver plus Mesa took Genode four months (Intel Gen8). Porting Linux drivers wholesale (FreeBSD drm-kmod) is a permanent maintenance stream with contested licensing. For applications, the highest-leverage item is making Terminal OS's existing compositor speak Wayland: one protocol implementation unlocks GTK, Qt, SDL, XWayland, Wine's Wayland driver and the Chromium/Firefox Wayland backends.

### Cited Findings
- **Genode's Intel GPU route (17.08).**
  - Genode built "an experimental GPU multiplexer for Intel HD Graphics Gen8 (Broadwell class) devices" "within four months". The multiplexer "consists of about 4,200 lines of code"; the client-side Mesa DRI i965 driver adds about 78,000 lines; Linux's Intel driver is about 100k lines.
  - Mesa went from 7.8.x to 11.2.2. EGL is the front end and loads a DRI back end (i965 or swrast).
  - They implemented "the DRM interface in our ported version of the Intel i915 DRM driver".
  - Mode-setting stayed in a separate component (VESA). The multiplexer had no power management and "permanently keeps the GPU awake" — [Genode 17.08 release notes](https://genode.org/documentation/release-notes/17.08).
  - Gen9+ support with Mesa 21.0's iris Gallium driver arrived in Genode 21.11, four years later — [Phoronix](https://www.phoronix.com/news/Genode-OS-21.11).
- **Mesa has supported Gen12 since 19.3.** Initial Gen12 (Tiger Lake) support landed in Mesa 19.3 for both anv (Vulkan) and iris (GL) — [Phoronix](https://www.phoronix.com/news/Mesa-19.3-RC1-Released).
- **FreeBSD drm-kmod (Linux DRM drivers over LinuxKPI).**
  - It was updated to match Linux 6.12 LTS in June 2026, for FreeBSD 15.1 and later — [Phoronix](https://phoronix.com/news/FreeBSD-drm-kmod-Linux-6.12).
  - LinuxKPI work is continuous: PCI device teardown, and suspend/resume for LinuxKPI wireless drivers — [FreeBSD Foundation laptop update, May 2026](https://freebsdfoundation.github.io/proj-laptop/monthly-updates/2026-05).
  - Licensing is contested in community discussion. One forum poster argues DRM files tagged GPL make the compiled module GPL; others dispute which files are GPL-only — [FreeBSD forums](https://forums.freebsd.org/goto/post?id=682318). In a Phabricator review, LinuxKPI's `gplv2` directory code was called "mostly not applicable to be moved into head" — [FreeBSD D26598](https://reviews.freebsd.org/D26598).
- **Haiku's Radeon Vulkan attempt.** X512 began porting Mesa RADV, starting from Lavapipe and writing "his own libdrm implementation". Completion was not confirmed — [Phoronix](https://www.phoronix.com/news/RADV-Port-WIP-For-Haiku).
- **Redox (2026).** It got its first Vulkan in Feb 2026 through the CPU-based Lavapipe, "not with any hardware driver support yet". cosmic-comp runs as a Winit window, as a proof of concept without input — [Phoronix, Feb 2026](https://phoronix.com/news/redox-os-february).
- **Managarm** ships native drivers for VBE, Intel G45, virtio GPU, Bochs, VMware SVGA and nVidia Turing+, and runs Weston, kmscon and KDE Plasma 6 — [Managarm README](https://github.com/managarm/managarm). Its Xwayland needed epoll bug-fixing — [OSDev forum](https://forum.osdev.org/viewtopic.php?p=307769).
- **What the Wayland stack expects from the OS.** epoll-shim exists because libinput, libevdev and Wayland require epoll, timerfd and signalfd semantics — [epoll-shim](https://github.com/jiixyj/epoll-shim). The Wayland wire protocol runs over a UNIX domain stream socket and passes file descriptors (shared-memory buffers, keymaps, dma-bufs) as ancillary data — (bk) [Wayland docs, wire format](https://wayland.freedesktop.org/docs/html/ch04.html).
- **llvmpipe** is Mesa's LLVM-JIT, multithreaded software rasterizer. Mesa is mostly MIT-licensed — (bk) [llvmpipe docs](https://docs.mesa3d.org/drivers/llvmpipe.html); (bk) [Mesa license](https://docs.mesa3d.org/license.html).
- **i915 uAPI reference.** Intel's i915 uAPI (GEM create/mmap, execbuffer2, contexts, queries) is documented in the kernel's DRM driver uAPI pages — (bk) [Linux DRM driver uAPI](https://docs.kernel.org/gpu/driver-uapi.html).
- **An alternative to an X server.** Haiku's Xlibe implements Xlib directly on the native window API, enough for Cairo and GTK "with only occasional glitches" — [Xlibe](https://github.com/waddlesplash/xlibe); [OSNews](https://www.osnews.com/story/134362/xlibe-an-xlib-x11-compatibility-layer-for-haiku/).
- **Ladybird's graphics stack on Linux** is Skia with the Vulkan feature, ANGLE (for WebGL), and FFmpeg with Vulkan — [Ladybird vcpkg.json](https://raw.githubusercontent.com/LadybirdBrowser/ladybird/master/vcpkg.json).

### Inferences
- **Terminal OS's unusual position.** It already submits work to the Intel Gen12 GPU itself (own effects, own zebin shader toolchain). That is the hardest part of a Genode-style route.
  - *Option A (recommended): implement in-kernel the subset of the i915 uAPI that iris and anv actually call* (GEM create/mmap-offset/set-domain, execbuffer2, context create/param, I915_QUERY engine and topology info, syncobj, PRIME/dma-buf export/import), plus port libdrm. Unmodified Mesa iris (GL 4.6) and anv (Vulkan) would then work on Gen12 only. Genode's precedent suggests about 4-9 months for one developer, more than Gen8 because Gen12 iris/anv expect a newer uAPI.
  - *Option B: port Linux i915/xe through a LinuxKPI-style shim.* This gives broad hardware support (incl. AMD amdgpu), but FreeBSD shows a permanent multi-person maintenance stream, and GPL obligations if the code links into the kernel.
- **Software path first.** Cross-build LLVM, then Mesa (meson cross file already present) with llvmpipe and lavapipe, and expose EGL through a native platform or OSMesa/surfaceless. Estimate: 1-3 months. Needs PROT_EXEC JIT memory, threads and futex.
- **Wayland, two ways:**
  - *Option 1 (fits the existing compositor):* embed libwayland-server (MIT) in the window server and implement wl_compositor, wl_shm, xdg-shell, wl_seat with xkbcommon keymaps, wl_output and wp_viewporter/fractional-scale, then linux-dmabuf once Mesa hardware exists. Estimate: 2-4 months for core protocols, plus 1-2 months for Xwayland (X11 apps, including Wine's winex11).
  - *Option 2 (Managarm route):* expose DRM/KMS and evdev and run Weston or KWin unmodified. This needs much more kernel uAPI (KMS atomic, libinput/udev), but gets a complete desktop for free.
  - Avoid writing per-toolkit backends for GTK; GTK's supported backends are X11, Wayland, Win32, macOS and Broadway — (bk) [GTK running docs](https://docs.gtk.org/gtk4/running.html).

### Gaps
- Mesa's own guidance for adding a new OS or winsys was not fetched.
- Whether Gen12 (Tiger Lake) needs GuC/HuC firmware loading for iris/anv on a non-Linux kernel is unverified.
- The size of Managarm's DRM/KMS emulation, and Astral's X.org driver choice, were not found.
- No public AMD (amdgpu) port to a hobby kernel was found beyond Haiku's work in progress.

## Q4. Browsers: Ladybird in 2026, porting it to a new OS, Firefox/Chromium porting precedents, and fallbacks

### Takeaway
Ladybird is the natural primary browser for Terminal OS: BSD-2-licensed, independent engine, and already being ported. It is a moving target, though:
- still pre-alpha, with a first alpha promised for 2026 on Linux and macOS only;
- 49 vcpkg dependencies;
- adopting Rust;
- seccomp/Landlock sandboxing on Linux;
- a separate compositor process with GPU isolation.

A third-party Haiku port broke repeatedly. Chromium and Firefox ports cost years of patch maintenance (the shared BSD patchset) or a Qt/GTK/Wayland stack (Haiku's QtWebEngine). WebKitGTK via GTK (Managarm, October 2026) is the second route, and NetSurf the cheap fallback.

### Cited Findings
- **Status.**
  - Ladybird "is in a pre-alpha state, and only suitable for use by developers". It runs on "Linux, macOS, Windows (with WSL2), and many other *Nixes"; licence is 2-clause BSD — [Ladybird GitHub](https://github.com/LadybirdBrowser/ladybird).
  - The website says "Our first Alpha for macOS and Linux is coming later this year", aimed at developers and early adopters. Windows: only the JS engine is built and tested in CI so far; "Mobile platforms are not a current focus".
  - It is funded by donations and sponsorships with "a small full-time engineering team", and "We have since adopted Rust as our C++ successor language and are incrementally porting subsystems to Rust" — [ladybird.org](https://ladybird.org/).
  - A Wikipedia snapshot (retrieved 26 Oct 2026) lists alpha 2026, beta 2027, stable 2028 and no binary releases — [Wikipedia](https://en.wikipedia.org/wiki/Ladybird_(web_browser)). No source up to late Oct 2026 reported the alpha as shipped.
  - A June 2026 ITHome report (secondary) says the project stopped accepting public pull requests during its alpha sprint — [ITHome](https://www.ithome.com/0/960/789.htm).
- **Architecture.** Ladybird has a main UI process, WebContent renderer processes ("Documents from different sites run in separate renderer processes, each sandboxed") and helper processes for "networking, image decoding, compositing and media" — [Ladybird GitHub](https://github.com/LadybirdBrowser/ladybird). The June 2026 newsletter adds:
  - "seccomp/Landlock on Linux and Seatbelt profiles on macOS"; "Sandboxing is now on by default";
  - the compositor moved into its own process; canvas command lists "are now replayed and rasterized in the compositor", and "WebGL commands are streamed over shared memory", so "WebContent processes no longer need direct GPU access";
  - adoption of Rust's encoding_rs, Wuffs for GIF and a Cranelift-based Wasm JIT — [Ladybird newsletter 2026-06-30](https://ladybird.org/newsletter/2026-06-30).
- **Dependencies (vcpkg.json, 49 entries), grouped:**
  - *Graphics:* skia (Linux/BSD features freetype, fontconfig, harfbuzz, icu, vulkan); angle; vulkan, vulkan-headers, vulkan-memory-allocator (Linux/BSD/Android).
  - *Media:* ffmpeg (avcodec, swresample, dav1d, opus, vulkan on Linux).
  - *Networking:* curl (brotli, http2, http3, openssl, websockets, zstd); openssl; libpsl; libproxy (not Android/BSD); dbus (Linux/FreeBSD).
  - *Text:* icu; harfbuzz (freetype, icu); fontconfig; woff2.
  - *Image formats:* libjpeg-turbo, libpng (apng), libwebp, libavif, tiff, wuffs.
  - *UI and platform:* qtbase (FreeBSD/macOS/Windows), sdl3, libedit.
  - *Runtime, debugging and utilities:* mimalloc (secure), llvm-libc, cpptrace/libdwarf, simdutf, simdjson, sqlite3, libxml2, pdfjs, libtommath, brotli, zlib.
  - Pinned versions include skia 148, Qt 6.11.2, ffmpeg 9.0.2, curl 8.23.0, openssl 3.6.5 and icu 78.3 — [vcpkg.json](https://raw.githubusercontent.com/LadybirdBrowser/ladybird/master/vcpkg.json).
- **Haiku's Ladybird port.** It ran with bugs: crashes on heavy sites, and "the WebProcess of a second tab instantly crashes". It was later "broken again since December", with worry that Ladybird "may require more third-party libraries and focus only on Linux and MacOS" — [Haiku forum: porting Ladybird](https://discuss.haiku-os.org/t/my-progress-at-porting-ladybird-the-serenityos-browser/13886). Two developers (nipos, 3dEyes) worked on ports independently (June 2026 recollection) — [Haiku forum: Ladybird status](https://discuss.haiku-os.org/t/ladybird-browser-development-status-update-and-roadmap/19507).
- **Other Haiku browsers.**
  - Falkon on QtWebEngine with Qt 5.15.7 bundles an old Chromium (94) and crashes too often for daily use — [Haiku forum: Falkon](https://discuss.haiku-os.org/t/falkon-ex-qupzilla/8916?page=2); [Haiku forum: is Falkon the best](https://discuss.haiku-os.org/t/is-falkon-the-best-web-browser-in-haiku/13056).
  - A Firefox ESR 102 port by a single developer was still under debugging — [Haiku forum: porting Firefox](https://discuss.haiku-os.org/t/progress-on-porting-firefox/13493?page=5).
  - WebPositive 1.3-alpha runs on HaikuWebKit 1.9.4 / WebKit 616.1.7 (same forum sources).
- **BSD Chromium.** FreeBSD uses "the combined patch set for OpenBSD and FreeBSD maintained by Robert Nagy" — [FreeBSD wiki: Chromium](https://wiki.freebsd.org/Chromium). OpenBSD confines each Chromium process type with pledge(2) and unveil(2) — [openports www/chromium](https://openports.pl/path/www/chromium). A EuroBSDCon 2021 talk details FreeBSD-specific patches (process handles, system information, HID, sndio) — [Porting Chromium to FreeBSD](https://papers.freebsd.org/2021/eurobsdcon/wolf-porting_chromium/).
- **Managarm's web work.** "Porting Epiphany to Managarm" (13 Oct 2026, i.e. WebKitGTK) and an NGI Zero grant "to enhance its web capabilities" (Jun 2026) — [managarm.org](https://managarm.org/). mlibc claims WebKitGTK runs on it — [mlibc README](https://github.com/managarm/mlibc).
- **NetSurf** is a small C browser with its own engine, GPLv2 — (bk) [NetSurf about](https://www.netsurf-browser.org/about/).
- **Rust targets for mlibc OSes.** Upstream Rust has Tier 3 `x86_64-unknown-managarm-mlibc` (plus aarch64 and riscv64gc variants, added 2025). Rust's platform docs say there is currently no std support for it. Managarm has had upstream LLVM support since LLVM 21.1.0 — [rustc platform support: managarm](https://doc.rust-lang.org/rustc/platform-support/managarm.html); [target spec](https://doc.rust-lang.org/nightly/nightly-rustc/src/rustc_target/spec/targets/x86_64_unknown_managarm_mlibc.rs.html).

### Inferences
- **What porting Ladybird to Terminal OS needs:**
  - *Toolchain:* a C++23 clang/GCC toolchain and CMake/Ninja with a vcpkg triplet or a system-libraries mode, plus a Rust toolchain with a Terminal OS (or mlibc) target, since Rust components are now in-tree. Upstream Rust has only a Tier 3 Managarm/mlibc target without std, so Terminal OS will likely need its own rustc target spec plus a port of the libc crate and std::sys (patterned on the Managarm target and Linux's std), or must keep Ladybird's Rust parts optional or cross-compiled. This is a significant hidden prerequisite to schedule early.
  - *POSIX services:* pthreads, AF_UNIX with SCM_RIGHTS (LibIPC), anonymous shared memory, poll/eventfd-style event loops, signals and mmap.
  - *Fonts and networking:* fontconfig/FreeType/HarfBuzz/ICU; curl with OpenSSL (the browser does not use the OS's BearSSL TLS 1.2).
  - *Media:* FFmpeg plus an audio sink.
  - *Graphics:* Skia CPU raster first, Vulkan later via lavapipe or anv (Q3).
  - *Optional at first:* ANGLE/WebGL can be disabled; sandboxing can be stubbed, later replaced by a pledge/unveil- or Landlock-like kernel facility.
  - *Frontend:* a native one on LibWebView (as the AppKit frontend does), or Qt6 through a QPA plugin (Q5).
- **Effort and risk.** Expect 1-3 months to render real pages given the in-progress port, and 6-12 months to reach usable daily browsing with video and heavy JS sites. Maintenance continues indefinitely: pin upstream revisions and rebase on a schedule, because upstream is Linux/macOS-focused and the Haiku port shows breakage risk.
- **Fallbacks, in order of cost:**
  1. NetSurf: weeks; good for docs and simple sites.
  2. WebKitGTK/Epiphany: needs GTK + Wayland + GLib + libsoup + GStreamer (Managarm path, many months).
  3. Firefox or Chromium: only after a Linux-like desktop stack; years, with permanent patch maintenance.

### Gaps
- Whether the Ladybird alpha shipped between late October and 7 November 2026 was not confirmed.
- Whether Rust is mandatory to build Ladybird today, and which subsystems are in Rust beyond encoding_rs and the Wasm JIT, was not verified.
- Servo as an embeddable alternative was not researched.
- Haiku's Firefox port status in 2026 was not found.

## Q5. Toolkits and SDKs: SDL2/SDL3, Qt QPA, GTK backends, Flutter embedder, native toolkit design

### Takeaway
Port SDL3 first: small, zlib-licensed, and designed for "dummy drivers first, real drivers next". Then choose between a Qt QPA plugin (the SerenityOS precedent) and, with more leverage, Wayland support that covers GTK, Qt and SDL at once. Flutter needs a custom embedder plus a Dart VM port, which Google does not support. A native SDK should follow the client-renders, compositor-composites model, with accessibility, IME, HiDPI and a stable C ABI designed in from day one.

### Cited Findings
- **SDL porting steps.** Add a platform entry in `include/SDL_platform.h` ("SDL_PLATFORM_X"), and either a CMake section or an `SDL_build_config_{platform}.h` based on `SDL_build_config_minimal.h`. Build first with the dummy drivers (audio disk/dummy, filesystem, video, haptic, joystick, generic threads, timers, loadso). Once you have "a working library without any drivers", implement real video, audio, joystick, thread, timer and filesystem drivers — [SDL README-porting](https://raw.githubusercontent.com/libsdl-org/SDL/main/docs/README-porting.md). SDL is zlib-licensed — (bk) [SDL license](https://www.libsdl.org/license.php).
- **SerenityOS** has "over three hundred ports". Its qt6-qtbase port (6.4.0) needed a working "serenity platform plugin" (the separate QSerenityPlatform project) for GUI applications — [SerenityOS Ports README (mirror)](https://git.rgbcu.be/RGBCube/serenity/src/commit/94de0061443fb669907ea4e3f04eef0f008a6ef9/Ports/README.md); [SerenityOS commits (mirror)](https://git.rgbcu.be/RGBCube/serenity/commit/b7cc7b4e024e8ea1ae2266557afd14872728dde8). The QPA model: plugins implement platform integration (windows, backing stores, fonts, themes) — (bk) [Qt QPA](https://doc.qt.io/qt-6/qpa.html).
- **Haiku's Xlibe** gets GTK running without a native GDK backend by emulating Xlib on Haiku's API. Its author's advice is to implement the X Render extension for Cairo — [Xlibe forum thread](https://discuss.haiku-os.org/t/xlibe-an-xlib-x11-compatibility-layer-for-haiku/11692); [Xlibe](https://github.com/waddlesplash/xlibe).
- **Flutter embedders.** The engine API "has no platform specific dependencies, has a stable ABI and is available in its entirety in a single C header file" (embedder.h), including a software-renderer configuration. Custom engine builds are not supported by the Flutter team and are costly to maintain; a GLFW example embedder is the template — [Custom Flutter Engine Embedders](https://flutter.googlesource.com/mirrors/flutter/+show/refs/heads/master/docs/engine/Custom-Flutter-Engine-Embedders.md); [embedder.h](https://github.com/flutter/engine/blob/869d9f528503778be1e5ab27ba53502f0cb20de2/shell/platform/embedder/embedder.h). flutter-pi needs `flutter_embedder.h` at compile time and `icudtl.dat` at runtime — [flutter-pi](https://github.com/lvsheng/flutter-pi).
- **Managarm's toolkits.** GTK2, GTK3, GTK4 and KDE Plasma 6 run on its Wayland stack — [GNOME porting post](https://github.com/managarm/managarm.github.io/pull/11/files); [Managarm README](https://github.com/managarm/managarm). Redox adopted COSMIC apps: a relibc fix was submitted from inside Redox using COSMIC Edit (Jan 2026), and COSMIC Settings runs — [lavx summary of Redox reports](https://news.lavx.hu/article/redox-os-makes-major-strides-with-vulkan-node-js-and-cosmic-desktop-integration); [Phoronix](https://phoronix.com/news/redox-os-february).
- **AccessKit** gives toolkits one accessibility tree and action model, with platform adapters for Windows UIA, macOS NSAccessibility, Unix AT-SPI and Android, plus a C API — [AccessKit (docs.rs)](https://docs.rs/crate/accesskit/latest); [lib.rs](https://lib.rs/accesskit).

### Inferences
- **Suggested order and effort for Terminal OS (own window server):**
  1. SDL3 video/audio/input backend on the native window server and HDA mixer: 2-6 weeks. Unlocks games, emulators and many small apps; Ladybird also lists sdl3.
  2. Qt6 QPA plugin: shared-memory backing store, FreeType/fontconfig font database, the UNIX event dispatcher, clipboard and IME hooks. 1-3 months; unlocks Qt Widgets/QML apps and a Qt-based Ladybird UI.
  3. GTK via Wayland (Q3) rather than a new GDK backend.
  4. Flutter only if there is a clear app need.
- **Native SDK best practices** (drawn from SerenityOS LibGUI, Haiku's Interface Kit and Wayland's design):
  - Client-side rendering into shared buffers, with damage regions, frame callbacks and vsync.
  - Scale-factor-aware layout (HiDPI and fractional scaling) and theming through tokens.
  - An accessibility tree in the AccessKit schema from the first widget.
  - An IME protocol, clipboard/drag-and-drop/file-chooser services, and app manifests with permissions.
  - A versioned C ABI with C++ and optionally Rust bindings, plus a public SDK sysroot, documentation and samples. Ship a cross toolchain (Docker images already exist) so outside developers can build apps without the OS source tree.

### Gaps
- No effort data was found for SerenityOS's Qt QPA plugin or for writing a new GDK backend.
- Dart VM porting requirements for a non-Linux OS were not fetched.
- A best-practice reference for native toolkit design comes from inference, not a single authoritative source.

## Q6. Packaging and updates: ports trees, package managers, signing, atomic/A-B updates, app stores

### Takeaway
Every comparable OS started with a source ports tree (SerenityOS Ports 300+, HaikuPorts, Managarm's xbstrap) and added binary packages later. Terminal OS should adopt an existing portable binary package manager (xbps, as Astral does; pkgsrc for breadth) rather than inventing one. Sign repositories with a simple Ed25519 scheme from the first public release, and get atomic updates cheaply with A/B system images or Haiku-style package-activation states, which its own UEFI loader can select.

### Cited Findings
- **SerenityOS Ports:** "over three hundred ports" built by shell-script recipes — [SerenityOS Ports README (mirror)](https://git.rgbcu.be/RGBCube/serenity/src/commit/94de0061443fb669907ea4e3f04eef0f008a6ef9/Ports/README.md).
- **Haiku packaging.**
  - packagefs mounts .hpkg packages into /boot/system without extracting them.
  - Each activation change backs up the previous state, and the boot loader can boot an older state's package list — [Mark Round, Haiku package management](https://www.markround.com/blog/2023/02/13/haiku-package-management/).
  - Users "load a former, working state from the boot options menu"; SoftwareUpdater can delete states older than 30 days but always keeps the last 10 — [Haiku SoftwareUpdater guide](https://www.haiku-os.org/docs/userguide/id/applications/softwareupdater.html).
  - Making an old state permanent is a user request without a clean answer — [Haiku forum](https://discuss.haiku-os.org/t/restoring-older-boot-state-permanently/7250).
- **pkgsrc** contains "over 26,000 packages"; "Code exists in pkgsrc to support 20+ Unix-like operating systems". Non-NetBSD platforms must bootstrap first, with per-platform `bootstrap/README.${platform}` notes — [pkgsrc.org](https://www.pkgsrc.org/).
- **Hobby OS precedents.** Astral uses xbps (Void Linux's package manager) — [Astral README](https://github.com/mathewnd/astral). Managarm builds its distribution with xbstrap — (bk) [xbstrap](https://github.com/managarm/xbstrap).
- **Atomic update designs (all bk):**
  - OSTree performs atomic upgrades of complete filesystem trees, with rollback — [OSTree introduction](https://ostreedev.github.io/ostree/introduction/).
  - ChromeOS keeps two root partitions (A/B), updates the inactive one and falls back on boot failure — [ChromiumOS autoupdate design](https://www.chromium.org/chromium-os/chromiumos-design-docs/filesystem-autoupdate/).
  - OpenBSD signs releases and packages with signify (Ed25519) — [signify(1)](https://man.openbsd.org/signify).
- **Flatpak sandboxing depends on Linux namespaces** (bubblewrap), so Flatpak/Snap-style stores do not transfer to a non-Linux kernel — (bk) [bubblewrap](https://github.com/containers/bubblewrap).

### Inferences
- **Suggested sequence for Terminal OS** (builds on toolchain/ports/build.sh and the Docker cross-toolchains):
  1. A machine-readable recipe format: version, source hash, patches, dependencies, licence. 2-4 weeks.
  2. CI bulk builds producing binary packages. 1-2 months.
  3. Port xbps, or write a small installer over the existing HTTPS client, with a signed repository index (minisign/signify-style Ed25519 keys embedded in the OS image). 1-2 months.
  4. System updates as A/B system partitions or images with boot counting and automatic fallback in the UEFI loader, or Haiku-style activation states, keeping /home separate. 1-3 months.
  5. A GUI "store" front end over the package index. 1-2 months.
- Defer Nix (heavy C++ and sandboxing expectations) and Flatpak (Linux namespaces).
- Record each port's licence in its recipe so the distribution can meet GPL source-offer obligations (C17).

### Gaps
- HaikuPorts recipe counts, xbps portability requirements and pkgsrc's minimum platform prerequisites were not fetched.
- No data was found on how long Astral took to adopt xbps.

## Q7. Windows app compatibility: porting Wine, the ReactOS approach, feasibility and value

### Takeaway
Wine is the only realistic route. A skilled developer got 64-bit Windows apps running on Haiku within months once POSIX was solid (Jan 2022, with a native display driver and no X11). Astral already lists Wine among its ports alongside X.org. ReactOS shows that a from-scratch NT reimplementation is a multi-decade effort that is still not production-ready. The value is high (a huge catalogue), but Wine should come after Wayland/X11 and Mesa; games additionally need Vulkan for DXVK/VKD3D.

### Cited Findings
- **Wine on Haiku.**
  - X512's port ran 7-Zip, Inkscape, Paint and Notepad++ in early 2022, 64-bit only, with screen-size detection and focus control not working — [Phoronix](https://www.phoronix.com/news/Wine-On-Haiku-Starts-2022); [Liliputing](https://liliputing.com/2022/01/run-some-windows-apps-on-haiku-operating-system-thanks-to-wine-port.html).
  - It used a native Haiku windowing driver (winehaiku.drv), without X11 or Xlib, handling window drawing, mouse/keyboard input and moves/resizes; focus, Z-order and some position syncing were missing; 32-bit apps might later come via WoW64 — [Haiku forum: porting Wine](https://discuss.haiku-os.org/t/my-progress-in-porting-wine/11741?page=2).
- **Astral (mlibc)** lists "wine" among its ports together with X.org — [Astral README](https://github.com/mathewnd/astral).
- **Wine's Wayland driver.**
  - Wine 9.22 (Nov 2024) enabled the native Wayland driver in default builds — [Phoronix](https://phoronix.com/news/Wine-9.22-Released). It is used only when X11 is unavailable — [GamingOnLinux](https://www.gamingonlinux.com/2024/11/wine-922-released-noting-the-wayland-driver-enabled-in-default-configuration).
  - Wine 11 added Wayland clipboard and drag-and-drop improvements and shaped/color-keyed windows in the "experimental" driver — [PC Gamer](https://www.pcgamer.com/hardware/gaming-on-linux-just-got-a-bump-with-new-wine-11-improvements-thatll-make-for-a-better-proton-on-steamos-too/).
  - Wine 11.11 (June 2026) added layered windows to the Wayland driver — [AlternativeTo](https://alternativeto.net/news/2026/6/wine-11-11-released-with-symcrypt-layered-windows-in-the-wayland-drivers-and-25-bug-fixes/).
  - A secondary report expects the driver to become a reasonable default around Wine 12 (early 2027) — [lavx](https://news.lavx.hu/article/wine-11-11-adds-alpha-modifier-v1-support-to-the-wayland-driver-for-native-surface-opacity).
- **ReactOS.** 0.4.15 (March 2025) was the largest release to date and the first to reach the desktop on amd64, whose builds cannot run 32-bit Windows software — [Phoronix](https://www.phoronix.com/news/ReactOS-0.4.15-Released); [Yahoo Tech](https://tech.yahoo.com/computing/articles/reactos-open-source-windows-just-200448247.html). Vista/7/8 application support is planned for 0.5.0, and "it could be years" (Yahoo Tech).
- **Licence.** Wine is LGPL-2.1+ — (bk) [Wine licensing](https://wiki.winehq.org/Licensing).
- **Wine's address-space reservation (preloader source).** Wine's preloader reserves memory "that is important to Wine" before ld.so loads anything. On x86_64 its `preload_info[]` reserves 0x00010000 + 0x00100000 (`/* DOS area */`) and 0x00110000 + 0x1ffef0000 (`/* low 8G */`), plus a PE executable range set at runtime via `WINEPRELOADRESERVE`. On i386 it reserves the low 64k, the DOS area, 0x00110000-0x68000000 and 0x7f000000-0x7fff0000 (`/* top-down allocations + shared user data */`). The whole file is wrapped in `#ifdef __linux__`, so other OSes need their own mechanism — [Wine loader/preloader.c (GitHub mirror)](https://raw.githubusercontent.com/wine-mirror/wine/master/loader/preloader.c).
- **Xlib alone is not enough.** On Haiku, a forum commenter noted Wine needs more than Xlib emulation — [Xlibe forum thread](https://discuss.haiku-os.org/t/xlibe-an-xlib-x11-compatibility-layer-for-haiku/11692).

### Inferences
- **What Wine needs from Terminal OS** (the address-space item is verified from the preloader source; the rest is inferred from Wine's design):
  - Control of the low 8 GB of the x86-64 address space (0x10000 to about 0x200000000) plus the PE image range. Wine's preloader is Linux-only, so Terminal OS needs an equivalent: either a custom preloader, or a kernel/ld.so policy that never places the stack, ld.so, libraries or the heap in the low 8 GB and honours MAP_FIXED/MAP_FIXED_NOREPLACE there. This directly conflicts with the current "the kernel maps nothing fixed below 4 GiB" policy.
  - Large reservations and mprotect on PE sections.
  - Signals with SA_SIGINFO, ucontext and sigaltstack for structured exception handling.
  - User-controllable segment bases for the TEB/TLS.
  - pthreads plus fast synchronization (futex/eventfd, or an ntsync-like device).
  - A display driver: winewayland.drv once Wayland exists, winex11.drv via X.org/Xwayland as Astral does, or a native driver like winehaiku.drv.
  - An audio driver (OSS-style or custom) and Vulkan/GL for wined3d and DXVK.
- **Effort.** After Wayland or X11 and Mesa exist: 2-4 months for basic 64-bit desktop apps (Haiku precedent), more for WoW64 32-bit apps, and gaming gated by hardware Vulkan (Q3).
- **Do not attempt an NT personality** (ReactOS approach) — it is a multi-decade effort.

### Gaps
- x86-64 TEB/GS-base requirements, and how Wine reserves address space on non-Linux hosts (macOS, FreeBSD) without the preloader, were not verified.
- No 2025-2026 status of the Haiku Wine port, and no description of Astral's Wine maturity, were found.

## Q8. Text, internationalization and accessibility: FreeType/HarfBuzz/ICU/fontconfig, input methods, Unicode/RTL/CJK, accessibility architectures, screen reader

### Takeaway
The text stack is largely "free" by porting FreeType, HarfBuzz, ICU and fontconfig, which are permissive and already required by Ladybird. Input methods need a protocol (Wayland text-input-v3/input-method-v2, or an equivalent in the native window server) plus an engine; fcitx5 and IBus expect D-Bus. Accessibility is where independent OSes are weakest. A small OS should build an accessibility tree into its toolkit, following the AccessKit schema, expose it over IPC, and ship a minimal screen reader on espeak-ng (GPLv3, 100+ languages, a few MB).

### Cited Findings
- **Ladybird requires the full text stack:** harfbuzz (freetype, icu), icu, fontconfig (Linux/BSD/macOS) and woff2. FreeType is held back "to avoid font rendering differences" — [Ladybird vcpkg.json](https://raw.githubusercontent.com/LadybirdBrowser/ladybird/master/vcpkg.json). Ladybird added macOS input method editor support in June 2026 — [Ladybird newsletter](https://ladybird.org/newsletter/2026-06-30).
- **AccessKit** defines "the data structures that represent an accessibility tree, and the trait for handling action requests from assistive technologies". It has adapters for Windows (UIA), macOS (NSAccessibility), Unix (AT-SPI) and Android, and a C API — [AccessKit (docs.rs)](https://docs.rs/crate/accesskit/latest); [lib.rs](https://lib.rs/accesskit).
- **eSpeak NG** is "released under the GPL version 3" or later and "supports more than 100 languages and accents". It is "about few Mbytes", can write WAV files, runs on Linux, BSD, Android, Windows and Mac, and "has been ported to other platforms" — [espeak-ng](https://github.com/espeak-ng/espeak-ng).
- **Reference architectures (all bk).** Orca is GNOME's screen reader and works through AT-SPI — [Orca](https://gitlab.gnome.org/GNOME/orca). AT-SPI2 is the D-Bus-based accessibility bus — [AT-SPI2](https://www.freedesktop.org/wiki/Accessibility/AT-SPI2/). Wayland's text-input-v3 protocol carries IME state between compositor and clients — [text-input-unstable-v3](https://wayland.app/protocols/text-input-unstable-v3).
- **Licences (all bk).** FreeType is dual-licensed under the FTL or GPLv2 — [FreeType license](https://freetype.org/license.html). HarfBuzz uses "Old MIT" — [HarfBuzz COPYING](https://github.com/harfbuzz/harfbuzz/blob/main/COPYING). ICU uses the Unicode licence — [ICU LICENSE](https://github.com/unicode-org/icu/blob/main/LICENSE).

### Inferences
- **Text.** Make UTF-8 the only system encoding. Use HarfBuzz for shaping (Arabic, Hebrew, Indic and CJK), ICU for bidi, line and word breaking and locale data (trim ICU data for size), and FreeType for rasterization. Use a fontconfig-like fallback chain with Noto coverage, including emoji. Estimate: 1-2 months, mostly shared with the Ladybird port; the native toolkit and Terminal must adopt the same stack to get RTL and complex scripts right.
- **Input.** xkbcommon-style keymaps and compose first; then an IME protocol in the window server (pre-edit, commit, cursor rectangle), mirroring text-input-v3 so that Wayland clients work later; then port fcitx5 or a small native engine for CJK and Indic input (needs D-Bus, or a shim). Estimate: 1-3 months.
- **Accessibility minimum viable product** (C15):
  - Every native widget publishes role, name, state and actions to an accessibility service over the existing IPC, using the AccessKit schema so that Rust/C toolkits can reuse adapters.
  - A screen-reader daemon follows focus and speaks through espeak-ng, running as a separate process because of GPLv3.
  - Full keyboard navigation, high-contrast and scaling themes, and a compositor-based magnifier.
  - Estimate: 2-4 months for a basic screen reader.

  This would likely put Terminal OS ahead of most independent OSes in C15.

### Gaps
- I found no evidence of shipping screen readers on Haiku, SerenityOS or Redox, but did not search exhaustively, so treat "weakest area" as provisional.
- No effort data was found for porting fcitx5 or IBus to a non-Linux OS.
- Whether Ladybird exposes an accessibility tree on Linux (AT-SPI) in 2026 was not verified.

## Q9. Fastest route to a "useful daily driver" for a solo developer or small team, and in what order

### Takeaway
The fastest route is a "Linux-shaped userland on your own kernel":
1. Finish mlibc with the Linux ABI and make kernel semantics Linux-compatible enough for unmodified ports, prioritized by Tsai/Loupe data and scored with os-test.
2. Build a ports tree and a self-hosting toolchain.
3. Teach the existing compositor Wayland, with Mesa software rendering.
4. Ship Ladybird, with NetSurf as fallback.
5. Then add hardware GL/Vulkan on Gen12, signed packages with atomic updates, Wine, IME/accessibility and, optionally, a Linux binary personality.

Astral, a one-person mlibc OS, reached X.org, GCC, Wine and OpenJDK in about 2.5 years; Managarm, a team, reached KDE Plasma 6, GNOME apps and Epiphany in 12 years.

### Cited Findings
- **Precedent timelines (single developer):** Astral rewrite from 2023 (initial commit April 2023) to Minecraft 1.20 on OpenJDK by late 2025, with X.org, Wine, GCC, bash, Quake, vim, xbps and WPA2 Wi-Fi — [HN](https://news.ycombinator.com/item?id=45958988); [Astral README](https://github.com/mathewnd/astral).
- **Precedent timelines (teams):**
  - Managarm: 12th anniversary in Oct 2026; KDE Plasma 6, Weston, GTK4/GNOME apps, Epiphany — [managarm.org](https://managarm.org/); [Managarm README](https://github.com/managarm/managarm).
  - Redox: native rustc/cargo (Jan 2026), Node.js and lavapipe Vulkan (Feb 2026), real-hardware boot work (Apr 2026) — [Phoronix Feb 2026](https://phoronix.com/news/redox-os-february); [Phoronix Apr 2026](https://phoronix.com/news/redox-os-april-2026); [lavx](https://news.lavx.hu/article/redox-os-achieves-major-milestone-rust-compiler-and-cargo-now-native).
  - Genode: Intel Gen8 3D in four months — [Genode 17.08](https://genode.org/documentation/release-notes/17.08).
  - Haiku: Wine apps within months on mature POSIX — [Phoronix](https://www.phoronix.com/news/Wine-On-Haiku-Starts-2022).
- **Syscall prioritization data:** the 40, 145, 202 and 272 thresholds — [Tsai et al.](https://www.cs.unc.edu/~porter/pubs/login-fall16.pdf). 20-40 syscalls cover typical server workloads, and about half can be stubbed — [Loupe](https://arxiv.org/html/2309.15996v1).
- **Conformance metric:** os-test publishes per-OS POSIX scores (Redox 73%, Genode 57%) — [Redox results](https://doc.redox-os.org/test-results/os-test/os-test-20250922/); [Genode results](https://genode.org/files/jws/os-test/20260820/html/index.html).
- **Browser timing risk:** Ladybird is still pre-alpha, with alpha planned for 2026 and stable for 2028 — [Ladybird GitHub](https://github.com/LadybirdBrowser/ladybird); [Wikipedia](https://en.wikipedia.org/wiki/Ladybird_(web_browser)).

### Inferences
Recommended order for Terminal OS (month ranges are effort for about one full-time developer, running partly in parallel; calendar time is likely 1.5-2x for a hobby schedule):

| # | Milestone | Effort | Content |
|---|---|---|---|
| 1 | mlibc completion and POSIX semantics (C4) | 0-3 months | Static, then dynamic ld.so. The Q1 checklist: futex, full mmap incl. arbitrary MAP_FIXED, signals with sigaltstack/ucontext, PTYs, AF_UNIX with SCM_RIGHTS, epoll/eventfd/timerfd/signalfd, memfd/shm, /proc/self. os-test in CI. |
| 2 | Self-hosting userland and ports tree (C11, C12) | 2-6 months | bash, coreutils, make, binutils/GCC or LLVM/clang, Python, Git, vim/nano, curl/OpenSSL, zlib/xz/zstd. SDL3 native backend plus a few games/emulators. Recipe format with licences. |
| 3 | Browser (C11) | 3-9 months, overlapping 2 | Ladybird with a native LibWebView frontend and Skia CPU raster. Needs a Rust toolchain target early. NetSurf as an interim browser. |
| 4 | Wayland in the existing compositor and Mesa software (C10) | 4-10 months | libwayland-server, xdg-shell, wl_shm, seat/keymaps; Mesa llvmpipe/lavapipe; Xwayland. Then GTK3/4 and Qt6 apps, LibreOffice-class apps later. Alternatively a Qt QPA plugin if Wayland is deferred. |
| 5 | Packaging and updates (C16) | 6-12 months | Binary packages (xbps or own), Ed25519-signed index, A/B or activation-state updates with loader rollback, simple store UI. |
| 6 | Gen12 hardware acceleration (C10, C14) | 8-18 months | Kernel i915-uAPI subset plus libdrm, giving Mesa iris/anv. Unlocks Ladybird's Vulkan/Skia GPU path, WebGL and Vulkan for DXVK. |
| 7 | Internationalization and accessibility (C15) | Parallel from month 4 | Text stack shared with Ladybird; IME protocol; AccessKit-style tree and espeak-ng screen reader. |
| 8 | Wine (C11) | 12-24 months | winewayland.drv or winex11.drv via Xwayland. |
| 9 | Optional Linux binary personality (C11) | 18+ months | Linux `syscall` entry, static musl then glibc. Proprietary GUI apps only after 4 and 6. |

- **Highest-leverage choices:**
  - Wayland server support in the existing compositor (one protocol, every toolkit).
  - Linux-ABI-faithful kernel semantics (every port gets cheaper, and binary compatibility is kept possible).
  - Choosing Mesa's i915 uAPI as the GPU contract rather than porting Linux DRM: lower long-term maintenance, and no GPL entanglement in the kernel.
- **Licence summary (C17):**
  - Permissive: mlibc, Mesa, libdrm, Wayland, X.org, HarfBuzz, ICU, SDL, Ladybird, Skia, curl, OpenSSL 3 (Apache-2.0).
  - Weak copyleft: GTK, Qt, Wine and FFmpeg (default build) are LGPL; FreeType is FTL/GPLv2.
  - GPL: espeak-ng (GPLv3), NetSurf (GPLv2), GCC and binutils.

  All are fine as separate user-space programs or dynamically linked libraries with source offers. The real licence risk is importing GPLv2 Linux kernel code (DRM core/LinuxKPI-style shims, Wi-Fi drivers) into a non-GPL kernel — contested even for FreeBSD's drm-kmod.
- **Rough category impact:** milestones 1-3 would plausibly move C11 and C12 from early-hobby levels (about 2-3) toward SerenityOS/Haiku territory (about 4-5). Adding 4-6 would approach 5-6, comparable to Managarm or Haiku. Reaching 7+ needs the hardware-accelerated browser, Wine and a maintained binary package repository.

### Gaps
- No public data was found on how many hours or developers Astral or Managarm spent per milestone, so the month ranges are inferences.
- No effort data was found for porting Rust std to a new mlibc OS. Upstream offers only a Tier 3 Managarm/mlibc target without std (see Q4).
- Terminal OS's own licence and contribution model were not examined (other researchers are auditing the codebase).
