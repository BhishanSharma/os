# Terminal OS kernel core audit (boot, memory, scheduling/SMP, processes, syscalls, IPC, signals, time, exceptions, security, power/idle)

Method: read-only audit of the repo at HEAD `3c9f14b` (2026-11-07), by reading the source, docs and git history. Citations are `path:line` (repo-relative) or commit hashes. "Likely bug" means the code reads as described but I did not run it (I was not allowed to build or run anything). The few external baselines are general knowledge and are flagged as such.

## 1. Boot path, UEFI loader and platform setup (C1)

### Takeaway
There are two boot paths. One is the project's own UEFI loader, TERMOS.EFI (about 650 lines, unusually complete for a hobby OS: GOP mode chosen from EDID, installer hand-off). The other is a legacy GRUB Multiboot2 ISO. Both put the kernel at a fixed 1 MiB physical / `0xFFFFFFFF80000000` virtual address with RWX mappings. Nothing is signed or verified, so there is no Secure Boot, no KASLR and no NX. The only recovery options are "one core" (the GRUB entry or the S key) and a kernel-shell fallback.

### Cited Findings
- **Two paths, one hand-off format.** TERMOS.EFI (loader/main.c) builds a Multiboot2 information block itself: cmdline, modules, framebuffer, ACPI RSDP, EFI system table, the merged memory map and the raw EFI map as tag 17. GRUB loads `kernel.bin` with `multiboot2` plus `module2 ramdisk.img.gz` — [loader/main.c:255-334,569-627](loader/main.c), [targets/x86_64/iso/boot/grub/grub.cfg:16-27](targets/x86_64/iso/boot/grub/grub.cfg), [docs/ARCHITECTURE.md:37-49](docs/ARCHITECTURE.md)
- **Loader flow.**
  - Turns off the watchdog, picks the GOP mode (EDID-native, else the largest mode up to 3840 wide) and reads `\EFI\TERMINALOS\KERNEL.BIN`, `RAMDISK.GZ` and `TERMOS.EFI`.
  - Parses PT_LOAD headers (paddr must lie in 1-16 MiB) and scans the first 64 KiB for the `"TERMOS64"` marker plus the 64-bit entry point.
  - Requires 1-6 MiB to be reclaimable after ExitBootServices, and gunzips the RAM disk into 2-4 GiB.
  - Polls for the S key for 1 s (this adds `nosmp`).
  - Builds identity tables for 4 GiB using 2 MiB writable, executable pages, and retries ExitBootServices up to 4 times.
  - A trampoline copied above 16 MiB then runs `cli`, loads CR3, clears CR0.WP, copies the kernel to 1 MiB with `rep movsb`, and jumps to `start64` with rdi=info and esi=MB2 magic.
  - Sources: [loader/main.c:499-654](loader/main.c), [loader/main.c:223-253](loader/main.c), [loader/main.c:388-420](loader/main.c), [loader/main.c:555-567](loader/main.c)
- **No integrity checks.** There is no signature or hash check of KERNEL.BIN: any ELF that contains the marker is loaded. `load_kernel` checks segment bounds but not that the program-header table itself lies inside the file. The loader is built with MinGW, `-fno-stack-protector`, and is unsigned — [loader/main.c:223-243](loader/main.c), [Makefile:440-444](Makefile)
- **No KASLR.** The physical load address (1 MiB) and `KERNEL_VMA` are fixed, and the image must stay below 4 MiB — [targets/x86_64/linker.ld:17-69](targets/x86_64/linker.ld), [src/intf/drivers/addr.h:18-22](src/intf/drivers/addr.h)
- **32-bit GRUB entry (`start`).**
  - Checks only the MB2 magic, that CPUID exists and that long mode exists. It does not check NX, XSAVE and so on.
  - Builds boot tables: identity 4 GiB, a 4 GiB direct map and 1 GiB at KERNEL_VMA, all with 2 MiB pages.
  - Sets CR4.PAE, then EFER.LME (bit 8) only. **EFER.NXE (bit 11) is never set**, and the AP trampoline also sets only LME.
  - Sources: [src/impl/x86_64/boot/main.asm:21-27,37-44,49-101,173-207](src/impl/x86_64/boot/main.asm), [src/impl/x86_64/core/smp_trampoline.asm:41-44](src/impl/x86_64/core/smp_trampoline.asm)
- **FPU/SIMD state.** `long_mode_start` sets CR0.MP, clears EM/TS, sets CR4.OSFXSR|OSXMMEXCPT and runs `fninit`. A search of src/ finds no CR4.OSXSAVE, `xsetbv`, XCR0 or `xsave`, so AVX/AVX2/AVX-512 cannot be used by programs, and the context switch saves only the 512-byte FXSAVE area — [src/impl/x86_64/boot/main64.asm:6-16](src/impl/x86_64/boot/main64.asm), [src/impl/x86_64/sys/task.c:323-325](src/impl/x86_64/sys/task.c)
- **GDT and TSS.**
  - Layout: null, kernel code 0x08, kernel data 0x10, 16-byte TSS 0x18, user code 0x28, user data 0x30, then one TSS per extra CPU.
  - One 8 KiB IST stack serves NMI, #DF and #MC.
  - User code sits below user data, which is the opposite of the order SYSRET needs (user data then user code). This fits with the absence of SYSCALL support (§5).
  - Sources: [src/impl/x86_64/core/gdt.c:16-57](src/impl/x86_64/core/gdt.c), [src/intf/core/gdt.h:7-15](src/intf/core/gdt.h), [src/impl/x86_64/core/exceptions.c:244-249](src/impl/x86_64/core/exceptions.c)
- **Platform bring-up in `kernel_main`.** The order is:
  1. serial, framebuffer
  2. ACPI RSDP, EFI runtime info
  3. cmdline flags `nogpu`, `nosmp`, `verbose`
  4. memory map, RAM disk placement, heap
  5. GDT, IDT, `paging_cpu_protect`
  6. 8259 PIC, PIT, `paging_init`
  7. NIC/Wi-Fi, mounts, `sti`, `timer_check`, MSI, PCI registry, GPU, USB, NVMe, AHCI, sound, touchpad, ACPI power (uACPI), DHCP, cpupower
  8. `task_init`, `smp_init`, `shell_run`

  Everything runs on one sequential path, drivers included, before the scheduler exists — [src/impl/kernel/main.c:254-503](src/impl/kernel/main.c)
- **Interrupt controllers.**
  - Legacy 8259 PIC plus PIT are primary. Fallbacks: virtual-wire LINT0, then a LAPIC periodic timer calibrated against RTC seconds.
  - The local APIC is used for MSI, IPIs and per-CPU timers. x2APIC is used only if the firmware already enabled it; the kernel never switches to it.
  - Sources: [src/impl/kernel/main.c:320-333](src/impl/kernel/main.c), [src/impl/x86_64/drivers/apic.c:76-95,141-214](src/impl/x86_64/drivers/apic.c)
- **EFI runtime services** (boot menu, reset) are called on a separate copied address space with identity maps — [src/impl/x86_64/drivers/paging.c:350-418](src/impl/x86_64/drivers/paging.c), [docs/ARCHITECTURE.md:90-91](docs/ARCHITECTURE.md)
- **Recovery and safe mode.**
  - GRUB has a "Terminal OS (one CPU core)" entry, and TERMOS.EFI offers only the S key within 1 s.
  - The kernel accepts `nogpu` and `verbose`, but TERMOS.EFI writes only `termos`, `bootpart=`, `live` and `nosmp`, so those flags are unreachable on the main boot path.
  - pid 1 falls back to the in-kernel shell if SHELL.ELF is missing or crashes.
  - Sources: [targets/x86_64/iso/boot/grub/grub.cfg:22-27](targets/x86_64/iso/boot/grub/grub.cfg), [loader/main.c:345-384,555-567](loader/main.c), [src/impl/kernel/main.c:273-275](src/impl/kernel/main.c), [docs/USERSPACE.md:74-75](docs/USERSPACE.md)
- **Early faults.** Faults before `idt_init()` are still triple faults, and the 32-bit boot code reports errors only as VGA `ERR:` letters — [docs/ARCHITECTURE.md:140-141](docs/ARCHITECTURE.md), [src/impl/x86_64/boot/main.asm:166-171](src/impl/x86_64/boot/main.asm)

### Inferences
- Secure Boot must be off on real PCs: the PE is unsigned and there is no shim or MOK path.
- Measured boot, TPM, a signed or verified kernel, A/B or rollback boot entries and a real rescue mode are all absent. Compared with Linux (shim + GRUB/systemd-boot, KASLR, NX from the first page tables) or Windows (Secure Boot, BitLocker, WinRE), C1 is early-hobby level, though better than average in loader polish.
- AMD and non-Intel systems boot, but get HLT idle and no HWP (§3).
- Hardware-wise, the code assumes an x86_64 PC with a 1-6 MiB hole, a 4 GiB identity map, a PIC and a PIT, and legacy CMOS.

### Gaps
- I could not test Secure Boot behaviour or the breadth of real hardware. The docs mention QEMU/OVMF, VirtualBox and one Alder Lake laptop ([docs/ROADMAP.md:68-70](docs/ROADMAP.md)).
- `efi.c` runtime-service calls were not audited line by line.
- (Resolved:) a search of all of src/ for `ioapic`/`0xFEC00000` finds nothing. Legacy IRQs go only through the 8259 PIC, and PCIe devices use MSI/MSI-X.

## 2. Memory management (C3)

### Takeaway
The basics are in place: 4-level paging, a higher-half kernel, demand paging, copy-on-write fork, partial munmap, anonymous/file/shm mmap, and SMEP/SMAP/WP. Protection, sharing and scalability are mostly missing:
- no NX or W^X anywhere
- no mprotect, so mlibc silently no-ops it
- no ASLR or KASLR
- no page cache or shared file pages
- no swap or OOM killer
- a fixed 256 MiB kernel heap
- unguarded kernel stacks
- linked-list regions and bitmap frames with linear lookups

### Cited Findings
- **Address-space layout.**
  - Lower half (128 TiB) is the program's. `DIRECT_MAP` `0xFFFF800000000000` holds all RAM; `MMIO_WINDOW` is `0xFFFFC00000000000`; there is an ACPI window; `KERNEL_VMA` is `0xFFFFFFFF80000000`. Every program's PML4 shares entries 256-511.
  - Programs are linked at 4 MiB. ld.so sits at the fixed `0x7E0000000000`, mmaps grow down from `0x7F0000000000`, and an 8 MiB stack region ends at USER_TOP.
  - Sources: [src/intf/drivers/addr.h:1-34](src/intf/drivers/addr.h), [docs/ARCHITECTURE.md:73-91](docs/ARCHITECTURE.md), [src/impl/x86_64/sys/process.c:49-51,99](src/impl/x86_64/sys/process.c), [src/impl/x86_64/sys/vm.c:12](src/impl/x86_64/sys/vm.c)
- **Physical frames (`physmem.c`).**
  - One bitmap per 2 MiB-aligned chunk (at most 64 chunks), a 16-bit refcount per page (for COW and shm), a linear `chunk_of()` on every free/ref, and RAM below 16 MiB kept out.
  - The memory map is truncated to 32 regions (`MAX_REGIONS`), so a fragmented map loses RAM.
  - There is no buddy allocator, no contiguous multi-page allocation, no zones and no NUMA.
  - Sources: [src/impl/x86_64/drivers/physmem.c:14-126](src/impl/x86_64/drivers/physmem.c), [src/impl/x86_64/drivers/memory.c:6-28](src/impl/x86_64/drivers/memory.c)
- **Kernel page tables.**
  - They come from a fixed 2 MiB pool at 4-6 MiB, and exhausting it is a `kpanic`.
  - The MMIO window is a bump allocator that is never reclaimed.
  - User page tables and heap-backed user pages use `alloc_frame()`, which `kmalloc`s 8 KiB to get one aligned 4 KiB frame: about 50% waste.
  - Sources: [src/impl/x86_64/drivers/paging.c:19-24,44-51,83-89](src/impl/x86_64/drivers/paging.c), [src/impl/x86_64/drivers/memory.c:79-92](src/impl/x86_64/drivers/memory.c)
- **Kernel heap.**
  - A fixed physical region below 1 GiB (because NIC DMA uses 32-bit addresses), capped at `HEAP_MAX` = **256 MiB**. ARCHITECTURE.md still says "at most 1 GiB".
  - Small requests use size classes of 16-2048 bytes, carved from 64 KiB chunks that never go back to the big heap. Big requests use a first-fit list whose free does an O(n) search for the previous block.
  - `kfree` silently ignores unknown pointers and double frees, and there are no redzones, poisoning or KASAN-like checks.
  - There is no vmalloc or dynamic growth.
  - Sources: [src/impl/kernel/main.c:66-75,292-304](src/impl/kernel/main.c), [src/impl/x86_64/drivers/heap.c:1-177](src/impl/x86_64/drivers/heap.c), [docs/ARCHITECTURE.md:101](docs/ARCHITECTURE.md)
- **Page-table entry flags.** Only PRESENT, RW, USER, 2MB and a software COW bit (bit 9) exist. There is no NX bit, no global-page bit and no PCID. User pages are mapped with only a writable/not-writable choice — [src/intf/drivers/paging.h:8-13,66](src/intf/drivers/paging.h), [src/impl/x86_64/drivers/paging.c:263-273](src/impl/x86_64/drivers/paging.c)
- **W^X is absent in the kernel too.**
  - The kernel image is mapped `PRESENT|RW`, so text is writable, and the direct map of all RAM is RW+X.
  - The linker emits one RWX LOAD segment, which KNOWN-ISSUES calls cosmetic and leaves for later.
  - Sources: [src/impl/x86_64/drivers/paging.c:129-164](src/impl/x86_64/drivers/paging.c), [targets/x86_64/linker.ld:20-58](targets/x86_64/linker.ld), [docs/KNOWN-ISSUES.md:129-131](docs/KNOWN-ISSUES.md)
- **Regions and faults (`vm.c`).**
  - Regions are kept in an unsorted singly linked list with O(n) `vm_find`. There are three kinds (ANON, FILE, SHM) and `prot` is only READ|WRITE.
  - `vm_fault` maps zeroed, file or shm pages on first touch and resolves COW: the last holder takes the page over, otherwise it is copied and a TLB shootdown follows.
  - Sources: [src/intf/sys/vm.h:14-34](src/intf/sys/vm.h), [src/impl/x86_64/sys/vm.c:43-136](src/impl/x86_64/sys/vm.c)
- **Fork.** All pages are shared COW, except heap-backed pages, which are copied eagerly. Afterwards the parent's TLB is flushed and other cores get a shootdown — [src/impl/x86_64/sys/vm.c:359-402](src/impl/x86_64/sys/vm.c)
- **File mappings.**
  - They are private: each mapping reads its own copy via `vfs_read_at` under `vfs_lock`, using 32-bit offsets.
  - There is no page cache, no MAP_SHARED write-back and no msync, and shared libraries are copied per process, which the ROADMAP admits.
  - Sources: [src/impl/x86_64/sys/vm.c:116-128](src/impl/x86_64/sys/vm.c), [docs/ROADMAP.md:95-96,184](docs/ROADMAP.md)
- **Shared memory.** Named or anonymous objects up to 4 GiB with lazily filled pages and resize — [src/impl/x86_64/sys/vm.c:259-353](src/impl/x86_64/sys/vm.c)
- **munmap** handles whole, head, tail and middle splits — [src/impl/x86_64/sys/vm.c:185-240](src/impl/x86_64/sys/vm.c)
- **mmap address allocation.** A bump-down allocator that never reuses freed virtual address space. A single mapping may be up to 64 TiB ("JavaScript cages") — [src/impl/x86_64/sys/vm.c:157-164](src/impl/x86_64/sys/vm.c)
- **No mprotect.** There is no mprotect, madvise, mlock or msync system call among the 81 numbers ([src/intf/sys/syscall_nums.h](src/intf/sys/syscall_nums.h)). mlibc's `VmProtect` returns 0 and does nothing, so PROT_NONE guard pages and JIT W^X flips silently never happen — [toolchain/mlibc/terminalos/generic/sysdeps.cpp:412-414](toolchain/mlibc/terminalos/generic/sysdeps.cpp)
- **Out of memory.** There is no swap, no compression, no overcommit policy and no OOM killer. A fault that cannot get a page sets `vm_out_of_memory`, and the *faulting* program is ended with "out of memory" — [src/impl/x86_64/sys/vm.c:80-115](src/impl/x86_64/sys/vm.c), [src/impl/x86_64/sys/process.c:698-733](src/impl/x86_64/sys/process.c)
- **Huge pages.** Only the kernel's direct map uses 2 MiB pages; none are offered to user space and there is no THP — [src/impl/x86_64/drivers/paging.c:129-150](src/impl/x86_64/drivers/paging.c)
- **Hardware protections.**
  - CR0.WP is always on. SMEP and SMAP are on when CPUID reports them, on every core.
  - Every interrupt and exception stub runs `clac`. A system call keeps EFLAGS.AC set for its *whole* duration, which KNOWN-ISSUES #3 admits.
  - UMIP is not enabled.
  - Sources: [src/impl/x86_64/drivers/paging.c:295-313](src/impl/x86_64/drivers/paging.c), [src/impl/x86_64/sys/smp.c:350-357](src/impl/x86_64/sys/smp.c), [src/impl/x86_64/core/exc_stubs.asm:29-39](src/impl/x86_64/core/exc_stubs.asm), [src/impl/x86_64/sys/process.c:1285-1290](src/impl/x86_64/sys/process.c), [docs/KNOWN-ISSUES.md:13-16](docs/KNOWN-ISSUES.md)
- **ASLR is absent.** Only ET_EXEC programs are accepted (no PIE), ld.so has a fixed base, and the stack top and mmap base are deterministic — [src/impl/x86_64/sys/process.c:1442-1453,1507-1535](src/impl/x86_64/sys/process.c), [src/impl/x86_64/sys/vm.c:26-29](src/impl/x86_64/sys/vm.c)
- **Guard pages.**
  - Only the boot stack has one (an unmapped page, reported as "KERNEL STACK OVERFLOW").
  - Per-task kernel stacks are 64 KiB `kmalloc` blocks with no guard, so an overflow silently corrupts the heap.
  - User stack overflow past 8 MiB is reported, because the area below is unmapped.
  - Sources: [src/impl/x86_64/boot/main.asm:236-244](src/impl/x86_64/boot/main.asm), [src/impl/x86_64/drivers/paging.c:160-164](src/impl/x86_64/drivers/paging.c), [src/impl/x86_64/sys/task.c:29,91-96](src/impl/x86_64/sys/task.c), [src/impl/x86_64/sys/process.c:705-715](src/impl/x86_64/sys/process.c)
- **TLB shootdown.**
  - Implemented with a global generation counter and IPI vector 0x42, sent to cores running the same root (or to all cores for kernel-half changes). Receivers reload CR3 entirely; cores spinning for the lock answer by polling.
  - The sender **gives up after 1e8 spins**.
  - `map_page`, `unmap_page` and `map_large_page` shoot down all cores on every change to a present entry.
  - This was added recently (commits `7c9c00b`, `1a9840d`, both October 2026).
  - Sources: [src/impl/x86_64/sys/smp.c:292-331](src/impl/x86_64/sys/smp.c), [src/impl/x86_64/drivers/paging.c:104-127,199-213](src/impl/x86_64/drivers/paging.c), [docs/KNOWN-ISSUES.md:26-41](docs/KNOWN-ISSUES.md)

### Inferences
- The MM is closer to an early SerenityOS or ToaruOS stage than to Redox, Haiku or current SerenityOS, which have NX, mprotect/PROT_NONE and ASLR (general knowledge, not re-verified here).
- The absence of NX plus RWX kernel text plus no ASLR makes any memory-corruption bug directly exploitable.
- Missing mprotect and a page cache will also block common runtimes: JIT W^X, guard-paged thread stacks, and shared .so text that saves RAM.
- The fixed 256 MiB heap is a global choke point, because syscall file buffering and pipes come from it (§5, §7).
- The give-up timeout in the TLB shootdown can, in theory, let a slow core keep a stale translation to a freed page.

### Gaps
- I did not measure fragmentation or real memory use. There are no in-kernel stress statistics apart from `meminfo`.
- I did not check whether `alloc_frame` heap pages ever back user memory in normal operation, or only as a fallback when physmem is empty.

## 3. Scheduling, SMP, timers and idle/power plumbing (C2, kernel side of C9 and C14)

### Takeaway
- **Scheduling:** a single global round-robin scan of a 256-slot task table, with no priorities, a 50 ms slice and no kernel preemption.
- **SMP:** up to 16 cores, but under a recursive big kernel lock that nearly every syscall, fault and timer bookkeeping step takes. Device interrupts go to the boot core.
- **Time:** a periodic 1 kHz tick on every core (100 Hz scheduler tick) and a 32-bit millisecond clock.
- **Idle and power:** idle uses MWAIT C6 or HLT; Intel HWP with a fixed balanced preference; no tickless operation, no deep idle and no thermal handling.

### Cited Findings
- **Task table.** Static `tasks[MAX_TASKS=256]`, threads included, with states FREE, READY, SLEEPING, ZOMBIE and BLOCKED. Each task has a 64 KiB `kmalloc` kernel stack and a 512-byte FXSAVE area — [src/intf/sys/task.h:14-47](src/intf/sys/task.h), [src/impl/x86_64/sys/task.c:29-75](src/impl/x86_64/sys/task.c)
- **Scheduler algorithm.**
  - `schedule()` scans all slots from the current task + 1 and wakes expired sleepers in the same O(n) pass.
  - It picks the first READY task that may run on this core. User tasks may run on any core; kernel tasks are pinned.
  - There are no priorities, nice levels, fairness accounting, affinity or load balancing beyond "whoever scans first". `TASK_SLICE` = 5 ticks (50 ms).
  - Sources: [src/impl/x86_64/sys/task.c:285-336](src/impl/x86_64/sys/task.c), [src/intf/sys/task.h:15](src/intf/sys/task.h)
- **Preemption.** Only user code is preempted, at timer ticks (irq0 or the AP timer → `user_check_interrupt` → `task_preempt`). Kernel code runs until it blocks — [src/impl/x86_64/core/irq.asm:84-91](src/impl/x86_64/core/irq.asm), [src/impl/x86_64/sys/process.c:735-749](src/impl/x86_64/sys/process.c), [src/impl/x86_64/sys/task.c:384-401](src/impl/x86_64/sys/task.c), [src/intf/sys/task.h:1-7](src/intf/sys/task.h)
- **Wait primitives.**
  - `task_block(chan, timeout)` and `task_wakeup(chan)` wake every waiter on that channel by scanning the whole table.
  - `kmutex` is a sleeping, recursive lock; `vfs_lock` is one.
  - Many waits are polling loops instead of wakeups: `wait` uses `task_sleep(20)`, waiting for the foreground uses 50 ms, keyboard reads use 10 ms, the stopped state uses 10 ms, and futex waits are capped at 100 ms.
  - Sources: [src/impl/x86_64/sys/task.c:224-272](src/impl/x86_64/sys/task.c), [src/impl/x86_64/sys/process.c:755-760,773-781,1928-1929,2543-2559,2667-2671](src/impl/x86_64/sys/process.c)
- **Context switch.**
  - Saves the callee-saved registers, then does FXSAVE/FXRSTOR on *every* switch (no lazy or XSAVE path), writes FS_BASE with `wrmsr`, and reloads CR3 without PCID.
  - Sets TSS.rsp0 and carries the BKL depth with the task. EFLAGS.AC is restored per task.
  - Sources: [src/impl/x86_64/core/taskswitch.asm:14-31](src/impl/x86_64/core/taskswitch.asm), [src/impl/x86_64/sys/task.c:314-335](src/impl/x86_64/sys/task.c)
- **Big kernel lock (BKL), mechanism.** A CAS spinlock on `bkl_owner` (a CPU index) with a global depth, taken with interrupts off, plus wait statistics (`bklstat`) — [src/impl/x86_64/sys/smp.c:52-290](src/impl/x86_64/sys/smp.c)
- **BKL, coverage.**
  - Taken by every syscall except `uptime` and `getpid`, by `exception_handler`, by the keyboard ISR and by the 100 Hz timer bookkeeping.
  - A user tick takes it only when there is work.
  - It is let go only in `cpu_wait`, `bkl_drop`, `kmutex`/`task_block` switches and on return to user mode.
  - Sources: [src/impl/x86_64/sys/process.c:1270-1293](src/impl/x86_64/sys/process.c), [src/impl/x86_64/core/exceptions.c:163-176](src/impl/x86_64/core/exceptions.c), [src/impl/x86_64/core/isr.c:7-13](src/impl/x86_64/core/isr.c), [src/impl/x86_64/drivers/timer.c:41-57](src/impl/x86_64/drivers/timer.c)
- **BKL, measured cost.** With the browser starting in QEMU, cores waited for the lock about 14% of the time, down from 15-22% before commit `deeb0fd`. ROADMAP: "The scheduler and the allocators need their own locks next"; what still holds the lock does so "because the scheduler does: switching tasks, futex, page faults" — [docs/ROADMAP.md:124-137](docs/ROADMAP.md), commit `deeb0fd` (2026-11-07)
- **BKL, races already found.** A real two-cores-in-the-kernel race (an interrupt between setting the owner and the depth) was fixed only on 2026-11-07, which shows the lock is still young — commit `deeb0fd`; [src/impl/x86_64/sys/smp.c:89-101](src/impl/x86_64/sys/smp.c)
- **Per-CPU data.**
  - Arrays indexed by `cpu_index()`, which reads RDTSCP's TSC_AUX or scans APIC IDs. There is no GS-base per-CPU block and no `swapgs`.
  - Many syscall paths use static globals: `phdr_at`, `interp_base`, `found_program`, `build_stack`'s `static page`, `graphics_owner`, `foreground_pid`. These are safe only under the BKL.
  - Sources: [src/impl/x86_64/sys/smp.c:39-50](src/impl/x86_64/sys/smp.c), [src/impl/x86_64/sys/process.c:70-73,1432,1513,1544,1588](src/impl/x86_64/sys/process.c)
- **SMP bring-up.**
  - Uses the ACPI MADT (at most 64 IDs), INIT-SIPI-SIPI through a trampoline at 0x8000, 64 KiB AP stacks, `MAX_CPUS`=16 and a 0.5 s timeout per core.
  - A core that misses the timeout is "forgotten", but its trampoline parameters and index are reused by the next core while the late core may still start. Its 8 KiB fatal stack is also leaked.
  - Device interrupts all go to core 0; other cores take only their own timer and IPIs.
  - Sources: [src/impl/x86_64/sys/smp.c:349-433](src/impl/x86_64/sys/smp.c), [src/intf/sys/smp.h:8-19](src/intf/sys/smp.h)
- **Timers.**
  - Core 0 uses the PIT at 1 kHz through the 8259. It falls back to the LAPIC (virtual wire), then to the LAPIC periodic timer calibrated against RTC seconds.
  - Other cores use the LAPIC periodic timer at 1 kHz, where every 10th interrupt is a scheduler tick.
  - The kernel tick is 100 Hz. `ms_count` is a 32-bit counter of interrupts, so it wraps after about 49.7 days and loses time while interrupts are off.
  - There is no HPET, no TSC clocksource, no TSC-deadline, no high-resolution timers and no tickless idle.
  - Sources: [src/intf/drivers/timer.h:7-26](src/intf/drivers/timer.h), [src/impl/x86_64/drivers/timer.c:20-68](src/impl/x86_64/drivers/timer.c), [src/impl/x86_64/drivers/apic.c:165-214,295-306](src/impl/x86_64/drivers/apic.c), [src/impl/x86_64/sys/smp.c:335-347](src/impl/x86_64/sys/smp.c), [docs/ROADMAP.md:147-148](docs/ROADMAP.md)
- **Idle.**
  - Each core's idle task peeks for work without the lock, then waits in `cpu_idle_wait`.
  - That uses MONITOR/MWAIT with the deepest of C1, C3 or C6 listed by CPUID leaf 5, and only when ARAT and an invariant TSC are present on a bare-metal Intel CPU; otherwise it uses HLT.
  - ACPI `_CST` and `_LPI` are not used, and the deeper hints C7-C10 are never chosen.
  - Sources: [src/impl/x86_64/sys/task.c:100-131](src/impl/x86_64/sys/task.c), [src/impl/x86_64/drivers/cpupower.c:71-134](src/impl/x86_64/drivers/cpupower.c)
- **P-states.** Intel HWP is turned on with EPP 128 ("balanced"), fixed, with no policy interface. In a VM or on non-Intel CPUs nothing is done; there is no AMD CPPC or legacy P-state code — [src/impl/x86_64/drivers/cpupower.c:54-65,97,114-122](src/impl/x86_64/drivers/cpupower.c), [docs/ROADMAP.md:147-148](docs/ROADMAP.md)
- **Sleep.**
  - "Sleep" means: freeze user tasks at their next return to user mode, mark file systems clean, turn the display off, keep halting.
  - ACPI SCI, EC and power-button handling is *polled* every 50 ms from the keyboard idle hook.
  - There is no S3/S4 and no device D3. Shutdown goes through uACPI S5.
  - Sources: [src/impl/x86_64/drivers/power.c:645-722](src/impl/x86_64/drivers/power.c), [src/impl/x86_64/sys/task.c:274-283](src/impl/x86_64/sys/task.c), [src/impl/kernel/main.c:245-252,499](src/impl/kernel/main.c), [docs/ROADMAP.md:68-78](docs/ROADMAP.md)

### Inferences
- The design resembles Linux 2.0-2.2's BKL era or early SerenityOS's big lock.
- Scalability beyond 2-4 cores is limited by the BKL, the O(n) scans and the global wakeups.
- The 1 kHz tick on every core blocks package C-states and costs battery. ROADMAP itself says sleep lasts "hours, not days" ([docs/ROADMAP.md:76-77](docs/ROADMAP.md)).
- The lack of priorities means the window server, audio and interactive work compete equally with CPU hogs. A 50 ms slice is coarse for UI latency.
- Removing the BKL needs, at least:
  - per-run-queue locks and per-CPU run queues
  - a VM lock per address space plus a PTE-update protocol
  - allocator locks (heap and physmem)
  - removing the static scratch globals in process.c
  - real wait queues
  - locking for drivers that rely on "no preemption + BKL", which task.h:5-7 says need no locks
  - IRQ-safe spinlocks distinct from sleeping mutexes

  That is a multi-month refactor.

### Gaps
- No real-hardware scheduler latency or throughput numbers exist beyond the BKL wait percentage.
- I did not check MSI vector allocation or whether MSI can target other cores (smp.h says everything goes to the boot core).

## 4. Processes, threads, signals, ELF loading and dynamic linking (C4)

### Takeaway
The process model is broad for a 4-week-old core: fork with COW, exec, spawn-as-user, wait with job-control flags, threads with TLS, futexes, process groups and sessions, ptys and an ELF loader that also loads mlibc's `ld.so`. Enough for bash, Vim and Ladybird (per the docs). The semantics are thin, though:
- signals arrive only at syscall exit, with no masks or siginfo
- each thread gets its own pid
- exec fails if the program has more than one thread
- there is no ptrace or any debugging interface, and no resource limits

### Cited Findings
- **Process state.** `process_t` holds the address space, regions, 1024 fd slots plus cloexec flags, 32 signal handlers plus a pending bitmask, cwd, sandbox, pgid/sid, stop state and the kernel-command "tap" pipes. It is shared by its threads, which are tasks — [src/impl/x86_64/sys/process_internal.h](src/impl/x86_64/sys/process_internal.h) (`process_t`, `MAX_FDS 1024`)
- **fork** copies `process_t`, COW-shares memory, takes references on fds and creates the child task from the parent's syscall frame with rax=0 — [src/impl/x86_64/sys/process.c:2384-2429](src/impl/x86_64/sys/process.c)
- **exec**
  - Copies argv and envp (at most 64 strings, 3 KiB), loads the new image into a fresh root and closes cloexec fds.
  - Resets handlers and FS base, swaps roots and rewrites the frame.
  - It refuses with EAGAIN if the program has more than 1 thread. POSIX instead terminates the other threads.
  - Sources: [src/impl/x86_64/sys/process.c:2431-2517](src/impl/x86_64/sys/process.c)
- **spawn** (posix_spawn-like) can start a program as another user. The kernel itself prompts for that user's password on the console unless the caller is root, and only system programs may run that way — [src/impl/x86_64/sys/process.c:1838-1888](src/impl/x86_64/sys/process.c), [src/impl/x86_64/sys/users.c:747-760](src/impl/x86_64/sys/users.c)
- **wait** supports pid or -1, NOHANG, UNTRACED, CONTINUED and FOREGROUND, but is implemented by polling every 20 ms. Orphans are re-parented to pid 1 — [src/impl/x86_64/sys/process.c:1891-1931,649-653](src/impl/x86_64/sys/process.c)
- **Threads.**
  - `SYS_THREAD(entry, arg, stack)` makes a task that shares `process_t`.
  - Every thread takes a new pid from `next_pid`, and `SYS_GETPID` returns the task's own pid. mlibc maps **both** `GetPid` and `GetTid` to `TOS_GETPID`, so **`getpid()` returns a different value in each thread**, which violates POSIX ([toolchain/mlibc/terminalos/generic/sysdeps.cpp:992-993](toolchain/mlibc/terminalos/generic/sysdeps.cpp)).
  - TLS works through `SYS_TCB_SET`, which writes IA32_FS_BASE.
  - There are no clone flags, no thread groups and no per-thread signal state.
  - Sources: [src/impl/x86_64/sys/process.c:2519-2541,1279-1281,1403-1408](src/impl/x86_64/sys/process.c), [src/impl/x86_64/sys/task.c:202-222](src/impl/x86_64/sys/task.c)
- **Futex.**
  - WAIT and WAKE only, keyed by physical address, so it works across processes.
  - WAKE wakes *all* waiters, because the count is ignored.
  - WAIT is capped at 100 ms "so Ctrl+C is noticed", so callers loop. There is no REQUEUE, bitset, PI or robust-list support.
  - mlibc builds timed waits on top of this with a millisecond uptime loop.
  - Sources: [src/impl/x86_64/sys/process.c:2543-2560](src/impl/x86_64/sys/process.c), [toolchain/mlibc/terminalos/generic/sysdeps.cpp:248-260](toolchain/mlibc/terminalos/generic/sysdeps.cpp)
- **Signals, delivery.**
  - Signals 1-31. Each process has a handler table plus a trampoline address.
  - Delivery happens **only at system-call exit**, one signal per exit: the frame is pushed on the user stack and the trampoline is entered.
  - `sigreturn` sanitises the frame: rip must be in user space, cs/ss are forced, rflags is masked (TF and AC are cleared).
  - Default actions: STOP/TSTP/TTIN/TTOU stop, CONT continues, CHLD/CONT/WINCH are ignored, anything else ends the program with 128+sig.
  - Sources: [src/impl/x86_64/sys/process.c:1412-1413,2605-2617,2725-2789](src/impl/x86_64/sys/process.c), [docs/ROADMAP.md:11-13](docs/ROADMAP.md)
- **Signals, what is missing.**
  - There are no masks, no siginfo, no sigaltstack, no SA_RESTART/SA_SIGINFO, no real-time or queued signals and no per-thread delivery.
  - A CPU-bound program with a handler never gets the signal until its next syscall. Kill and default actions do happen at ticks through `kill_code`.
  - mlibc's `Sigprocmask` is a no-op ("no masks: signals arrive at once"), and `kill(pid, 0)` always returns success.
  - Sources: [toolchain/mlibc/terminalos/generic/sysdeps.cpp:1064-1111](toolchain/mlibc/terminalos/generic/sysdeps.cpp), [src/impl/x86_64/sys/process.c:681-696,738-749](src/impl/x86_64/sys/process.c)
- **Job control.**
  - getpgid, getsid, setpgid (with session rules), setsid and getppid.
  - Signals can be sent to process groups. Stop and continue are reported to wait. SIGCHLD is sent only if the parent installed a handler.
  - There is no SIGTTIN for background readers.
  - Sources: [src/impl/x86_64/sys/process.c:2627-2724,629-636](src/impl/x86_64/sys/process.c), [docs/ROADMAP.md:103-108](docs/ROADMAP.md)
- **PTYs.** Pseudo-terminals are local socket pairs with a termios subset: ICANON, ISIG, ECHO, the VINTR/VSUSP/VQUIT/VEOF characters, window size and the foreground pgrp. Ctrl+C, Ctrl+Z and Ctrl+\ become signals to the group — [src/impl/x86_64/sys/process.c:464-520](src/impl/x86_64/sys/process.c), [src/impl/x86_64/sys/process_internal.h](src/impl/x86_64/sys/process_internal.h) (`pty_t`)
- **Debugging interface.**
  - There is none: no ptrace, core dumps, `/proc` or user backtraces. A search of src/ for `ptrace|gdbstub|core dump|backtrace|unwind` finds nothing.
  - The exception gates are DPL 0, so a user `int3` raises #GP and the program is killed. A software debugger therefore cannot even plant breakpoints.
  - Sources: [src/impl/x86_64/core/exceptions.c:244-249](src/impl/x86_64/core/exceptions.c), [src/impl/x86_64/core/idt.c:15-25](src/impl/x86_64/core/idt.c)
- **ELF loader.**
  - Accepts ELF64 x86-64 ET_EXEC programs and ET_DYN only for the interpreter, using PT_LOAD and PT_INTERP. The bounds checks were hardened against wrap-around after an attack test (commit `85468cb`).
  - Programs over 8 MiB are demand-paged from the file; smaller ones are copied eagerly into **RW** pages, so their code is writable.
  - PF_X is ignored, and PF_W is honoured only for file-backed segments.
  - The interpreter is loaded only from the system folder, at the fixed `0x7E0000000000`.
  - The auxv carries only PHDR, PHENT, PHNUM, PAGESZ, ENTRY, RANDOM and BASE: no HWCAP, UID, SECURE or SYSINFO_EHDR.
  - Sources: [src/impl/x86_64/sys/process.c:1428-1535,1559-1562,110-128](src/impl/x86_64/sys/process.c), commit `85468cb`
- **AT_RANDOM entropy.** Its 16 bytes come from `timer_ms() * 2654435761`, which is predictable and seeds libc canaries and pointer guards — [src/impl/x86_64/sys/process.c:1548-1550](src/impl/x86_64/sys/process.c)
- **Dynamic linking.**
  - mlibc's `ld.so`, `libc.so`, `libdl`, `libm`, `libpthread` and `librt` go into the RAM disk's LIB folder, and dlopen works (`ports/dyncheck`, system test `dynamic`).
  - There is no PIE, C++ links libstdc++/libgcc statically, Ladybird stays static, and library pages are copied per program.
  - Sources: [docs/ROADMAP.md:87-96](docs/ROADMAP.md), [Makefile:107-119](Makefile)
- **Resource limits.**
  - The kernel has none: no rlimits, quotas or accounting beyond per-task ticks.
  - Hard caps: 256 tasks (threads included), 1024 fds, 64 argv strings, 16 accounts, 256 ports, 16 KiB pipes.
  - mlibc's getrlimit returns invented values.
  - Sources: [src/intf/sys/task.h:14](src/intf/sys/task.h), [src/impl/x86_64/sys/port.c:11-13](src/impl/x86_64/sys/port.c), [src/impl/x86_64/sys/pipe.c:8](src/impl/x86_64/sys/pipe.c), [src/impl/x86_64/sys/users.c:32](src/impl/x86_64/sys/users.c), [toolchain/mlibc/terminalos/generic/sysdeps.cpp:358-368](toolchain/mlibc/terminalos/generic/sysdeps.cpp)

### Inferences
- Coverage is ahead of most hobby kernels: job control, ptys and dynamic ld.so are things ToaruOS-class systems took years to reach. Semantics are behind SerenityOS (which has ptrace, signal masks, pledge/unveil and coredumps, per general knowledge).
- A fork bomb or thread bomb hits the 256-task cap and starves other users: there are no per-user limits.
- Signals that arrive only at syscall exit break programs that rely on asynchronous SIGALRM, SIGINT or SIGPROF while computing.

### Gaps
- I did not inspect `ports/dyncheck` (out of scope).
- I did not check how `deliver_signals` interacts with the syscall restart semantics expected by bash and Vim.

## 5. Syscall interface and POSIX/mlibc coverage (C4)

### Takeaway
There are 81 custom syscalls (0-80), entered only through `int 0x80` with no SYSCALL/SYSRET and no vDSO. They are non-POSIX in shape: several are multiplexed "op" calls, files open in only three modes, and whole files are buffered in kernel memory. mlibc fills in much of POSIX in user space, some of it by faking success: mprotect, sigprocmask, rlimits and `kill(pid, 0)`. Pointer checks are page-table walks followed by direct dereference, not `copy_from_user`. For scale, Linux x86-64 has roughly 450+ syscalls (general knowledge; `arch/x86/entry/syscalls/syscall_64.tbl`).

### Cited Findings
- **Entry path.** The only entry is the `int 0x80` interrupt gate with DPL 3 (`0xEE`). The stub saves an `exc_frame`, and arguments arrive in rdi, rsi, rdx, r10, r8, r9 with the result in rax. There is no SYSCALL/SYSRET or SYSENTER (no LSTAR/STAR/swapgs anywhere in src/) and no vDSO, so every `clock_gettime` traps — [src/impl/kernel/main.c:329](src/impl/kernel/main.c), [src/impl/x86_64/core/usermode.asm:11-56](src/impl/x86_64/core/usermode.asm), [docs/USERSPACE.md:170-173](docs/USERSPACE.md)
- **The 81 numbers.**
  - `SYS_EXIT`=0 through `SYS_POLL`=80, defined in a 550-line header that is shared with the C library.
  - Many are multiplexers: CONSOLE, GFX, SOUND, SYSTEM, JOB, USOCK, SHMFD, SANDBOX, and the PORT_* family.
  - Sources: [src/intf/sys/syscall_nums.h](src/intf/sys/syscall_nums.h), [src/impl/x86_64/sys/process.c:1295-1414](src/impl/x86_64/sys/process.c), [src/impl/x86_64/sys/smp.c:160-177](src/impl/x86_64/sys/smp.c) (name table)
- **Errors** come back as negative `SYSERR_*` values in rax, which mlibc converts with `to_errno` — [src/impl/x86_64/sys/process.c:1035-1051](src/impl/x86_64/sys/process.c), [toolchain/mlibc/terminalos/generic/sysdeps.cpp:1-40](toolchain/mlibc/terminalos/generic/sysdeps.cpp)
- **User-pointer handling.**
  - `user_range_ok` checks the user-half bounds and overflow, walks the PTEs and faults in missing region pages. The kernel then dereferences the user pointer directly inside a call-wide STAC window.
  - `copy_user_string` calls `user_range_ok` once *per byte*, which means a page-table walk per byte.
  - If user memory disappears mid-call (a sibling thread's munmap), the kernel puts a scratch page there and kills the program with 139.
  - Sources: [src/impl/x86_64/sys/process.c:147-203](src/impl/x86_64/sys/process.c), [src/impl/x86_64/sys/process.c:171-188](src/impl/x86_64/sys/process.c), [docs/KNOWN-ISSUES.md:13-16,43-48](docs/KNOWN-ISSUES.md)
- **What exists in the kernel.**
  - Files: read, write, open (READ, WRITE=create/truncate, APPEND), close, seek, stat, statx, unlink, mkdir, rename, chmod, chown, chdir, getcwd, listdir, readdir.
  - Processes: fork, exec, spawn, wait, kill, sigsend, signal, sigreturn, job.
  - Threads: thread, thread_exit, futex, tcb_set.
  - Memory: mmap, mmap2, mmapat, munmap, sbrk, shmmap, shmfd.
  - Descriptors: pipe, dup, dup2, fdflags (cloexec), fdinfo, poll, pollwait.
  - Sockets and IPC: local sockets with fd passing and ptys (usock), TCP/DNS socket calls, ports.
  - Misc: uptime (ms), time (RTC), sleep (ms), uname, getuser, auth, sandbox, registry, kcommand.
  - Sources: [src/impl/x86_64/sys/process.c:1295-1414](src/impl/x86_64/sys/process.c)
- **What is missing in the kernel.**
  - Memory: mprotect, madvise, msync, mlock.
  - I/O and descriptors: O_RDWR, pread/pwrite, ftruncate (shm only), real fcntl, and a generic ioctl (ptys use usock ops).
  - Paths: the `*at` family, link/symlink.
  - Credentials: umask, setuid/setgid/getgroups.
  - Time: ns-resolution clocks and timers, `nanosleep` below 1 ms.
  - Waiting and events: select/epoll/kqueue/eventfd/timerfd/signalfd.
  - Also absent: getrandom, sched_*, rlimits, ptrace, prctl, seccomp.
  - mlibc emulates the `*at` calls only for AT_FDCWD or absolute paths and returns ENOSYS otherwise.
  - Sources: [src/intf/sys/syscall_nums.h](src/intf/sys/syscall_nums.h), [toolchain/mlibc/terminalos/generic/sysdeps.cpp:841-876](toolchain/mlibc/terminalos/generic/sysdeps.cpp)
- **mlibc fakes.**
  - VmProtect is a no-op returning 0; Sigprocmask is a no-op; Get/SetRlimit return invented values; `Kill(pid, 0)` returns 0 for any pid.
  - ClockGet has millisecond resolution: CLOCK_REALTIME is the RTC at first call plus uptime, and the monotonic clock is the 32-bit `timer_ms`. Sleep rounds up to milliseconds.
  - Sources: [toolchain/mlibc/terminalos/generic/sysdeps.cpp:358-368,412-414,952-984,1064-1111](toolchain/mlibc/terminalos/generic/sysdeps.cpp)
- **File semantics at the syscall layer.**
  - `open` reads files of 1 MiB or less whole into the kernel heap; bigger read-only files are streamed.
  - Writes and appends buffer the **entire file in the kernel heap** (at most 64 MiB per write call) and write it out at `close`.
  - Sizes and offsets are 32-bit, `lseek` past EOF is clamped, and there is no read/write mode.
  - Sources: [src/impl/x86_64/sys/process.c:306-371,814-853,891-908](src/impl/x86_64/sys/process.c), [docs/USERSPACE.md:190](docs/USERSPACE.md)
- **Fast paths.** `SYS_UPTIME` and `SYS_GETPID` are answered before taking the BKL; everything else takes it — [src/impl/x86_64/sys/process.c:1274-1293](src/impl/x86_64/sys/process.c)

### Inferences
- The ABI is bespoke and still changing: numbers 62-80 were added out of order. It cannot run Linux binaries, and porting needs mlibc sysdeps for every new call.
- Silent no-op success for mprotect and sigprocmask is more dangerous than ENOSYS, because programs believe they are protected.
- The `int 0x80` path costs an interrupt-gate round trip plus BKL acquisition per call. With no vDSO, timing-heavy programs such as browsers pay a trap for every clock read.

### Gaps
- I did not count which mlibc sysdeps are implemented natively versus emulated in full (only the key ones above).
- I did not check socket sysdeps (network calls are out of scope).

## 6. IPC: pipes, local sockets, Mach-style ports, shared memory, futexes, events, and the "door" document (C4, C5)

### Takeaway
IPC is varied: BSD-style pipes and local sockets with fd passing and ptys, Mach-style ports with a name service, named and anonymous shared memory, physical-address futexes and `poll`. It is functional but unprotected and copy-heavy:
- shm and socket names are a global namespace with no owner checks
- ports are not sandboxed
- wakeups are global
- 1 MiB port messages are copied twice

`docs/door-adr-0001.md` is **not** an IPC mechanism. It is a UI design-system ADR ("DOOR OS"). There is no Solaris-doors-like primitive.

### Cited Findings
- **Pipes.**
  - A 16 KiB ring with reader and writer counts, blocking or non-blocking. SIGPIPE ends the writer if it has no handler.
  - Every pipe or socket event bumps one global `io_gen` and wakes *all* tasks blocked on it.
  - Sources: [src/impl/x86_64/sys/pipe.c:8-29](src/impl/x86_64/sys/pipe.c), [src/impl/x86_64/sys/process.c:439-462,831-841](src/impl/x86_64/sys/process.c)
- **Local sockets (usock).** A pipe each way, queues of passed fds, and a pty mode. Listeners are found by plain string-compare on the name (at most 32 listeners, backlog 16), with no owner or mode check on names — [src/impl/x86_64/sys/process_internal.h](src/impl/x86_64/sys/process_internal.h) (`usock_t`, `MAX_LISTENERS`), [src/impl/x86_64/sys/syscall_usock.c:142-194](src/impl/x86_64/sys/syscall_usock.c)
- **Ports.**
  - A global table of 256 ports, with at most 64 queued messages per port and messages up to 1 MiB.
  - Rights are RECEIVE, SEND and SEND_ONCE, with 32 rights per process.
  - There are 32 service names. A name belongs to the uid that registered it first, and only that uid or root can move it (commit `da5ab98`).
  - Sending copies user → `kmalloc` buffer → queue; receiving copies back the same way.
  - Sources: [src/impl/x86_64/sys/port.c:11-30,150-175](src/impl/x86_64/sys/port.c), [src/intf/sys/port.h:14-18](src/intf/sys/port.h), [src/impl/x86_64/sys/process.c:2562-2603](src/impl/x86_64/sys/process.c)
- **The sandbox does not cover IPC.** ROADMAP: "ports (Mach IPC) and shared memory are not limited" — [docs/ROADMAP.md:80-85](docs/ROADMAP.md)
- **Shared memory.**
  - `shmmap(name, size)` and `shmfd` NEW/RESIZE/SIZE/UNLINK/EXISTS work on a global namespace of 32-character names, with **no owner, mode or sandbox check**.
  - So any program can map, resize (shrink) or unlink another user's object, for example the window server's surfaces.
  - Sources: [src/impl/x86_64/sys/process.c:2277-2383](src/impl/x86_64/sys/process.c), [src/impl/x86_64/sys/vm.c:259-353](src/impl/x86_64/sys/vm.c)
- **Futexes** are keyed on the physical address, so they wake across processes; the window server relies on this — [src/impl/x86_64/sys/process.c:2546-2550](src/impl/x86_64/sys/process.c), [docs/ROADMAP.md:31-32](docs/ROADMAP.md)
- **Events.**
  - `SYS_POLL` takes up to `POLL_MAX` kernel fds in one call. `pollwait` waits on the io generation, or on `net_wait` for sockets.
  - There is no epoll, kqueue, eventfd or timerfd.
  - Sources: [src/impl/x86_64/sys/process.c:556-594](src/impl/x86_64/sys/process.c)
- **The "door" document.**
  - The ADR's title is "DOOR OS design foundations". It covers tokens, palette, fonts and icons, records the process model as "Static ELF programs, `thread_create` / `pthread_create`, `malloc`, ports + shared memory for IPC", and says "no per-app permission model exists".
  - Sources: [docs/door-adr-0001.md:1-4,15,20](docs/door-adr-0001.md)

### Inferences
- The set of IPC kinds is comparable to what a desktop needs (a window server over shm plus futex, a launchd-like name service).
- The missing access control on shm, socket names and ports undoes the app sandbox and cross-user isolation. A malicious app can read or tamper with other apps' window surfaces.
- Mach-like ports without send-right checks for the name service, without capability transfer of memory, and with double copies are far from XNU's ports or OOL memory, or from Linux's memfd/SCM_RIGHTS performance.

### Gaps
- I did not check whether window-server surfaces use named or anonymous shm. With anonymous shm passed over usock, the exposure is smaller.
- I did not test port-right passing in messages.

## 7. Security model and sample audit of syscall handlers (C5)

### Takeaway
- **What exists:** Unix-like users (16 accounts, one group each, iterated SHA-256 passwords), ext3 owners and modes, a FAT32 home-folder rule, a path-prefix app sandbox with a network flag, SMEP/SMAP/WP, and careful ELF and sigreturn checks added after an internal "attack test" commit.
- **What is missing:** NX/W^X, ASLR/KASLR, stack protectors, Secure Boot, encryption, a memory-hard password KDF and capability-style IPC.
- **What is risky:** a ring-0 kernel shell reachable from any program (`kcommand`), a global `privileged` bypass flag, and several likely DoS bugs.

### Cited Findings
- **Accounts.** PASSWD holds `name:uid:salt:hash:home`, with at most 16 accounts, root as uid 0, users from 1000, and **gid = uid** (no supplementary groups). The kernel's own context counts as root: `user_current()` returns a static `{"root", 0, "/"}` outside programs and sessions — [src/impl/x86_64/sys/users.c:1-14,31-35,410-415](src/impl/x86_64/sys/users.c), [src/impl/x86_64/sys/users.c:432-437](src/impl/x86_64/sys/users.c)
- **Password hashing.**
  - SHA-256 iterated **2000 rounds** over salt and password, with a 16-byte salt from SHA-256(TSC, timer, RTC, RDRAND if present).
  - The comparison is constant-time, and a failed `auth` sleeps 1 s.
  - Root without a password cannot be logged into (commit `9017ca2`).
  - This is far weaker than yescrypt, scrypt or Argon2, and also weaker than SHA-512-crypt's default of 5000 rounds (general knowledge).
  - Sources: [src/impl/x86_64/sys/users.c:34,87-139,153-157](src/impl/x86_64/sys/users.c), [src/impl/x86_64/sys/process.c:2806-2828](src/impl/x86_64/sys/process.c)
- **File permissions.**
  - On ext2/ext3: owners and modes through VFS creds (search on folders, write on the parent, sticky /TMP).
  - On FAT32: root may write anywhere, others only under their home and /TMP. Reads are open to everyone except PASSWD and WIFI.CFG.
  - Sources: [src/impl/x86_64/sys/users.c:395-424](src/impl/x86_64/sys/users.c), [docs/ROADMAP.md:51-66](docs/ROADMAP.md)
- **Global `privileged` counter (risky).**
  - While it is above 0, the write guard, the read guard and `creds()` let **every** task through.
  - It is raised around saving PASSWD and creating home folders, and both do FAT32/ext2 I/O.
  - If that I/O sleeps or drops the BKL, another core's task can pass permission checks in that window. This is a likely race, not proven.
  - Sources: [src/impl/x86_64/sys/users.c:286-343,396-424](src/impl/x86_64/sys/users.c)
- **Sandbox.**
  - Per process, inherited by children, never widened. A sandboxed path may touch /HOME and /USB only under the listed folders; everything else is left to owners and modes.
  - The network is checked only at RESOLVE, CONNECT and LISTEN.
  - Built-in apps are not sandboxed.
  - Sources: [src/impl/x86_64/sys/process.c:1141-1185](src/impl/x86_64/sys/process.c), [src/impl/x86_64/sys/syscall_net.c:73](src/impl/x86_64/sys/syscall_net.c), [docs/ROADMAP.md:80-85](docs/ROADMAP.md)
- **Network sandbox bypass (likely).**
  - `kcommand` runs the in-kernel shell's commands, `download <url>`, `ping` and `wifi` among them, in the caller's context.
  - Neither `sys_kcommand` nor the net stack calls `process_may_network()`, which is called only in syscall_net.c:73. A sandboxed app without `network = yes` can therefore probably still fetch URLs.
  - Sources: [src/impl/x86_64/sys/process.c:2026-2054](src/impl/x86_64/sys/process.c), [src/impl/x86_64/sys/shell.c:274-283,1153-1163](src/impl/x86_64/sys/shell.c)
- **Kernel attack surface.**
  - Through `kcommand`, every program can drive the 1,665-line shell and the editor, script and installer code in ring 0.
  - `crash`, `install`, `reboot`, `clock` and `wifi start` require root, but `shutdown`, `suspend` and `resolution` do not.
  - Sources: [src/impl/x86_64/sys/shell.c:546-589,963-974](src/impl/x86_64/sys/shell.c), [docs/USERSPACE.md:57-63](docs/USERSPACE.md)
- **Compiler hardening.**
  - Kernel: `-ffreestanding -fno-stack-protector -fno-pie -mno-red-zone -mgeneral-regs-only -mcmodel=kernel -O2`, with no `-D_FORTIFY_SOURCE`, CFI, `-fstack-clash-protection`, `-fzero-call-used-regs` or retpolines.
  - Small-libc programs: `-fno-stack-protector -fno-pie`, linked with `-z noexecstack` only, which means nothing without NX. There is no PIE or RELRO.
  - `-Werror` is opt-in (`WERROR=1`).
  - Sources: [Makefile:69-72,141-152,175,189-193,359-360](Makefile)
- **Mitigations present:** SMEP, SMAP, CR0.WP and the AC clearing on interrupts (§2).
- **Mitigations absent:** NX/W^X, ASLR, KASLR, UMIP, KPTI-style isolation, a strong AT_RANDOM, guard pages on kernel stacks (§1, §2, §4).
- **Boot and storage security:** no Secure Boot, TPM or measured boot, and no disk or home encryption (§1; no such code in src/).
- **Syscall-handler audit, sound patterns:**
  - EXEPATH and FDPATH bound `n < len` after a write check — [src/impl/x86_64/sys/process.c:1376-1394](src/impl/x86_64/sys/process.c)
  - `poll` bounds `n` before multiplying — [src/impl/x86_64/sys/process.c:558-560](src/impl/x86_64/sys/process.c)
  - Port sizes are capped at PORT_MSG_MAX — [src/impl/x86_64/sys/process.c:2570-2593](src/impl/x86_64/sys/process.c)
  - Futex checks alignment and range — [src/impl/x86_64/sys/process.c:2544-2550](src/impl/x86_64/sys/process.c)
  - `sigreturn` sanitises rip, cs, ss and rflags — [src/impl/x86_64/sys/process.c:2778-2789](src/impl/x86_64/sys/process.c)
  - `sound` checks `count ≤ 2^20` before `count*4` — [src/impl/x86_64/sys/process.c:933-934](src/impl/x86_64/sys/process.c)
  - spawn and exec copy argv into kernel buffers once — [src/impl/x86_64/sys/process.c:1842-1861,2436-2448](src/impl/x86_64/sys/process.c)
  - kill and sigsend check the uid — [src/impl/x86_64/sys/process.c:1933-1941,2734](src/impl/x86_64/sys/process.c)
  - The ELF loader avoids wrap-around (commit `85468cb` fixed a heap disclosure and a write to physical page 0, plus a double fetch in SYSTEM_LOG).
- **Audit finding: likely hang (integer overflow).** `file_grow` doubles a `uint32_t` capacity, `while (cap < need) cap *= 2`. For `need > 2^31` the capacity wraps to 0 and the loop never ends, in the kernel with the BKL held. Opening an existing file over 2 GiB with OPEN_APPEND calls `file_grow(f, size + 1)` — [src/impl/x86_64/sys/process.c:236-247,323-339](src/impl/x86_64/sys/process.c)
- **Audit finding: kernel-heap exhaustion by any user.** File writes buffer the whole file in the shared kernel heap (at most 256 MiB): 64 MiB per call, unbounded in total, with capacity doubling. Pipes and port messages also use `kmalloc` — [src/impl/x86_64/sys/process.c:849](src/impl/x86_64/sys/process.c), [src/impl/kernel/main.c:73](src/impl/kernel/main.c)
- **Audit finding: likely BKL leak via #DB.**
  - `exception_handler` takes the BKL and *returns* for vectors 1 and 3 without `bkl_leave`.
  - A program can set EFLAGS.TF itself (`popf`), which raises #DB after every instruction: a `kprintf` per instruction and a leaked lock level each time.
  - On SMP the BKL then stays held while the program runs in ring 3, so other cores stall on their next kernel entry. This needs a test to confirm.
  - Sources: [src/impl/x86_64/core/exceptions.c:163-169](src/impl/x86_64/core/exceptions.c)
- **Audit finding: TOCTOU and double fetch by design.** Handlers dereference user memory directly after `user_range_ok`, inside the STAC window. Correctness relies on each handler reading each value once, which KNOWN-ISSUES #3 acknowledges. Write permission is checked at `open`, but the data is written at `close` through the stored path, possibly from the exit path — [src/impl/x86_64/sys/process.c:147-161,320,352-366](src/impl/x86_64/sys/process.c), [docs/KNOWN-ISSUES.md:13-16](docs/KNOWN-ISSUES.md)

### Inferences
- The security model matches a single-user hobby desktop: it is honest about its cuts and has some real hardening (SMEP/SMAP, loader and sigreturn checks, attack tests).
- Against Linux, Windows or macOS it lacks essentially every exploit mitigation and boot-chain protection.
- The sandbox is advisory while `kcommand`, shm and ports are unrestricted.
- Fastest wins: NX plus W^X, a stack protector, proper RDRAND/RDSEED entropy for AT_RANDOM and salts, Argon2id or yescrypt, and gating `kcommand` behind root or the sandbox.

### Gaps
- I did not trace whether disk I/O under `privileged++` actually releases the BKL, which would confirm the race.
- I did not exercise the #DB and `file_grow` paths.
- I did not audit `sys_gfx` (graphics) or `sys_system` handlers in depth.

## 8. Robustness and debuggability

### Takeaway
- **Good for a hobby kernel:** a panic screen written without the heap or `kprintf` (to VGA and COM1, with a decoded fault, registers and an addr2line hint), IST stacks, a boot-stack guard page, serial mirroring and a QEMU system-test suite.
- **Missing:** backtraces, symbols, crash dumps, a persistent kernel log, a kernel debugger, stopping other CPUs on panic and runtime assertions.
- **Debt markers:** zero TODO/FIXME/HACK in the source; debt is tracked in the docs instead.

### Cited Findings
- **Panic screen.**
  - Shows the vector name and mnemonic, decodes #PF and #GP, detects stack overflow via the guard page, prints all GPRs, CR0/CR3/CR4, 6 stack words, uptime, and an `addr2line` hint.
  - A recursive fault halts. #DB and #BP "report and resume". A user-mode fault kills only the program.
  - Sources: [src/impl/x86_64/core/exceptions.c:1-250](src/impl/x86_64/core/exceptions.c), [docs/ARCHITECTURE.md:125-141](docs/ARCHITECTURE.md)
- **What a panic does not do.** It does not send IPIs or NMIs to other cores; they keep running until they need the BKL. There is no unwinder or symbolizer and no frame-pointer flag in CFLAGS. There is no kdump or persistent panic record — [src/impl/x86_64/core/exceptions.c:121-128](src/impl/x86_64/core/exceptions.c), [Makefile:141-152](Makefile)
- **Assertions.**
  - `kpanic` has only 8 call sites in all of src/: page-table pool exhausted, `map_page` inside a 2 MiB page, `copy_to_space` unmapped, idle-task allocation (×2), a finished task scheduled again, the manual shell panic, and the definition itself.
  - There is no runtime `assert` or `BUG_ON` (only `_Static_assert`). Several invariants fail silently instead: `kfree` on a bad pointer, a TLB shootdown timeout, a late AP.
  - Sources: grep of src/ (this audit), [src/impl/x86_64/drivers/heap.c:140-161](src/impl/x86_64/drivers/heap.c), [src/impl/x86_64/sys/smp.c:325-330,423-429](src/impl/x86_64/sys/smp.c)
- **Logging.**
  - All `kprintf` output is mirrored to COM1, at 115200 baud, polled.
  - A 16 KiB boot log (`dmesg`) is kept only until boot ends.
  - `logd` keeps logs on the boot stick at /USB/LOGS.
  - Sources: [src/impl/x86_64/lib/print.c:84-89,337,897](src/impl/x86_64/lib/print.c), [src/impl/kernel/main.c:125-128](src/impl/kernel/main.c)
- **Debuggers.** `make debug` boots the *legacy ISO* in QEMU with `-s -S` (a gdb stub on :1234), and `DEBUG=1` builds with `-g -O0`. There is no in-kernel gdbstub or kdb and no ptrace (§4) — [Makefile:181-187,497-504](Makefile)
- **Tests.**
  - Host unit tests run with ASan and UBSan for net, TCP, WPA and ext2, but none for kernel-core MM or the scheduler.
  - QEMU system tests drive the OS (`tests/qemu/run_tests.py`, "16 of 16 pass" in `85468cb`), including vmtest (fork while a thread writes on another core, a buffer unmapped during `read()`: commit `833415c`) and the attack tests.
  - Sources: [Makefile:510-551](Makefile), commits `85468cb`, `833415c`
- **Debt markers.** There are 0 TODO/FIXME/HACK/XXX markers in src/, loader/ and targets/; the only matches are in a binary font. Debt is listed in ROADMAP "Cuts" and KNOWN-ISSUES — grep (this audit), [docs/ROADMAP.md:3-148](docs/ROADMAP.md), [docs/KNOWN-ISSUES.md:6-22,121-135](docs/KNOWN-ISSUES.md)
- **KNOWN-ISSUES items for the kernel core.**
  - #3: direct user dereference inside the SMAP window.
  - The RWX LOAD segment.
  - `isr_timer(registers_t regs)` takes a struct by value, which the code still does at [src/impl/x86_64/drivers/timer.c:42](src/impl/x86_64/drivers/timer.c).
  - Compiler warnings remain, and CI has no WERROR.
  - Faults before `idt_init` triple-fault.
  - The build container is not reproducible.
  - Sources: [docs/KNOWN-ISSUES.md:13-16,121-135](docs/KNOWN-ISSUES.md)

### Inferences
- Diagnosing field crashes on real hardware depends on serial access or a photo of the screen. With SMP, the lack of panic-time CPU stop and backtraces will make heisenbugs (BKL and TLB races) hard to root-cause.
- Writing debt into the docs instead of the code is disciplined, but it also means the tracking is not tied to the lines it describes.

### Gaps
- I did not inspect `tests/qemu/run_tests.py` coverage (out of scope; the CI researcher covers it).

## 9. Architecture, code quality, portability, size, documentation accuracy and history

### Takeaway
- **Design:** a monolithic, single-address-space kernel with *everything* in ring 0: drivers, TCP/IP, BearSSL TLS with CA anchors, uACPI, GPU and Wi-Fi WPA2, plus a shell, editor, script interpreter, installer and a toy C compiler.
- **Size:** the kernel core proper is about 12k lines, with a 2,949-line `process.c` god-file.
- **Portability:** x86 details leak into "generic" sys/ code, so an ARM64 or RISC-V port is a large refactor.
- **Docs:** ROADMAP.md is accurate; ARCHITECTURE.md and USERSPACE.md are stale in several places.
- **History:** the multitasking, SMP and VM core is only about 4 weeks old and heavily AI-assisted.

### Cited Findings
- **Everything is in ring 0.**
  - The kernel links uACPI and BearSSL.
  - In-kernel non-core code: `sys/shell.c` 1,665, `editor.c` 366, `script.c` 231, `install.c` 418 and `lib/compiler.c` 676 lines, for 3,684 lines in total with sysinfo and system.
  - TLS trust anchors take 8,380 lines.
  - Sources: [Makefile:139,207-212,286-288](Makefile), file sizes (this audit), [docs/ARCHITECTURE.md:161-167](docs/ARCHITECTURE.md)
- **Lines of code** (`wc -l`, this audit):

  | Part | Lines |
  | --- | --- |
  | boot asm | 304 |
  | UEFI loader | 1,123 |
  | core (GDT/IDT/exceptions/stubs/trampoline/MB2) | 1,282 |
  | `kernel/main.c` | 503 |
  | memory management (paging, heap, memory, physmem, vm) | 1,237 |
  | scheduling/SMP/time/idle (task, smp, timer, apic, pic, msi, cpupower, switch and trampoline asm) | 1,872 |
  | processes/syscalls/IPC (process.c, process_internal.h, usock, net syscalls, pipe, port, usermode.asm) | 3,809 |
  | `users.c` | 795 |
  | core headers | ~1,070 |
  | **kernel core total** | **~12k** |
  | all of `src/impl` C+asm | 42,294 |
  | of which drivers | 17,170 |
  | of which net | 2,189 |
  | of which lib | 3,054 |
- **Hotspot.**
  - `process.c` (2,949 lines) mixes: about 80 syscall handlers, the ELF loader, the keyboard line discipline, ptys, sound ownership, graphics (`sys_gfx`, lines 2084-2261), the sandbox, signals, job control and auth.
  - It includes 37 headers, among them igfx, hda, iwlwifi, wifi, power, efi, fat32, tcp, net, mouse and fbcon.
  - It was touched by 50 of 146 commits.
  - Sources: [src/impl/x86_64/sys/process.c:9-47,2084-2261](src/impl/x86_64/sys/process.c), `git log -- src/impl/x86_64/sys/process.c`
- **The intf/impl split is only partly honoured.**
  - Memory-management headers live under `src/intf/drivers/` (paging.h, heap.h, memory.h, physmem.h).
  - The "portable" sys/ code lives under `src/impl/x86_64/sys/` and contains inline asm: process.c 7, task.c 7, smp.c 21, vm.c 5 and users.c 3 sites.
  - That asm covers FS_BASE `wrmsr`, `fxsave`, RDTSCP, `invlpg`, and raw x86 PTE masks (`0x000FFFFFFFFFF000`) in vm.c.
  - `struct exc_frame` (the x86 trap layout) doubles as the generic fork, thread, signal and exec frame.
  - Sources: [src/impl/x86_64/sys/vm.c:89-102](src/impl/x86_64/sys/vm.c), [src/impl/x86_64/sys/task.c:323-328](src/impl/x86_64/sys/task.c), [src/intf/core/exceptions.h:6-12](src/intf/core/exceptions.h)
- **x86 platform assumptions:** PIC/PIT/CMOS/IMCR, no IOAPIC (none in src/), an identity-mapped low MiB and an AP trampoline at 0x8000, TSO memory ordering (volatile flags without barriers in the BKL and TLB code) — [src/impl/x86_64/drivers/apic.c:141-163](src/impl/x86_64/drivers/apic.c), [src/impl/x86_64/sys/smp.c:15,389-396](src/impl/x86_64/sys/smp.c)
- **ARCHITECTURE.md is stale.** It claims:
  - PIT at 100 Hz; the code runs a 1 kHz hardware timer.
  - A heap "at most 1 GiB" with "first-fit... 8-byte alignment"; the code caps it at 256 MiB and uses 16-byte size classes.
  - A frame allocator that is "barely used"; physmem is now the main allocator.
  - RTL8139 with "no TX"; this was fixed.
  - The shell as "one big if/else chain".
  - A drivers table missing about 25 drivers.
  - Sources: [docs/ARCHITECTURE.md:25-27,62-63,101,150-167](docs/ARCHITECTURE.md)
- **USERSPACE.md is stale.** It claims "up to 14 programs", "no pipes... no exec", a "256 KiB stack" and "no floating point". The code has 256 tasks, pipes, exec and an 8 MiB stack, and mlibc ports use SSE. Its statements that kernel code is never preempted and that syscalls use `int 0x80` are still true — [docs/USERSPACE.md:149-173,185-190](docs/USERSPACE.md)
- **ROADMAP.md matches the code**, with two cuts now resolved: "no TLB shootdown" was fixed in §2, and "no TLS" was fixed through `fs_base`. It is the most reliable document — [docs/ROADMAP.md:11-13](docs/ROADMAP.md), [src/impl/x86_64/sys/task.c:326-328](src/impl/x86_64/sys/task.c)
- **History.**
  - First commit 2025-10-02. There were 27 commits in Oct 2025, then a gap, then 104 in Oct 2026 and 15 in Nov 2026.
  - First appearances: `process.c` 2026-10-09 (`d6b866e`), `task.c` and `smp.c` 2026-10-10, `vm.c` 2026-10-11, `loader/main.c` 2026-10-11.
  - 63 of 146 commits carry `Co-Authored-By: Claude` (from `a1c53a0`, 2026-10-09).
  - Authors: bhishan-genoma 111, BhishanSharma 27, alan-w-arch 8.
  - Sources: `git log`, `git shortlog -sn HEAD` (this audit)

### Inferences
- An ARM64 or RISC-V port would need:
  - an arch layer for traps and frames, context switch, FPU state, TLS register, PTE format and TLB maintenance (vm.c and paging.c are x86-specific)
  - interrupt controllers (GIC, PLIC) and timers (generic timer, CLINT)
  - boot via UEFI on ARM plus DT or ACPI
  - SMP bring-up (PSCI, SBI)
  - explicit memory barriers

  A rough estimate is 3-6 person-months for a first kernel-core port, more for drivers.
- The very young age of the core (weeks) means little soak time on real hardware. Rapid AI-assisted change raises the risk of latent concurrency bugs; the 2026-11-07 BKL race fix is an example.

### Gaps
- I counted lines only (no complexity metrics). I did not run the build to see warning counts.

## 10. Preliminary scores (C1, C2, C3, C4, C5; kernel side of C9 and C14) and prioritized kernel-core gaps

### Takeaway
On the shared 0-10 scale, the kernel core sits at **2-4: "works in emulators and on a few real machines along the happy path, with major gaps"**. It scores above typical hobby kernels in process and POSIX breadth (it hosts bash, Vim and Ladybird via mlibc). Security mitigations and SMP scalability are its weakest points. To reach the 5-6 band (SerenityOS, Haiku, Redox), the core needs roughly 1-1.5 person-years of focused work, ordered by the list below.

### Cited Findings
- **C1 Boot, firmware and platform: 3.**
  - For: its own UEFI loader with EDID mode choice and installer hand-off, plus GRUB; runs on QEMU/OVMF, VirtualBox and an Alder Lake laptop.
  - Against: no Secure Boot, signing, KASLR, NX, IOAPIC or real boot menu or recovery (S key and `nosmp` only); a fixed physical layout; faults before the IDT triple-fault.
  - Evidence: §1, [docs/ROADMAP.md:68-70](docs/ROADMAP.md)
- **C2 Kernel architecture, scheduling and SMP: 3.**
  - For: SMP on up to 16 cores with TLB shootdown and per-core idle outside the lock.
  - Against: a BKL on almost everything (about 14% lock wait in QEMU with the browser); a global O(n) round-robin with no priorities and a 50 ms slice; no kernel preemption; no per-CPU run queues, GS per-CPU or IRQ balancing; a monolithic kernel with the shell and compiler in ring 0.
  - Evidence: §3, §9
- **C3 Memory management: 3.**
  - For: higher half, demand paging, COW fork, partial munmap, file, anon and shm mmap, SMEP/SMAP/WP.
  - Against: no NX/W^X, mprotect, ASLR, page cache, swap or OOM killer; a fixed 256 MiB heap; a 2 MiB kernel page-table pool; unguarded kernel stacks; linear data structures.
  - Evidence: §2
- **C4 Processes, threads, IPC, signals and syscall/POSIX surface: 4.**
  - For: fork, exec, spawn, wait, threads with TLS, futex, job control, ptys, pipes, local sockets with fd passing, ports, shm, poll, dynamic ld.so; enough for bash, Vim and Ladybird.
  - Against: 81 bespoke `int 0x80` calls; signals only at syscall exit and with no masks; a pid per thread; ms-resolution clocks; no ptrace or rlimits; whole-file buffering; silent mlibc fakes.
  - Evidence: §4-§6
- **C5 Security and privacy: 2.**
  - For: users with salted iterated SHA-256, ext3 modes, a path-prefix sandbox, SMEP/SMAP, attack tests.
  - Against: no NX, ASLR, KASLR, stack protector, Secure Boot or encryption; weak entropy; `kcommand` exposing a ring-0 shell (a likely network-sandbox bypass); shm, socket names and ports unprotected; a global `privileged` bypass; likely DoS bugs (#DB BKL leak, `file_grow` hang, heap exhaustion).
  - Evidence: §7
- **C9 Power and thermal, kernel side: 2.5.**
  - For: Intel HWP with balanced EPP; MWAIT C6 idle on bare-metal Intel; freeze-based "sleep" that works on one laptop.
  - Against: a periodic 1 kHz tick on every core (no tickless); no ACPI `_CST`/`_LPI`, so C7-C10 are never used; no AMD CPPC; no thermal zones or throttling; ACPI events polled every 50 ms; no S3/S4 or device D3.
  - Evidence: §3
- **C14 Performance and efficiency, kernel side: 2.5.**
  - For: an `-O2` build, size-class kmalloc, a lock-free uptime/getpid fast path, BKL measurement tooling.
  - Against:
    - Locking: the BKL, O(n) scheduler and wakeup scans, a global `io_gen` thundering herd, futex wake-all with 100 ms polling.
    - Syscalls: the `int 0x80` path with no vDSO; a per-byte page walk in `copy_user_string`.
    - Context switches: FXSAVE on every switch, no XSAVE so no AVX in user space, full CR3 flushes with no PCID.
    - Memory and I/O: no page cache, so shared libraries are copied per process; whole-file buffering.
    - Waiting: many `task_sleep` polling loops.
  - Evidence: §3-§6

**Prioritized kernel-core gaps.** Effort is for one experienced kernel developer; these are my estimates.

| # | Gap | What it involves | Person-weeks |
| --- | --- | --- | --- |
| 1 | NX and W^X | Set EFER.NXE on all cores. Add NX to the PTE flags, VM_EXEC, PF_X in the loader. Split the kernel into PHDRS with RX/R/RW mappings. Map the direct map NX. Add a real `mprotect`/PROT_NONE and remove mlibc's no-op. | 2-4 |
| 2 | Fix the likely bugs | #DB/#BP BKL leak; `file_grow` overflow; heap exhaustion via write buffering (needs per-process caps); late-AP race; TLB shootdown give-up; the `privileged` global; `kcommand` gating by root and sandbox. | 1-2 |
| 3 | `copy_from_user`/`copy_to_user` | Exception-table fixups, a narrow STAC window and an audit of every handler for double fetches. | 2-3 |
| 4 | Entropy and ASLR | A kernel RNG (RDRAND/RDSEED plus jitter), `getrandom`, a real AT_RANDOM and salts. ASLR for stack, mmap and ld.so; PIE support; then KASLR (relocatable kernel). | 4-7 |
| 5 | Hardening flags | Stack protector in kernel and libc, FORTIFY, RELRO/PIE for ports. Replace iterated SHA-256 with Argon2id or yescrypt. | 1-2 |
| 6 | IPC access control | Owners and modes or capabilities for shm, usock names and ports; sandbox enforcement for IPC and kernel commands. | 2-4 |
| 7 | Syscall fast path | Reorder the GDT for SYSRET; STAR/LSTAR/SFMASK; `swapgs` plus a GS per-CPU area; a vDSO with TSC-based `clock_gettime`. | 3-4 |
| 8 | Timekeeping | A 64-bit ns monotonic clock (invariant TSC with HPET or PIT calibration); hrtimers; ns `nanosleep` and `clock_gettime`; then TSC-deadline tickless idle. | 3-5 |
| 9 | FPU state | XSAVE/XSAVEOPT with an XCR0 setup, enabling AVX, AVX2 and AVX-512 for user space (compatibility and performance). | 1-2 |
| 10 | POSIX signals | Masks, sigaction flags, siginfo, sigaltstack, delivery on any return to user mode (ticks too), per-thread signals and tgid semantics (shared getpid), EINTR/SA_RESTART. | 4-6 |
| 11 | Scheduler | Per-CPU run queues with locks, priorities or nice (CFS-like or MLFQ), wait queues instead of channel scans, load balancing and affinity, voluntary kernel preemption points. | 6-10 |
| 12 | BKL removal | Fine-grained locks (VM per address space, heap and physmem, VFS and drivers, process table), removal of the static scratch globals in process.c, IRQ-safe spinlocks versus mutexes, IOAPIC/MSI routing to all cores. | 12-20 |
| 13 | Memory scalability | Buddy allocator, vmalloc and a dynamic heap, guarded kernel stacks, a page cache with shared file mappings and shared .so text, an OOM killer, optionally swap, maple-tree or RB-tree regions, VA reuse. | 10-16 |
| 14 | Syscall-layer file semantics | Real fds (O_RDWR, pread/pwrite, 64-bit offsets, no whole-file buffering), the `*at` family, fcntl, an ioctl framework, epoll-like events. Overlaps the VFS area. | 5-8 |
| 15 | Debuggability | Frame pointers or an unwinder plus a kallsyms-like symbol table for panics, a panic-time CPU stop via NMI IPI, a persistent ring buffer, a gdbstub, ptrace (or a debug-port API) and core dumps. | 5-8 |
| 16 | Resource limits | rlimits and per-user caps (tasks, memory, fds); a dynamic task table. | 2-3 |
| 17 | Power, kernel side | ACPI `_CST`/`_LPI` C-states, AMD CPPC, thermal zones and passive cooling, runtime PM hooks, interrupt-driven SCI. Partly driver work. | 6-10 |
| 18 | Boot chain | A signed loader (shim or own keys), kernel signature verification, a boot menu with fallback or previous kernel and a safe-mode flag set (`nogpu`, `verbose`, `nosmp`) from TERMOS.EFI. | 3-5 |
| 19 | Move policy out of ring 0 | Move the shell, editor, script, installer and compiler commands, and later TLS, out of the kernel. | 4-8 |

**Milestone sketch** (my grouping, for the report writer):

| Milestone | Contents | Person-weeks |
| --- | --- | --- |
| M1 Hardening | Items 1-6 | ~12-22 |
| M2 ABI and time | Items 7-10 | ~11-17 |
| M3 Scalability | Items 11-13 | ~28-46 |
| M4 POSIX completeness and debug | Items 14-16 | ~12-19 |
| M5 Platform | Items 17-19 | ~13-23 |

Reaching the 5-6 band in C2-C5 roughly corresponds to finishing M1-M3 plus most of M4.

### Inferences
- The project is unusually *broad* for its age, but *shallow* in exactly the places production kernels invested most: security mitigations, locking and scheduling, and memory sharing.
- The highest-leverage work is M1. It is cheap compared with its security impact, and C5 would rise from about 2 to about 4.
- M3 is the expensive, risky refactor that determines whether multicore performance can approach the independent-OS peers.

### Gaps
- The scores are preliminary. They are based on reading code and docs, not on running benchmarks or real-hardware tests.
- The peer comparisons (SerenityOS, Haiku, Redox, Linux, NT, XNU) come from general knowledge and were not re-verified against sources in this session.
- The effort estimates are expert guesses, not derived from data.
