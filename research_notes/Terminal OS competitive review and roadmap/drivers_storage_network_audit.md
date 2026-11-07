# Terminal OS: drivers, storage, filesystems, networking, TLS, Wi-Fi, graphics, audio, input, USB and power (source audit)

*Read-only audit of the repository at HEAD `3c9f14b` (2026-11-07). Paths are relative to the repo root; `git:<hash>` = commit. Line numbers are as of HEAD. Effective development history: 2025-10-02..07 (toy kernel), then 2026-10-08..11-07 (almost everything here); 146 commits, most co-authored with "Claude Opus 5.5" per their trailers.*

## 1. Driver inventory: device classes, chipsets/PCI IDs, interrupt mode, DMA, real-hardware evidence

### Takeaway
There are 37 C files (17,170 lines) in `src/impl/x86_64/drivers`, plus 2,189 lines in `net/`, and 774 + 418 lines in `sys/vfs.c` and `sys/install.c` (20,551 lines in total). They cover one narrow, Intel-centric hardware profile: NVMe, AHCI, xHCI, HDA, Intel Gen12 iGPU assist, Intel AX101 Wi-Fi, Intel LPSS I2C touchpads, RTL8139/RTL8168/e1000, uACPI. The only real machine named anywhere is one Acer Alder Lake laptop. Everything else is QEMU or VirtualBox. Four drivers use MSI/MSI-X. The rest use the 8259 PIC or poll from the timer tick or idle loop. There is no IOAPIC support.

### Cited Findings
| File (lines) | Class / chips handled | Interrupts | DMA | Real-HW evidence | Source |
|---|---|---|---|---|---|
| nvme.c (375) | NVMe, PCI class 01/08/02, any vendor; one controller, namespace 1 | MSI-X preferred, then MSI, else polled | 64 KiB bounce buffer + PRP list | laptop SSD (TERMINALOS partition), QEMU `video` test | [nvme.c:1-10,143,248-254](src/impl/x86_64/drivers/nvme.c); [git:3124546]; [run_tests.py:410](tests/qemu/run_tests.py) |
| ahci.c (494) | SATA AHCI class 01/06/01; ≤2 HBAs, ≤8 disks; Intel RST (01/04) only detected | MSI, else re-check each tick | 64 KiB bounce, one PRD per 4 KiB | QEMU `sata` test only | [ahci.c:1-13,80-81,399-414,431-434](src/impl/x86_64/drivers/ahci.c); [git:344acd9] |
| ata.c (125) | legacy IDE 0x1F0, PIO, LBA28, primary master only | polled, ~1 s timeouts | none | QEMU | [ata.c:1-3,39](src/impl/x86_64/drivers/ata.c); [ARCHITECTURE.md:156](docs/ARCHITECTURE.md); [git:84ffd90] |
| xhci.c (1626) | xHCI class 0C/03/30, **first controller only**; HID boot kbd/mouse, HID report-protocol pointers, BOT mass storage, USB 2/3 hubs, CDC-ECM | MSI (IMOD 250 µs) + timer-poll backstop | TRB rings; 64 KiB bounce for sticks | laptop USB kbd/mouse/stick ([git:f07662b], [git:bbc9bb2]); hubs, ECM and tablets in QEMU | [xhci.c:1-26,617-618,1514-1545](src/impl/x86_64/drivers/xhci.c); [git:1bb75ca], [git:ea035f5] |
| hda.c (808) | Intel HDA class 04/03, or the Intel SST DSP (04/01) driven as plain HDA | none: CORB/RIRB polled, stream refilled from timer hook | BDL ring of 4×16 KiB | QEMU intel-hda recorded to WAV | [hda.c:1-14,239,387-389,736-757](src/impl/x86_64/drivers/hda.c); [git:29070a2] |
| igfx.c (1747) | Intel Gen12/13 iGPU at 00:02.0, IDs 0x9A40-9AFF, 0x4C80-4C8F, 0x4680-46DF, 0x4620-462F, 0xA780-A7AF, 0xA720/1 | polled (seqno wait ≤50 ms) | GGTT + PPGTT; BCS/RCS execlists | real HW only, not emulable | [igfx.c:110-114,170-226](src/impl/x86_64/drivers/igfx.c); [GPU.md:17-19](docs/GPU.md) |
| display.c (216) | Bochs VBE "DISPI" (QEMU std VGA 1234:, VirtualBox 80EE:, VMSVGA 15AD: in VBE mode) | none | none | VMs | [display.c:1,81-83](src/impl/x86_64/drivers/display.c) |
| iwlwifi.c (2152) | Intel CNVi "So" family: AX101 tested; AX201/AX211 claimed | MSI + polled CSR_INT | RX/command queues, 4 KiB buffers | Acer Aspire A325-51 (AX101) | [iwlwifi.c:1-16,1160-1162](src/impl/x86_64/drivers/iwlwifi.c); [wifi.c:62](src/impl/x86_64/drivers/wifi.c) |
| wifi.c (266) | PCI ID table of 103 Wi-Fi chips (Intel 46, Atheros, Realtek, MediaTek, Broadcom), naming only | n/a | n/a | n/a | [wifi.c:20-110,264](src/impl/x86_64/drivers/wifi.c) |
| i2c_hid.c (1066) | Intel LPSS DesignWare I2C (Skylake…Meteor Lake, Apollo/Gemini Lake IDs) + HID-over-I2C touchpads | polled every 4 ms (no GPIO IRQ) | none (FIFO PIO) | real laptop | [i2c_hid.c:1-9,61-77,766-767](src/impl/x86_64/drivers/i2c_hid.c); [git:f07662b] |
| keyboard.c / mouse.c (420/539) | PS/2 8042 keyboard (US layout), mouse with IntelliMouse wheel, VirtualBox absolute pointer | PIC IRQ1/IRQ12 | none | laptop + VMs | [keyboard.c:41,189](src/impl/x86_64/drivers/keyboard.c); [mouse.c:1,169-193](src/impl/x86_64/drivers/mouse.c) |
| vmmdev.c (66) | VirtualBox guest device (80EE, class 08/80): mouse integration, window size | none | none | VirtualBox | [vmmdev.c:1](src/impl/x86_64/drivers/vmmdev.c); [registry.c:29](src/impl/x86_64/drivers/registry.c) |
| e1000.c (308) | 8086:100E (82540EM), 100F, 1004, 10D3 (82574L) | PIC line if routed, plus timer poll | 32 RX / 8 TX legacy descriptors | VirtualBox, QEMU (DHCP, ping, HTTPS) | [e1000.c:83-94,188,240-242](src/impl/x86_64/drivers/e1000.c); [git:5cf198c] |
| rtl8139.c (299) | 10EC:8139 | PIC (IRQ 11 in QEMU) | 8 KiB RX ring (WRAP), 4 TX | QEMU | [rtl8139.c:122,142](src/impl/x86_64/drivers/rtl8139.c); [KNOWN-ISSUES.md:54-64](docs/KNOWN-ISSUES.md) |
| rtl8168.c (268) | 10EC:8168/8161/8169 | PIC, else polled | 32 RX / 8 TX | **never run** | [rtl8168.c:76-77,125,145](src/impl/x86_64/drivers/rtl8168.c); [NETWORKING.md:18,31-36](docs/NETWORKING.md) |
| power.c / acpi.c (790/197) | uACPI glue, embedded controller, battery/AC/button/lid; own AML scan for I2C descriptors | SCI **not wired**: polled from idle loop | n/a | laptop | [power.c:1-8,292-306](src/impl/x86_64/drivers/power.c); [acpi.c:1,86-97](src/impl/x86_64/drivers/acpi.c) |
| cpupower.c (175) | Intel HWP/EPP, MWAIT C1/C3/C6 (bare-metal Intel only) | n/a | n/a | laptop | [cpupower.c:3-7,97](src/impl/x86_64/drivers/cpupower.c) |
| msi.c, apic.c, pic.c, pci.c, efi.c, rtc.c, timer.c, registry.c | MSI/MSI-X (32 vectors from 0x50); LAPIC/x2APIC timer (no IOAPIC); 8259; CF8/CFC config; UEFI variables; CMOS RTC; PIT; device tree | — | — | — | [msi.c:11-12](src/impl/x86_64/drivers/msi.c); [apic.c:1-20](src/impl/x86_64/drivers/apic.c); [pci.c:1-25](src/impl/x86_64/drivers/pci.c) |

- The first commit to boot real hardware is titled "Run on a real laptop: APIC timer, local-time clock, USB, I2C touchpad". It added an APIC-timer fallback for laptops whose PIT gives no interrupts — [git:f07662b].
- The only named machine is "Acer Aspire A325-51: AX101" — [wifi.c:62](src/impl/x86_64/drivers/wifi.c). ROADMAP calls the test laptop "Alder Lake, an LPIT table … Modern Standby and no ACPI S3" — [ROADMAP.md:68-69](docs/ROADMAP.md).
- The real-hardware debugging channel is a boot log written to the USB stick (`/USB/LOGS`, `scripts/usb-logs.ps1`) — [GPU.md:17-19](docs/GPU.md); [main.c:125-158](src/impl/kernel/main.c).
- There are zero TODO/FIXME/XXX/HACK markers in all 20,551 lines (grep count 0 per file). Debt is recorded in docs ("Cuts:" paragraphs in ROADMAP.md, KNOWN-ISSUES.md) instead — [ROADMAP.md:62-66,75-78,146-148](docs/ROADMAP.md).

### Inferences
- Real-hardware coverage is about "one Alder Lake laptop + QEMU + VirtualBox". Any other laptop has a high chance of missing at least one of: Wi-Fi (non-AX101), audio (SOF/SoundWire codecs), the touchpad (AMD or non-LPSS I2C), graphics acceleration (non-Gen12), or USB ports on a second xHCI.
- With no IOAPIC, PCI INTx interrupts on modern PCs (APIC-only routing) are generally unusable. Legacy NICs on real hardware will run polled from the 100 Hz tick. The e1000 and RTL8168 code already assume this ([nic.c:32-45](src/impl/x86_64/drivers/nic.c)).

### Gaps
- There is no hardware compatibility list. Apart from the commit messages, nothing records which SSD, SATA, USB or HDA codec models were tried on real hardware.
- I could not independently verify the Acer A325-51's chipset. The repo's own docs say Alder Lake.

## 2. Driver model: bus/driver framework, probing, hotplug, isolation, and the cost of adding a driver

### Takeaway
There is no driver framework. A descriptive "IOKit-lite" registry exists, but `kernel_main` calls each driver's init in a fixed order. Each driver rescans PCI itself, and most drivers are singletons (one controller, one NIC, one GPU, one Wi-Fi card). Hotplug exists only for USB. Every driver runs in ring 0 under the big kernel lock, with no IOMMU. Adding a simple PCI driver is cheap. Supporting multiple instances, power management or isolation requires building the framework first.

### Cited Findings
- `registry.h` describes "the device tree (IOKit-lite)". PCI functions are matched by class or vendor to a driver *name* for display (`ioreg`). It never binds or calls drivers. MAX_NODES is 128 — [registry.h:1-8](src/intf/drivers/registry.h); [registry.c:8,12-30,57-82](src/impl/x86_64/drivers/registry.c).
- Init order is hard-coded in `kernel_main`: NIC probe (377), ATA and RAM-disk mount (397), MSI (411), igfx (414-419), USB (425), NVMe (436), AHCI (442), persistent mounts (447-448), sound (450), touchpad (453), ACPI (456), DHCP (464+), Wi-Fi autoconnect (480), CPU power (487) — [main.c:377-494](src/impl/kernel/main.c).
- Each driver calls `pci_scan()` into its own 64-entry array — [ahci.c:428-431](src/impl/x86_64/drivers/ahci.c); [xhci.c:1514-1516](src/impl/x86_64/drivers/xhci.c); [registry.c:58-59](src/impl/x86_64/drivers/registry.c). The scan brute-forces 256 buses through legacy CF8/CFC config cycles. There is no ECAM/MCFG, so the PCIe extended config space (AER, L1 substates, etc.) is unreachable — [pci.c:1-25,45,109-111](src/impl/x86_64/drivers/pci.c).
- PCI helpers: BAR0 assignment for BARs the firmware left empty (laptop LPSS controllers) — [pci.c:208-335](src/impl/x86_64/drivers/pci.c). D3→D0 wake-up via `pci_power_on` — [pci.c:340+](src/impl/x86_64/drivers/pci.c).
- The only abstractions are three structs:
  - `nic_driver_t`, a vtable with a single static `active` card, first found wins — [nic.h](src/intf/drivers/nic.h); [nic.c:24-52](src/impl/x86_64/drivers/nic.c).
  - `drive_t`, a list with MAX_DRIVES 8 and an optional flush — [drive.h:12-22](src/intf/drivers/drive.h).
  - `blockdev_t` — [disk.h](src/intf/drivers/disk.h).
- Singleton patterns:
  - NVMe keeps one set of static queues and bounce buffer — [nvme.c:60,200-203,250-253](src/impl/x86_64/drivers/nvme.c).
  - xHCI returns after the first controller — [xhci.c:1514-1545](src/impl/x86_64/drivers/xhci.c).
  - HDA picks one controller — [hda.c:387-389](src/impl/x86_64/drivers/hda.c).
  - igfx probes only 00:02.0 — [igfx.c:170-172](src/impl/x86_64/drivers/igfx.c).
  - Wi-Fi uses `wifi_get(0)` and rejects non-Intel cards ("Only Intel Wi-Fi cards have a driver so far.") — [iwlwifi.c:1120-1131](src/impl/x86_64/drivers/iwlwifi.c).
- Hotplug: USB only, serviced from the shell's idle loop (`usb_service`) — [xhci.c:1550-1580](src/impl/x86_64/drivers/xhci.c); [main.c:245-252](src/impl/kernel/main.c). There is no PCIe hotplug, Thunderbolt or dock handling (no matches in `src`).
- Isolation:
  - Network IRQ handling takes the BKL — [nic.c:62-66](src/impl/x86_64/drivers/nic.c).
  - Storage waits release it — [nvme.c:113-116](src/impl/x86_64/drivers/nvme.c); [ahci.c:250-253](src/impl/x86_64/drivers/ahci.c).
  - There is no IOMMU/VT-d (no DMAR matches). ROADMAP lists "Cut: no user-space drivers (DriverKit) yet" — [ROADMAP.md:234](docs/ROADMAP.md).
- Interrupts:
  - MSI uses 32 slots from vector 0x50 — [msi.c:11-12](src/impl/x86_64/drivers/msi.c). Only vector 0 of each device is used (single vector, no per-queue vectors) — [nvme.c:261-264](src/impl/x86_64/drivers/nvme.c).
  - MSIs land on one core: "On a core that does not receive the NVMe interrupt each command waited for a timer tick" — [git:2be7e9d].
  - No IOAPIC: 0 matches in `apic.c`, `smp.c`, `acpi.c` and `power.c`, and `uacpi_set_interrupt_model` is never called.
- DMA: `dma_alloc` for NVMe, AHCI and USB buffers — [nvme.c:200](src/impl/x86_64/drivers/nvme.c); [ahci.c:402-403](src/impl/x86_64/drivers/ahci.c); [xhci.c:666](src/impl/x86_64/drivers/xhci.c). NIC drivers give `kmalloc` buffers with 32-bit physical addresses to the card — [ARCHITECTURE.md:106](docs/ARCHITECTURE.md).
- Commit size as a cost proxy:
  - The e1000 driver was 310 lines plus small edits in 5 files — [git:5cf198c].
  - AHCI was 494 lines plus a drive-list refactor — [git:344acd9].
  - CDC-ECM was +239 lines inside `xhci.c` — [git:1bb75ca].

### Inferences
- Adding a driver costs little ceremony: write a `.c` file, call it from `kernel_main`, and use `pci_scan`/`mmio_map`/`dma_alloc`/`msi_enable`/`timer_add_poll_hook`. Rough cost: a polled PCI device 0.5-1 person-week; an MSI+DMA-ring device 1-2 pw. A USB class driver costs 1-3 pw, because classes live inside the monolithic `xhci.c` rather than behind a USB-core/class split.
- The hidden cost is structural. Multiple instances, suspend/resume callbacks, runtime PM, hot-removal, interrupt affinity, and user-space or isolated drivers all need a real bus/driver model first, estimated at 4-6 pw.

### Gaps
- I did not find a written driver-author guide. `docs/DEVELOPING.md` was not reviewed in depth.

## 3. Storage: NVMe, AHCI/SATA, ATA PIO, USB mass storage, partitions, block cache, write policy, async I/O, RAM disk

### Takeaway
Storage works, but every path is synchronous: one command in flight per device, a memcpy through a 64 KiB bounce buffer for every I/O, no request queue and no shared buffer/page cache. NVMe has no FLUSH and no TRIM, and AHCI has no NCQ or hotplug. USB sticks use Bulk-Only Transport (BOT) with READ/WRITE(10) and no SYNCHRONIZE CACHE. Only AHCI implements a write-cache flush, which undermines ext3's journal guarantees on NVMe and USB media.

### Cited Findings
- **NVMe:**
  - Described as "NVMe 1.x controller, polled": admin queue plus one I/O queue, namespace 1 — [nvme.c:1-10](src/impl/x86_64/drivers/nvme.c).
  - QDEPTH is 64, but `submit()` posts one command and spins or sleeps until it completes — [nvme.c:32,95-118](src/impl/x86_64/drivers/nvme.c).
  - MSI-X is preferred (vector 0) — [nvme.c:248-264](src/impl/x86_64/drivers/nvme.c).
  - Every read and write is copied through a 16-page (64 KiB) bounce buffer — [nvme.c:33,200-203,300-325](src/impl/x86_64/drivers/nvme.c).
  - 4 KiB-sector drives accept whole blocks only — [nvme.c:303](src/impl/x86_64/drivers/nvme.c).
  - I/O times out after 5 s and returns -1. There is no abort or controller reset — [nvme.c:318](src/impl/x86_64/drivers/nvme.c).
  - The drive is registered without a `flush` callback — [nvme.c:283-292](src/impl/x86_64/drivers/nvme.c). There are no FLUSH (opcode 0x00) or DSM/TRIM (0x09) commands anywhere in the file.
- **AHCI:**
  - Only "one command in slot 0" is used, so there is no NCQ (no FPDMA commands) — [ahci.c:208-259](src/impl/x86_64/drivers/ahci.c).
  - A per-HBA mutex allows one command at a time — [ahci.c:97-105,263-279](src/impl/x86_64/drivers/ahci.c).
  - Firmware handoff uses the BIOS/OS handoff register (BOHC) — [ahci.c:392-397](src/impl/x86_64/drivers/ahci.c).
  - Errors restart the port, reset the link (COMRESET) if the disk stays busy, and retry once — [ahci.c:175-205,268-271](src/impl/x86_64/drivers/ahci.c).
  - FLUSH CACHE EXT has a 30 s timeout — [ahci.c:76,286-292](src/impl/x86_64/drivers/ahci.c).
  - Not supported: ATAPI, port multipliers, 4Kn and pre-LBA48 disks — [ahci.c:~460-470](src/impl/x86_64/drivers/ahci.c); [ROADMAP.md:146-147](docs/ROADMAP.md).
  - Ports are enumerated once at init, so there is no SATA hotplug — [ahci.c:405-407](src/impl/x86_64/drivers/ahci.c).
  - Intel RST/RAID mode is only reported ("switch it to AHCI") — [ahci.c:431-434](src/impl/x86_64/drivers/ahci.c).
- **ATA:**
  - PIO, LBA28, primary master only — [ARCHITECTURE.md:156](docs/ARCHITECTURE.md). LBA is capped at 32 bits — [disk.c:34-42](src/impl/x86_64/drivers/disk.c).
  - Timeouts were added in [git:84ffd90], but [KNOWN-ISSUES.md:125-126](docs/KNOWN-ISSUES.md) still says the driver "polls the status register forever" (stale).
- **USB mass storage:**
  - Bulk-Only Transport only (class 8, subclass 6, protocol 0x50) — [xhci.c:912](src/impl/x86_64/drivers/xhci.c).
  - Device bring-up runs INQUIRY, TEST UNIT READY and REQUEST SENSE (up to 20 tries) — [xhci.c:695-705](src/impl/x86_64/drivers/xhci.c).
  - READ CAPACITY(10) with 512-byte blocks only, so the limit is 2 TiB and 4 KiB-sector sticks are refused — [xhci.c:707-712](src/impl/x86_64/drivers/xhci.c).
  - I/O uses READ(10)/WRITE(10) in 128-sector chunks through `st_buf` — [xhci.c:617-618,735-755](src/impl/x86_64/drivers/xhci.c).
  - No SYNCHRONIZE CACHE and no UAS. USB partitions get no `flush` hook — [disk.c:77-94](src/impl/x86_64/drivers/disk.c).
- **Block layer:**
  - `blockdev_t` is a set of read/write/flush function pointers — [disk.h](src/intf/drivers/disk.h).
  - Writes are allowed only inside the system's own partition ("never Windows' data") — [disk.c:53-55,71-73](src/impl/x86_64/drivers/disk.c); [disk.h:31-35](src/intf/drivers/disk.h).
  - Flush is forwarded only if the drive has one — [disk.c:57-61,133](src/impl/x86_64/drivers/disk.c).
- **Partitions:**
  - GPT and MBR are read — [part.c:1,80-140](src/impl/x86_64/drivers/part.c).
  - Known GPT types: ESP, MSR, Microsoft data, WinRE, Linux — [part.c:15-22](src/impl/x86_64/drivers/part.c).
  - Filesystems are sniffed as FAT32 or ext2/3/4 (ext4 detected by incompat 0x2C0) — [part.c:31-75](src/impl/x86_64/drivers/part.c).
  - The GPT entry count is capped at 256 — [part.c:113](src/impl/x86_64/drivers/part.c).
  - `part_gpt_add` writes an entry with CRC32 and moves the backup table. CRC32 is computed only there — [part.c:146-212](src/impl/x86_64/drivers/part.c).
- **Caching:** there is no unified buffer or page cache.
  - FAT32 caches one FAT sector, write-through except during large writes and deletes — [fat32.c:1-8,29-69](src/impl/x86_64/drivers/fat32.c); [fatvol.h:22-25](src/intf/drivers/fatvol.h).
  - ext2 caches metadata per volume: at most 256 clean blocks, at most 2048 dirty blocks per transaction. File data bypasses the cache — [ext2.c:3-16,35-36,171-275](src/impl/x86_64/drivers/ext2.c).
- **Write policy:**
  - All writes are synchronous. FAT32 writes contiguous runs up to 1 MiB in one command and defers FAT updates. Before that change, installing the 32 MB RAM disk "took about ten minutes" — [git:2be7e9d].
  - There is no async or vectored I/O interface anywhere in the block path.
- **RAM disk:**
  - A 40 MiB FAT32 image loaded as a boot module and used in place — [git:84ffd90]; [disk.c:5-33](src/impl/x86_64/drivers/disk.c).
  - It is "/" when nothing persistent exists, otherwise "/SYS" — [main.c:80-94](src/impl/kernel/main.c); [vfs.h:9-13](src/intf/sys/vfs.h).
- **Mount policy:**
  - Internal drives are used only through a FAT32 partition labelled TERMINALOS — [main.c:100-123](src/impl/kernel/main.c), the check is at line 105.
  - The USB boot stick gets FAT32 "TERMOS USB" at /USB — [main.c:137-158](src/impl/kernel/main.c) — and an ext3 "TERMOS DATA" partition created from 1 GiB to the end of the stick — [main.c:160-221](src/impl/kernel/main.c).

### Inferences
- **Journal integrity risk:** ext3 commits call `dev_flush` ([ext2.c:167-169,285-291,381](src/impl/x86_64/drivers/ext2.c)), but that is a no-op on NVMe and on USB sticks. On drives with volatile write caches, a power cut can therefore reorder or lose journal and in-place writes despite the "never half way" claim ([readme.md:50](readme.md)). This is a strong inference from the code. Not tested on hardware.
- **Throughput:** with queue depth effectively 1, a 64 KiB maximum per command and a memcpy for each, NVMe throughput is likely an order of magnitude below the device's capability. No benchmark exists to quantify this.

### Gaps
- There are no storage benchmarks or device-model lists in the repo. Multi-namespace NVMe, NVMe power states (APST) and SMART/health reporting are all absent (no code), but their impact is unmeasured.

## 4. Filesystems and VFS (vfs.c, fat32.c, ext2.c), install and persistence

### Takeaway
The VFS is small: 6 mounts, longest-prefix matching, case-insensitive names, and a full path walk on every call with no dentry or inode cache. Unix permissions are enforced on ext2/ext3 only. FAT32+VFAT and ext2/ext3 are supported, ext3 with a real, simplified JBD journal and replay. Missing:
- creating symlinks or hard links, and following symlinks;
- ext4, streaming writes (files are written whole at close), files over 4 GB, and correct handling of files deleted while still open;
- tmpfs, devfs and procfs;
- exFAT, NTFS, ISO9660, encryption and snapshots.

An *installed* system's "/" is FAT32 with no owners. The permissions and journal therefore apply only when running from the USB stick's ext3 data partition.

### Cited Findings
- **Mounts:**
  - Each `mount_t` holds a FAT volume or an ext2 volume — [vfs.c:13-19](src/impl/x86_64/sys/vfs.c). VFS_MAX_MOUNTS is 6 — [vfs.h:66](src/intf/sys/vfs.h).
  - Mount points are stored in capitals — [vfs.c:240](src/impl/x86_64/sys/vfs.c). A volume is probed as FAT32 first, then ext2 — [vfs.c:250-264](src/impl/x86_64/sys/vfs.c).
  - Mounts are found by longest prefix — [vfs.c:124-149](src/impl/x86_64/sys/vfs.c). The FS is serialized by the global `vfs_lock` mutex — [vfs.h:87](src/intf/sys/vfs.h).
- **Names and lookup:**
  - Names keep their case but compare without it "(as on Windows)" — [vfs.c:24-37](src/impl/x86_64/sys/vfs.c).
  - `resolve()` normalizes the path, applies the sandbox `path_check`, then looks up every component on every call with a kmalloc'd scratch buffer. There is no dentry or inode cache — [vfs.c:187-224](src/impl/x86_64/sys/vfs.c).
  - `step()` checks search (x) permission on ext2. A component that is not a directory returns NOT_FOUND, so symlinks are never followed — [vfs.c:164-185](src/impl/x86_64/sys/vfs.c).
- **Permissions:**
  - Owner/group/other bits with a single gid. Root and the kernel bypass them — [vfs.c:66-89](src/impl/x86_64/sys/vfs.c). Sticky /TMP — [vfs.h:15-19](src/intf/sys/vfs.h).
  - FAT32 has only path guards, the "home-folder rule" — [vfs.c:38-46](src/impl/x86_64/sys/vfs.c); [vfs.h:19](src/intf/sys/vfs.h); [KNOWN-ISSUES.md:10-12](docs/KNOWN-ISSUES.md).
  - Groups are "one per user". Symlinks, hard links and xattrs are not created ("Linux's are read") — [ROADMAP.md:62-66](docs/ROADMAP.md). There are no ACLs.
- **File semantics:**
  - The kernel file API is whole-file — [vfs.c:363-436,733-745](src/impl/x86_64/sys/vfs.c). `ext2_write` takes the complete contents, and `ext2_read_at` takes a 32-bit offset — [ext2.h:89,96](src/intf/drivers/ext2.h).
  - "Files are written whole at `close` … the largest file is 4 GB". A deleted file still open, or mapped, is read from freed blocks — [KNOWN-ISSUES.md:17-19](docs/KNOWN-ISSUES.md).
  - File-backed `mmap` exists at the syscall interface (SYS_MMAP/MMAP2 take an fd) — [process.c:1250-1252,1349-1355](src/impl/x86_64/sys/process.c). This is kernel core; only the interface was checked.
  - There is no tmpfs, procfs or devfs (no matches in `src/impl`).
- **FAT32:**
  - FAT32 only. FAT12/16 are rejected (`clusters < 65525`) — [fat32.c:790](src/impl/x86_64/drivers/fat32.c).
  - VFAT long names up to 255 characters with Windows `~N` aliases. The FSInfo free count is marked unknown — [git:f0780d7].
  - Long names are ASCII only. `ls` shows at most 128 entries, `tree` 32, and `cat` refuses files over 1 MB — [KNOWN-ISSUES.md:10-11](docs/KNOWN-ISSUES.md).
  - Rename was implemented on 2026-11-07 — [fat32.c:616](src/impl/x86_64/drivers/fat32.c); [git:1e7c70e]. KNOWN-ISSUES.md:11 and ROADMAP.md:62 still say FAT32 cannot rename (stale).
- **ext2/ext3 journal design:**
  - Each public call is one transaction. Metadata goes to the journal (descriptor, blocks, commit), the journal superblock is pointed at it, the blocks are written in place, then the journal is emptied. The journal holds only one transaction, so revoke records are not needed — [ext2.c:1-12,285-291,339-381](src/impl/x86_64/drivers/ext2.c).
  - File data is written to newly allocated blocks before the commit, with the old blocks freed in the same transaction (copy-on-write for data) — [ext2.c:14-16](src/impl/x86_64/drivers/ext2.c).
- **ext2/ext3 recovery and limits:**
  - Committed transactions are replayed at mount — [ext2.c:906-960](src/impl/x86_64/drivers/ext2.c). A Linux journal containing revoke records is refused ("e2fsck must do this one") — [ext2.c:950](src/impl/x86_64/drivers/ext2.c). External journals are refused — [ext2.c:1038](src/impl/x86_64/drivers/ext2.c).
  - Any incompat feature other than FILETYPE/RECOVER is refused, so ext4 (extents, 64bit, flex_bg) does not mount. An unknown ro_compat feature makes the volume read-only — [ext2.c:1020-1032](src/impl/x86_64/drivers/ext2.c).
  - Hashed (dir_index) folders are read linearly, and their index flag is cleared on change — [ext2.c:29,833,868](src/impl/x86_64/drivers/ext2.c).
  - Fast symlinks are recognized only when freeing — [ext2.c:898](src/impl/x86_64/drivers/ext2.c).
- **ext2 tests:**
  - A `crash journal` hook stops after a commit — [ext2.h:64](src/intf/drivers/ext2.h); [ext2.c:407-408](src/impl/x86_64/drivers/ext2.c).
  - Host test: 313 lines; `test_basics`/`test_crash`/`test_full`; 61 checks; e2fsck validates the written volumes, including mke2fs-made ones — [tests/ext2_host_test.c](tests/ext2_host_test.c); [ROADMAP.md:59-62](docs/ROADMAP.md).
  - QEMU `storage` test: an ext3 stick with owners, reboot, a journal crash, and e2fsck — [run_tests.py:500-504](tests/qemu/run_tests.py).
- **Install:**
  - It may touch only a FAT32 partition labelled TERMINALOS (files kept) or an empty partition (formatted FAT32). It never touches MSR, WinRE, ESP, NTFS, BitLocker or exFAT partitions — [install.c:1-8,76-90,300-327](src/impl/x86_64/sys/install.c).
  - It copies TERMOS.EFI, KERNEL.BIN and RAMDISK.GZ — [install.c:29-33,187-215](src/impl/x86_64/sys/install.c). It adds Boot#### at the *end* of BootOrder through UEFI runtime variables — [install.c:154-185](src/impl/x86_64/sys/install.c); [efi.c:1-8](src/impl/x86_64/drivers/efi.c).
  - The loader is unsigned, so Secure Boot must be off — [readme.md:110-112](readme.md).
  - "The installer still makes FAT32 (firmware boots only from FAT)" — [ROADMAP.md:62-63](docs/ROADMAP.md).
  - There is no update, rollback, A/B or signature logic in `install.c` (no matches).
- **Persistence on the stick:** the first boot from a stick larger than 1.2 GB adds a GPT entry and formats ext3 "TERMOS DATA" from 1 GiB to the end. Re-imaging the stick keeps that partition, which is found again — [main.c:160-221](src/impl/kernel/main.c); [readme.md:50](readme.md). There is no in-OS fsck: a failed mount says "`fsck` it in Linux" — [main.c:209](src/impl/kernel/main.c).

### Inferences
- Security and robustness differ sharply between the two deployments. Booted from USB, the system has ext3 with journal and Unix modes. Installed on an internal SSD, it has FAT32: no owners, path guards only, no journal, and FAT updates that are not crash-atomic. This contradicts users' expectation that installing is the "better" mode.
- Whole-file writes and the 4 GB limit rule out databases, large media, VM images and logs that append in place. The per-call path walk with no caches means metadata-heavy workloads (builds, `git`, package installs) will be slow.

### Gaps
- Not verified: file locking (flock/fcntl), inotify-style change notification, quotas. I did not grep for them, so their absence is unconfirmed.
- No measurements of filesystem performance.

## 5. USB (xHCI, classes, hubs, hotplug, speeds)

### Takeaway
One monolithic xHCI driver (1,626 lines) handles low/full/high/SuperSpeed devices (up to 5 Gbit/s), hubs (up to 5 tiers, with transaction translators) and hotplug. It supports four class functions: HID boot keyboard/mouse, HID report-protocol pointers (tablets, single-touch screens), Bulk-Only mass storage, and CDC-ECM Ethernet. Missing: isochronous transfers (so no USB audio or webcams), UAS, NCM/RNDIS, Bluetooth, printers, any xHCI controller beyond the first, EHCI/OHCI/UHCI, and USB4/Thunderbolt.

### Cited Findings
- The header describes polling the event ring, route strings, transaction translators and CDC-ECM — [xhci.c:1-26](src/impl/x86_64/drivers/xhci.c).
- Speed codes are FULL/LOW/HIGH/SUPER only, and the highest named speed is "5 Gbit/s" — [xhci.c:89-92,1588-1593](src/impl/x86_64/drivers/xhci.c). There is no SuperSpeedPlus (10/20 Gbit/s) handling.
- MAX_DEVICES is 32 — [xhci.c:132](src/impl/x86_64/drivers/xhci.c).
- The driver takes the controller from the BIOS and turns SMIs off — [xhci.c:1396-1415](src/impl/x86_64/drivers/xhci.c).
- `usb_init` takes the first xHCI (prog-if 0x30) and returns — [xhci.c:1514-1545](src/impl/x86_64/drivers/xhci.c). MSI with moderation is set to at most one interrupt per 250 µs, and the timer poll stays as a backstop — [xhci.c:1388-1392,1533-1538](src/impl/x86_64/drivers/xhci.c).
- Hotplug: root and hub ports are rescanned from the idle loop. A removed device gives its slot back with Disable Slot — [xhci.c:1550-1580](src/impl/x86_64/drivers/xhci.c); [git:344acd9].
- HID:
  - Each interface is switched to the boot protocol (SET_PROTOCOL, "may be refused") and SET_IDLE — [xhci.c:566,601-603](src/impl/x86_64/drivers/xhci.c).
  - Keyboards are boot-protocol only (6-key rollover), translated to set-1 scancodes — [xhci.c:1291-1307](src/impl/x86_64/drivers/xhci.c).
  - The report protocol is parsed only for pointers — [xhci.c:136-138](src/impl/x86_64/drivers/xhci.c); [git:ea035f5].
- A stalled endpoint is recovered with Reset Endpoint and a new dequeue pointer — [xhci.c:114,630-657](src/impl/x86_64/drivers/xhci.c).
- Hubs: USB 2 and USB 3 hub descriptors, port power and reset, change bits including over-current, and the TT think time — [xhci.c:766-860](src/impl/x86_64/drivers/xhci.c).
- CDC-ECM:
  - Every configuration is examined, the packet filter is set, 16 receives are queued, and the MAC comes from a string descriptor. A dongle plugged in later asks DHCP itself — [xhci.c:974-1160](src/impl/x86_64/drivers/xhci.c); [git:1bb75ca].
  - There is no CDC-NCM, no RNDIS, and no vendor-only adapters such as the ASIX AX88179 — [NETWORKING.md:38-43](docs/NETWORKING.md).
- `lsusb` only *names* Bluetooth (class 0xE0), camera (0x0E) and audio (1) interfaces — [xhci.c:1614-1619](src/impl/x86_64/drivers/xhci.c).
- The header comment in `usb.h` is stale ("Hubs and other devices are listed but not used") — [usb.h:3-6](src/intf/drivers/usb.h).
- EHCI, OHCI and UHCI appear only as PCI class names, and there is no USB4/Thunderbolt/Type-C code (no matches in `src`).
- QEMU tests cover:
  - `hub`: keyboard, mouse, stick and a hub behind a hub, with plug-in and removal;
  - `tablet`: pixel-exact clicks through QMP;
  - `usbnet`: DHCP and ping, then re-plugging into another hub port.
  — [run_tests.py:693-782](tests/qemu/run_tests.py).

### Inferences
- On many Tiger Lake and Alder Lake laptops the Type-C ports sit on the CPU's own xHCI, a separate PCI function from the chipset's xHCI. With only the first controller used, those ports would likely be dead. This combines the code with general platform knowledge and is untested.
- Boot-protocol-only keyboards mean no media/consumer keys and no N-key rollover over USB.

### Gaps
- I found no evidence of a SuperSpeed device being tested on real hardware.

## 6. Input (PS/2, I2C-HID touchpads, touchscreens, keyboard layouts)

### Takeaway
Input covers the test laptop well: PS/2 keyboard and mouse, and an Intel LPSS I2C Precision Touchpad with tap, two-finger tap/scroll and palm rejection. The rest is thin: only the US keyboard layout, no I2C touchscreens, single-touch USB touch, touchpad polling instead of GPIO interrupts, and nothing for AMD I2C controllers or PS/2 touchpad protocols.

### Cited Findings
- **Keyboard:**
  - PS/2 scancode set 1 with US tables only (`kbdus`/`kbdus_shift`). There are no other layout tables or switching (a `layout` grep finds nothing else) — [keyboard.c:41,189](src/impl/x86_64/drivers/keyboard.c).
  - Home/End/PgUp/PgDn/Delete/Insert and Shift/Ctrl+arrows reached programs only from 2026-11-07 — [git:1e7c70e].
- **Mouse:** PS/2 IntelliMouse wheel at 200 reports/s, plus VirtualBox absolute positioning — [mouse.c:169-193](src/impl/x86_64/drivers/mouse.c).
- **Touchpad, controller side:**
  - Intel LPSS DesignWare I2C plus HID over I2C 1.0 — [i2c_hid.c:1-9](src/impl/x86_64/drivers/i2c_hid.c). PCI IDs cover Skylake through Meteor Lake, plus Apollo/Gemini Lake — [i2c_hid.c:61-77](src/impl/x86_64/drivers/i2c_hid.c).
  - Devices are found by a custom scan of ACPI tables for I2C serial-bus descriptors, not uACPI `_CRS` — [acpi.c:1,86-97](src/impl/x86_64/drivers/acpi.c).
  - Controllers are woken from D3 and given a BAR if the firmware left none — [git:f07662b]. The bus runs at 400 kHz, falling back to standard mode — [git:b433451].
  - The pad is polled every 4 ms, with no GPIO interrupt — [i2c_hid.c:766-767](src/impl/x86_64/drivers/i2c_hid.c).
- **Touchpad, gestures:**
  - Move, click, tap, two-finger tap and two-finger scroll — [git:f07662b].
  - Palm (confidence-bit) rejection and filtering of resting fingers and jumps — [git:00beae9]; [i2c_hid.c:485-548](src/impl/x86_64/drivers/i2c_hid.c).
  - Tap-and-drag selection — [git:bd8b932]. User settings for speed, acceleration and tap — [git:d68d4d2].
  - No three-finger, pinch or swipe gestures exist (no such code found).
- **Touchscreens:**
  - I2C touchscreens are only *named* — [i2c_hid.c:976-977](src/impl/x86_64/drivers/i2c_hid.c). The parser looks for the touchpad collection 0x000D0005 — [i2c_hid.c:385](src/impl/x86_64/drivers/i2c_hid.c).
  - USB tablets and touchscreens work as single-touch absolute pointers — [git:ea035f5]; [ROADMAP.md:142-143](docs/ROADMAP.md).

### Inferences
- AMD laptops' I2C controllers (ACPI platform devices, not PCI) and Synaptics/ALPS/Elan PS/2 touchpad modes are unsupported, so on non-Intel laptops the touchpad likely falls back to plain PS/2 or does nothing.
- Non-US users cannot type their layout, which overlaps with category C15.

### Gaps
- Input-method/IME handling is out of scope here and was not checked.

## 7. Graphics (igfx.c, gpu_shaders.h, shader toolchain, framebuffer fallback, other GPUs)

### Takeaway
There is no modesetting. The screen stays in the UEFI GOP mode that the loader picks (native resolution from EDID). On Intel Gen12/13 only, `igfx.c` adds a hardware cursor plane, tear-free page flips, the blitter, and a GPGPU path. That path runs ten OpenCL-C kernels, compiled offline by Intel's ocloc and checked against the CPU at boot, for the desktop's blur, shadow, fade and scaling. Everything else is framebuffer-only: other Intel generations, AMD, NVIDIA, virtio-gpu and VMware SVGA. There is no OpenGL/Vulkan, multi-monitor, display hotplug, brightness control, or HDR/VRR.

### Cited Findings
- `igfx.c` leaves the firmware's mode and pipe alone and uses only the cursor plane and page flips — [igfx.c:1-11](src/impl/x86_64/drivers/igfx.c). The loader picks the native mode from EDID — [git:b433451].
- Supported device-ID ranges are Gen12/13 only. Other Intel GPUs get "not a Gen12/13 display (software pointer)" — [igfx.c:110-114,174](src/impl/x86_64/drivers/igfx.c).
- It drives the single pipe whose plane 1 shows the linear GOP framebuffer — [igfx.c:178-193](src/impl/x86_64/drivers/igfx.c). There is no DPLL, link-training, hotplug (HPD) or EDID code in `igfx.c` (no matches).
- 6-bpc panels get dithering — [igfx.c:196-209](src/impl/x86_64/drivers/igfx.c); [git:0aa1d90]. The cursor gets its own display-buffer (DDB) blocks and watermarks — [igfx.c:229-250](src/impl/x86_64/drivers/igfx.c).
- Engines take work through execlists, with no GuC/HuC firmware — [GPU.md:3-6](docs/GPU.md); [igfx.c:331-350,435-440](src/impl/x86_64/drivers/igfx.c). Forcewake — [igfx.c:143-167](src/impl/x86_64/drivers/igfx.c).
- The blitter (BCS) runs `XY_FAST_COPY_BLT`, and the render engine (RCS) does GPGPU dispatch — [GPU.md:8-15](docs/GPU.md); [igfx.c:462-570](src/impl/x86_64/drivers/igfx.c).
- Shaders: `gpu/shaders.cl` (OpenCL C) is compiled by ocloc in `toolchain/gpu` (Docker, Ubuntu 24.04) into `src/intf/drivers/gpu_shaders.h` (389 KB, checked in). The kernels are SIMD16, with no arguments and no scratch space — [GPU.md:40-59](docs/GPU.md); [git:e99bdaa].
- Batches follow PIPE_CONTROL → PIPELINE_SELECT GPGPU → GPGPU_WALKER, and the CPU waits at most 50 ms — [GPU.md:61-78](docs/GPU.md).
- Failure policy: a hang turns the whole GPU path off, and a wrong pixel disables that shader. There is no engine reset — [GPU.md:32-36](docs/GPU.md); [igfx.c:1092,1446,1490-1499](src/impl/x86_64/drivers/igfx.c).
- Programs reach it through `gfx_gpu` and `gfx_gpu_draw` (at most 12 drawings per batch, screen owner only, CPU fallback) — [GPU.md:80-98](docs/GPU.md); [igfx.c:1627](src/impl/x86_64/drivers/igfx.c).
- The backlight is only switched off and on for sleep. There is no brightness API — [igfx.c:73-108](src/impl/x86_64/drivers/igfx.c).
- In VMs, Bochs VBE handles modes and the screen follows the VirtualBox window size — [display.c:1-20,81-93](src/impl/x86_64/drivers/display.c); [git:2819d45]. "In a virtual machine the driver is not used and the desktop draws on the CPU" — [GPU.md:19](docs/GPU.md).
- Testing is on real hardware only, and stages 1, 3 and 5 are marked "new: check the log" — [GPU.md:12-18](docs/GPU.md).
- The stated next step is "the whole frame as GPU layers" — [GPU.md:113-115](docs/GPU.md).

### Inferences
- The GPU work is a compositor-effects accelerator, not a graphics stack. Ported apps that expect GL/Vulkan render on the CPU (Skia and Ladybird are CPU-rendered per the userland commits).
- The engineering is deep for one GPU family, but it covers roughly Intel laptops from 2020 to 2023 only.

### Gaps
- It is unknown whether an external monitor works. The code touches only the firmware's pipe, and a mode change is impossible.

## 8. Audio (Intel HDA)

### Takeaway
Audio is playback-only Intel HDA: one controller and one output stream at 48 kHz/16-bit/stereo. The codec graph is walked from speaker, headphone or line-out pins to a DAC. HDMI/DP and digital outputs are excluded. There is no jack detection, no capture, no codec quirk table, and volume is applied in software. The stream is exclusive to one program (no mixer). It has been tested in QEMU.

### Cited Findings
- The driver takes class 04/03, or the Intel SST DSP (04/01) "used as plain HDA" — [hda.c:1-14,387-389,420](src/impl/x86_64/drivers/hda.c).
- CORB/RIRB are polled with no IRQ (GIE off) — [hda.c:239](src/impl/x86_64/drivers/hda.c). Unsolicited responses (jack events) are skipped — [hda.c:176](src/impl/x86_64/drivers/hda.c).
- The path search excludes digital converters and HDMI/DP pins — [hda.c:278,300](src/impl/x86_64/drivers/hda.c).
- The format is fixed at 48 kHz 16-bit 2-channel — [hda.c:85-86,376,572,762-770](src/impl/x86_64/drivers/hda.c).
- Volume scales the samples in software — [hda.c:123,493,627](src/impl/x86_64/drivers/hda.c).
- A ring of 4×16 KiB is refilled from a timer hook that follows the DMA position (LPIB) — [hda.c:14,736-757](src/impl/x86_64/drivers/hda.c).
- WAV files (8/16-bit, mono/stereo) are resampled to 48 kHz by linear interpolation — [hda.c:666-723](src/impl/x86_64/drivers/hda.c).
- There is no capture or input stream. "ADC" and "microphone" appear only in diagnostic names — [hda.c:290-292,509](src/impl/x86_64/drivers/hda.c).
- There are 0 quirk/fixup entries (grep count).
- `SYS_SOUND` gives the stream to one owner. A second program gets SYSERR_INUSE — [process.c:912-950](src/impl/x86_64/sys/process.c), line 921.
- Testing used "QEMU's intel-hda recorded to a WAV file" — [git:29070a2].

### Inferences
- On machines whose audio goes through the Sound Open Firmware (SOF) DSP (DMIC arrays, SoundWire/I2S codecs, common from Tiger Lake on), the "plain HDA" approach finds no usable codec. It works only where an HDA-link codec exists.
- With no capture there are no microphones, so no calls or voice input. USB and Bluetooth audio are impossible: there are no isochronous USB transfers and no Bluetooth stack.

### Gaps
- I found no explicit confirmation that sound played on the real laptop. The commit message names QEMU only.

## 9. Networking: NIC drivers and the stack (Ethernet, ARP, IPv4/IPv6, ICMP, UDP, TCP, DHCP, DNS, sockets, loopback, routing, firewall, multicast)

### Takeaway
The network stack is IPv4-only, interrupt-context and single-interface. It has ARP (8 entries), ICMP echo, DHCP, a one-shot DNS A-record client, and a 64-socket TCP with go-back-N retransmission and fast retransmit. Missing:
- in TCP: congestion control, RTT estimation, window scaling, SACK and timestamps;
- a secure ISN (it is derived from the tick);
- IPv6, multicast/mDNS, UDP/raw sockets for programs, a routing table and a firewall.

Programs get custom TCP-only system calls; mlibc layers BSD sockets on top of them in userland.

### Cited Findings
- **NIC selection:**
  - The first card found is the only card: RTL8139, then RTL8168, then e1000 — [nic.c:24-47](src/impl/x86_64/drivers/nic.c).
  - Wi-Fi and USB adapters replace it through `nic_attach`, and are polled from the timer — [nic.c:49-52](src/impl/x86_64/drivers/nic.c).
  - Received frames are handled in IRQ context and must stay short — [nic.h](src/intf/drivers/nic.h); [NETWORKING.md:82-83](docs/NETWORKING.md).
- **Driver maturity:**
  - RTL8168: "Written, untested on hardware" and "QEMU cannot emulate this chip, so the driver has not been run yet" — [NETWORKING.md:18,31-36](docs/NETWORKING.md); [readme.md:97-98](readme.md); [git:0d45ab0].
  - e1000: tested in VirtualBox and QEMU with DHCP, ping and HTTPS — [git:5cf198c].
  - Rings are 32 RX / 8 TX descriptors. There is no checksum offload, TSO, jumbo frames or VLAN (no matches) — [e1000.c:83-84](src/impl/x86_64/drivers/e1000.c); [rtl8168.c:76-77](src/impl/x86_64/drivers/rtl8168.c).
- **IPv4 and ARP:**
  - The ARP cache has 8 entries — [net.c:116-118](src/impl/x86_64/net/net.c).
  - The IPv4 header checksum is verified. Fragments are dropped ("not supported") — [net.c:299-301](src/impl/x86_64/net/net.c).
  - Only unicast to us is accepted, DF is set, and the next hop is the host on our subnet, else the single gateway — [NETWORKING.md:60-63](docs/NETWORKING.md).
  - There is no routing table, IPv6 (no EtherType 0x86DD anywhere), IGMP/multicast/mDNS, or packet filter (no matches).
- **ICMP and loopback:** echo replies, plus unreachable/TTL-exceeded for the active ping. "Loopback" is only `ping` to our own IP or 127.x, answered without touching the NIC — [net.c:240-290,523-553](src/impl/x86_64/net/net.c); [NETWORKING.md:64-68](docs/NETWORKING.md).
- **UDP and DNS:**
  - UDP is sent with checksum 0 — [net.c:801](src/impl/x86_64/net/net.c).
  - The DNS client accepts one question and returns the first A record only. There is no AAAA and no CNAME chasing — [net.c:805-835](src/impl/x86_64/net/net.c).
  - net.c describes itself as "deliberately a small client rather than a general socket stack: one DNS A lookup, one TCP connection, HTTP/1.0 GET" — [net.c:644-647](src/impl/x86_64/net/net.c). This comment predates tcp.c.
- **DHCP:**
  - DISCOVER/OFFER/REQUEST/ACK with 3 attempts of 2 s each. QEMU's addresses are the fallback. "Leases are not renewed yet" for wired links — [NETWORKING.md:23-26](docs/NETWORKING.md); [net.c:848-880](src/impl/x86_64/net/net.c).
  - Over Wi-Fi the lease is renewed halfway — [iwlwifi.c:2074-2075](src/impl/x86_64/drivers/iwlwifi.c).
- **TCP (tcp.c):**
  - Fixed TCB table; input runs from the NIC poll in the timer interrupt — [tcp.c:1-6](src/impl/x86_64/net/tcp.c).
  - 64 KiB RX and TX buffers per socket, our MSS 1460, RTO 1 s doubling up to 16 s, 8 retries, TIME-WAIT 2 s, backlog 4 — [tcp.c:26-32,183-184](src/impl/x86_64/net/tcp.c).
  - The advertised window is clamped to 65535 and the only option sent is MSS, so there is no window scaling, SACK or timestamps — [tcp.c:72,81-84](src/impl/x86_64/net/tcp.c).
  - Fast retransmit after 3 duplicate ACKs, then go-back-N — [tcp.c:352-353](src/impl/x86_64/net/tcp.c). Zero-window probing — [tcp.c:149](src/impl/x86_64/net/tcp.c).
  - There are no cwnd, ssthresh or srtt variables, so no congestion control or RTT estimation.
  - ISN = `get_tick() * 2654435761 ^ i * 7919` — [tcp.c:193](src/impl/x86_64/net/tcp.c).
  - An MSS below 64 is treated as 536. This fixes a hang of the whole system caused by an MSS-0 peer — [tcp.c:214-225](src/impl/x86_64/net/tcp.c); [git:06dae5f].
  - Socket limit: TCP_MAX_SOCKETS is 64 — [tcp.h:14](src/intf/net/tcp.h). The readme still says "TCP with 16 connections at once" — [readme.md:56](readme.md). The commit that wrote tcp.c said "16 sockets" — [git:3124546].
- **Program interface:**
  - Custom system calls RESOLVE/CONNECT/LISTEN/ACCEPT/SEND/RECV/SOCKINFO/SOCKCLOSE, each gated by the sandbox's network permission — [syscall_net.c:1-4,73-97](src/impl/x86_64/sys/syscall_net.c); [git:f2c67df].
  - There are no UDP or raw sockets for programs (no matches).
  - Local (Unix-like) sockets are pipe pairs with file-descriptor passing — [syscall_usock.c:1-2](src/impl/x86_64/sys/syscall_usock.c).
  - BSD sockets are provided by the mlibc port in userland — [git:25d3dcc]; [git:b4dba43]. Only the interface was checked.
- **The kernel's own HTTP client (`download`):** HTTP/1.0, the response buffered in memory up to 16 MiB, redirects not followed — [NETWORKING.md:76-83](docs/NETWORKING.md).
- **Tests:**
  - `net_host_test.c`: 402 lines, 55 checks, with a mocked NIC, a simulated gateway and DHCP server, timeouts, corrupt checksums and random frames — [NETWORKING.md:69-70](docs/NETWORKING.md).
  - `tcp_host_test.c`: 303 lines, 17 checks, against a lossy, reordering simulated peer — [git:3124546].
  - QEMU `network` test: an HTTPS download — [run_tests.py:783-795](tests/qemu/run_tests.py).

### Inferences
- Without congestion control the stack can flood shared links. With a 64 KiB window and no scaling, throughput is capped at about 64 KiB per RTT (roughly 5 Mbit/s at 100 ms RTT), so bulk downloads over the internet are slow regardless of the link.
- A tick-derived ISN is predictable, enabling off-path TCP injection or spoofing. RFC 6528 requires a keyed hash. Combined with UDP checksum 0 and a single DNS answer accepted, spoofing resistance is weak.
- With no UDP for programs, ported software cannot do its own DNS, NTP, QUIC/HTTP3, WebRTC, mDNS/DNS-SD or games. DNS resolution goes through `SYS_RESOLVE`.

### Gaps
- I did not inspect how the DNS transaction ID and source port are generated, so DNS spoofing resistance beyond the above is unassessed.
- NTP/time sync appears absent: the clock comes from the RTC only ([NETWORKING.md:99](docs/NETWORKING.md)). I did not grep for an NTP client in userland.

## 10. TLS and crypto: BearSSL usage, TLS 1.3, certificate validation, trust store, randomness

### Takeaway
The kernel's HTTPS client uses BearSSL's TLS 1.2 client in its "full" profile. It validates the chain and hostname against 150 root CAs generated once on 2026-10-09. TLS 1.3 is absent, there is no revocation checking and no session resumption, and BearSSL is fetched unpinned. Entropy comes from RDRAND only, with no pool and no RDSEED. Programs get no kernel randomness system call.

### Cited Findings
- `br_ssl_client_init_full` with the generated trust anchors, `br_ssl_client_reset(&sc, host, 0)` (SNI and name check, no resumption) and the full-record I/O buffer `BR_SSL_BUFSIZE_MONO` in .bss — [bearssl_client.cpp:179-212](src/impl/x86_64/tls/bearssl_client.cpp).
- No `br_ssl_engine_set_versions` call exists in `bearssl_client.cpp` (grep), so the protocol range is BearSSL's default for the full profile.
- Entropy: 64 bytes from RDRAND only. Without RDRAND the handshake returns -7 — [bearssl_client.cpp:70-82,202-209](src/impl/x86_64/tls/bearssl_client.cpp). "Secure entropy requires a CPU with RDRAND; QEMU is launched with `-cpu max`" — [NETWORKING.md:99](docs/NETWORKING.md).
- Validation time comes from the CMOS RTC, with the build date as a floor. A clock set ahead makes valid certificates look expired (BearSSL error 54) — [bearssl_client.cpp:56-67](src/impl/x86_64/tls/bearssl_client.cpp); [NETWORKING.md:99](docs/NETWORKING.md).
- Trust store:
  - `scripts/gen-trust-anchors.sh` runs BearSSL's `brssl ta` over the build image's `/etc/ssl/certs/ca-certificates.crt` — [gen-trust-anchors.sh:1-27](scripts/gen-trust-anchors.sh).
  - It holds 150 anchors — [trust_anchors.c:6724,8377](src/impl/x86_64/tls/trust_anchors.c). The file has a single commit — [git:a1c53a0] (2026-10-09). Refreshing it is a manual rerun — [NETWORKING.md:93-97](docs/NETWORKING.md).
- BearSSL source: `git clone --depth 1 https://www.bearssl.org/git/BearSSL`, an unpinned HEAD — [buildenv/Dockerfile:17](buildenv/Dockerfile). The Docker image is "not reproducible" — [KNOWN-ISSUES.md:134-135](docs/KNOWN-ISSUES.md).
- The only other kernel RDRAND use is password-salt seeding (TSC + RDRAND + counter) — [users.c:107-120](src/impl/x86_64/sys/users.c). There is no getrandom-style system call (no matches in `sys/*.c` or `intf/sys/*.h`).
- WPA2 crypto (PBKDF2, the 4-way handshake MICs, key unwrap) also uses BearSSL — [git:fa75c36]; [wpa.c](src/impl/x86_64/net/wpa.c).

### Inferences
- BearSSL implements TLS only up to 1.2. Its full profile, by its documented defaults, also enables TLS 1.0/1.1 and some legacy suites, and it has no OCSP/CRL support. This is general knowledge of BearSSL and should be confirmed against the BearSSL source. TLS 1.3 therefore needs a different library.
- On CPUs without RDRAND (old or some low-end x86, some VM CPU models) HTTPS fails outright. Relying on a single hardware RNG with no mixing is a single point of trust.
- The trust store will age: any new root CA, and any distrust of an included CA, requires an OS rebuild.

### Gaps
- I did not verify which TLS stack the userland browser (Ladybird) and mlibc-based programs use. That belongs to other researchers' scope.

## 11. Wi-Fi (iwlwifi, firmware licence, rates, WPA2, missing features)

### Takeaway
A substantial clean-room bring-up (2,152 lines, modelled on OpenBSD's iwx(4)) supports Intel CNVi "So"-family cards, tested only on an AX101. It ships a single firmware file (Hr RF). It does scan, open and WPA2-Personal (CCMP) joins, reconnects and Wi-Fi DHCP renewal. Missing:
- 802.11a/g legacy rates are the only ones, on one chain, with no aggregation (54 Mbit/s or less);
- no 802.11n/ac/ax rates, WPA3/SAE, PMF, enterprise authentication, roaming or active 5 GHz scanning;
- no driver for any other vendor's chip. A 103-entry table names other chips without driving them.

### Cited Findings
- The header describes the "So" family (AX101/AX201/AX211 on CNVi), with bring-up "as OpenBSD's iwx(4) does it" — [iwlwifi.c:1-16](src/impl/x86_64/drivers/iwlwifi.c).
- Firmware: `/AX101.FW` is Intel's `iwlwifi-so-a0-hr-b0-77.ucode`, unchanged — [iwlwifi.c:34](src/impl/x86_64/drivers/iwlwifi.c); [firmware/README.md:3-7](firmware/README.md).
- Licence: Intel permits redistribution in binary form without modification, with the notice reproduced and "No reverse engineering, decompilation, or disassembly" — [firmware/LICENCE.iwlwifi_firmware:1-14](firmware/LICENCE.iwlwifi_firmware).
- Device selection: the first adapter, Intel only — [iwlwifi.c:1120-1131](src/impl/x86_64/drivers/iwlwifi.c).
- NVM bands are reported, and the regulatory country is the "ZZ" world default. Bluetooth coexistence is set to Wi-Fi mode — [iwlwifi.c:707-713,738,756](src/impl/x86_64/drivers/iwlwifi.c).
- Scan: 2.4 GHz channels 1-13; 5 GHz "listening only: legal everywhere" (passive). Hidden networks are only counted — [iwlwifi.c:979-1058,1098-1116](src/impl/x86_64/drivers/iwlwifi.c).
- Security: open and WPA2-PSK with CCMP. WEP, WPA/TKIP, WPA3-only and PMF-required (802.11w) networks are refused with an explanation — [iwlwifi.c:900-905](src/impl/x86_64/drivers/iwlwifi.c). The card does AES-CCMP with keys installed by the driver — [iwlwifi.c:1442-1466](src/impl/x86_64/drivers/iwlwifi.c).
- WPA2 itself is in `net/wpa.c` (243 lines: PBKDF2, 4-way handshake, group-key updates). Its host test is `wpa_host_test.c` (192 lines, 19 checks, IEEE/RFC vectors, a simulated access point) — [git:fa75c36].
- Rates:
  - TLC is configured with "legacy rates" on chain A only. If rate scaling is refused, data goes at a fixed 6 Mbit/s — [iwlwifi.c:1287,1305,1691-1728](src/impl/x86_64/drivers/iwlwifi.c).
  - Received MPDUs are taken one at a time, "no aggregation" — [iwlwifi.c:1393](src/impl/x86_64/drivers/iwlwifi.c).
  - "Legacy 802.11a/g rates for now (up to 54 Mbit/s)" — [readme.md:57](readme.md).
- Link management: missed beacons and deauthentication/disassociation trigger reconnection — [iwlwifi.c:1485-1530](src/impl/x86_64/drivers/iwlwifi.c). The network is saved in `/WIFI.CFG` (name plus derived key) and rejoined at boot — [git:fa75c36]; [main.c:480-485](src/impl/kernel/main.c).
- Interrupts: MSI plus polling of CSR_INT — [iwlwifi.c:575-586,1160-1162,1197-1198](src/impl/x86_64/drivers/iwlwifi.c).
- `wifi.c` names 103 chips with the Linux driver family for each. For anything else it prints "detected; scanning and connecting need a driver for this chip" — [wifi.c:20-110,264](src/impl/x86_64/drivers/wifi.c).

### Inferences
- In practice only AX101 (So MAC + Hr RF) is covered. AX211 uses a different RF ("Gf") with different firmware, and Tiger Lake's AX201 (0xA0F0/0x43F0) is a different MAC generation ("QuZ"), so the header's AX201/AX211 claim is likely optimistic. This rests on general knowledge of Linux iwlwifi and is untested here.
- Legacy rates on one chain without aggregation should give well under 30 Mbit/s of real throughput.
- The world regulatory domain with passive-only 5 GHz scanning means 5 GHz networks are found only if they beacon during the passive dwell.

### Gaps
- There is no Wi-Fi throughput measurement or list of tested access points.
- I did not check the firmware licence for any compatibility issue with the project's MIT licence. The binary sits on the RAM disk under Intel's own terms.

## 12. Power: uACPI integration, S5, sleep, battery/EC, CPU power, thermal, backlight, lid, runtime PM

### Takeaway
uACPI (vendored at commit `2cb5e71`) runs the firmware's AML. Working: S5 power-off, an embedded-controller driver, battery/AC reporting, power button and lid events, Intel HWP with balanced EPP, and MWAIT C6 idle. The ACPI interrupt (SCI) is polled from the idle loop because there is no IOAPIC. "Sleep" is a software freeze (screen off, tasks frozen, cores halted), not S3 and not true S0ix residency. There is no thermal management, brightness control or device runtime PM.

### Cited Findings
- uACPI is identified only by the git hash `2cb5e71ef58393839e1988ef9a8f1d8ba2845a8d` (no semantic version in the tree) — [third_party/uacpi/VERSION](third_party/uacpi/VERSION). Its README lists sleep states, GPE/fixed events and ACPI 6.6 resources — [third_party/uacpi/README.md](third_party/uacpi/README.md). It was vendored in [git:997d25c].
- The SCI "is not wired up, power_poll() checks the event registers from the idle loop" — [power.c:1-8,292-306,689-710](src/impl/x86_64/drivers/power.c).
- Init sequence: initialize, load the namespace, set up the EC before `_REG`/`_INI`, finalize GPEs, install the power-button fixed event, then find PNP0C0C (button), PNP0C0D (lid), PNP0C0A (battery) and ACPI0003 (AC) — [power.c:600-645](src/impl/x86_64/drivers/power.c).
- The embedded controller is found through ECDT or PNP0C09, gets an address-space handler, and runs `_Qxx` query events. EC timeouts are counted — [power.c:62,336-447](src/impl/x86_64/drivers/power.c).
- Battery charge, health and time left are shown, along with the charger state — [power.c:449-575](src/impl/x86_64/drivers/power.c); [readme.md:45](readme.md).
- Power-off is S5 only — [power.c:715-723](src/impl/x86_64/drivers/power.c).
- Reboot pulses the 8042 keyboard controller, then writes 0xCF9. It does not use the FADT reset register or EFI ResetSystem — [system.c:42-62](src/impl/x86_64/sys/system.c).
- Sleep:
  - `go_to_sleep` runs `vfs_shutdown` (volumes marked clean), `task_freeze` and `igfx_sleep` (backlight, plane and cursor off). A key, the power button or the lid wakes it — [power.c:648-710](src/impl/x86_64/drivers/power.c); [git:7dcfee5].
  - ROADMAP's cuts: "no ACPI S3 and no D3 for devices (so it draws more than Windows' Modern Standby: hours, not days), no wake on the touchpad, the power button no longer shuts down" — [ROADMAP.md:68-78](docs/ROADMAP.md).
  - No LPS0 `_DSM` or platform power-management calls exist (no `_DSM`/LPIT handling in `power.c`).
- No thermal code exists: no `_TMP`, `_PSV`, `_CRT`, thermal zones or fans (no matches), so there is no critical-temperature shutdown by the OS.
- There is no brightness control. The backlight is only switched off and on — [igfx.c:73-108](src/impl/x86_64/drivers/igfx.c).
- CPU power:
  - HWP is turned on from the lowest to the highest level with EPP "balanced" (0x80) — [cpupower.c:22,54-62](src/impl/x86_64/drivers/cpupower.c).
  - Idle uses MWAIT with the deepest of C1/C3/C6 listed in CPUID leaf 5, only if ARAT and an invariant TSC are present — [cpupower.c:98-104,133](src/impl/x86_64/drivers/cpupower.c).
  - All of this is disabled in VMs and on non-Intel CPUs — [cpupower.c:97](src/impl/x86_64/drivers/cpupower.c).
  - ACPI `_CST`/`_PSS`/`_CPC` are not used. ROADMAP: "no AMD P-states (CPPC); no tickless idle (the 1 ms tick still wakes each core)" — [ROADMAP.md:148](docs/ROADMAP.md).

### Inferences
- Without thermal zones the OS relies on firmware and CPU self-protection (PROCHOT, hardware shutdown). Fans controlled by the OS via ACPI never run up.
- The freeze-style sleep plus a 1 kHz tick on every core plus devices left in D0 means poor battery life in sleep and at idle, consistent with ROADMAP's "hours, not days".
- AMD systems get no power management beyond the firmware defaults.

### Gaps
- There are no measurements of idle or sleep power draw. Whether the lid and sleep work on any machine other than the test laptop is unknown.

## 13. Absent hardware classes and VM devices

### Takeaway
A wide set of mainstream hardware classes has no code at all. Some appear only as names in `lspci`/`lsusb`/partition listings.

### Cited Findings
- **Nothing in `src` (grep):** Bluetooth (only named: [xhci.c:1618](src/impl/x86_64/drivers/xhci.c), [wifi.c](src/impl/x86_64/drivers/wifi.c)), webcams/UVC, SD/MMC card readers, printers, sensor hubs (ISH), accelerometers, fingerprint readers, TPM, Thunderbolt/USB4, PCIe hotplug, IOMMU/VT-d (no DMAR), and Hyper-V VMBus.
- **Detected but not used:**
  - EHCI, OHCI and UHCI appear only as PCI class names in `pci.c`.
  - virtio: only the QEMU codec vendor name ([hda.c:484](src/impl/x86_64/drivers/hda.c)) and a PCI vendor name; no virtio-blk/net/gpu/input.
  - VMware SVGA II gets only the VBE fallback — [display.c:83](src/impl/x86_64/drivers/display.c).
  - NTFS, exFAT and BitLocker are detected only so they are never touched — [part.c:100-101](src/impl/x86_64/drivers/part.c); [install.c:84-87](src/impl/x86_64/sys/install.c).
  - Bluetooth, camera and audio USB interfaces are only named by `lsusb` — [xhci.c:1614-1619](src/impl/x86_64/drivers/xhci.c).
- **Discrete GPUs:** no AMD or NVIDIA code. Non-Gen12 GPUs get only the GOP framebuffer — [igfx.c:174](src/impl/x86_64/drivers/igfx.c); [GPU.md:17-19](docs/GPU.md).

### Inferences
- Without virtio drivers, VM users depend on emulated legacy devices (RTL8139/e1000, IDE/AHCI, Bochs VBE). That is slow, and cloud or KVM images that offer only virtio would have no disk or network.
- Without an IOMMU, any DMA-capable peripheral (Thunderbolt or PCIe) can read all of RAM. The risk is moot today because there is no hotplug, but it blocks safe external-device support.

### Gaps
- Not checked: serial UARTs beyond COM1, smart cards, and gamepads (USB HID gamepads are not parsed: only keyboard and pointer usages are, per §5).

## 14. Quality: error handling, timeouts, hard-coded limits, TODO counts, docs, host-test coverage

### Takeaway
The code is careful in a "happy path plus timeouts" way. Every hardware wait has a timeout. AHCI and USB have reset/retry paths, and the GPU disables itself on failure. But NVMe, xHCI and HDA have no controller-reset recovery, and limits are fixed small static tables. There are no TODO markers; debt is tracked in prose, and several docs are stale. Host unit tests cover only the protocol code (net, TCP, WPA2) and ext2. All drivers are tested only through QEMU system tests or by hand on one laptop.

### Cited Findings
- **TODO/FIXME/XXX/HACK:** 0 in every driver, net, TLS, VFS and install file (per-file grep count).
- **Error handling examples:**
  - AHCI restarts the port, resets the link and retries once — [ahci.c:175-205,268-271](src/impl/x86_64/drivers/ahci.c).
  - xHCI clears stalls — [xhci.c:630-657](src/impl/x86_64/drivers/xhci.c).
  - NVMe just returns -1 after 5 s — [nvme.c:318](src/impl/x86_64/drivers/nvme.c).
  - The GPU path is switched off on a hang — [igfx.c:1490-1499](src/impl/x86_64/drivers/igfx.c).
  - EC timeouts are counted — [power.c:347-352](src/impl/x86_64/drivers/power.c).
  - A peer's MSS 0 hung the whole system until [git:06dae5f].
  - A kernel-lock race caused "a rare panic in the storage test" — [git:deeb0fd].
- **Hard-coded limits:**
  - networking: ARP 8, TCP backlog 4, 64 sockets × 128 KiB of buffers, DHCP 3×2 s;
  - devices: NIC rings 32/8, USB devices 32, drives 8, HBAs 2, MSI slots 32, registry nodes 128;
  - storage: mounts 6, NVMe QDEPTH 64 (used as 1), 64 KiB bounce buffers.
  — sources as cited in §§2-9. The local time zone is compile-time "IST" — [NETWORKING.md:99](docs/NETWORKING.md); [main.c:240-242](src/impl/kernel/main.c).
- **Stale docs:**
  - [KNOWN-ISSUES.md:125-126](docs/KNOWN-ISSUES.md) (ATA polls forever), fixed in [git:84ffd90].
  - [KNOWN-ISSUES.md:11](docs/KNOWN-ISSUES.md) and [ROADMAP.md:62](docs/ROADMAP.md) (FAT32 cannot rename), fixed in [git:1e7c70e].
  - [NETWORKING.md:35-36](docs/NETWORKING.md) ("needs UEFI boot and a framebuffer console, which the OS does not have yet"), added in [git:550d30e].
  - [readme.md:56](readme.md) ("16 connections") versus [tcp.h:14](src/intf/net/tcp.h) (64).
  - [readme.md:93-98](readme.md): the hardware section still describes the IDE/RAM-disk era.
  - [usb.h:3-6](src/intf/drivers/usb.h).
  - [ARCHITECTURE.md:143-157](docs/ARCHITECTURE.md): the driver table lists only the early drivers.
- **Host tests:** [net_host_test.c](tests/net_host_test.c) (402 lines, 55 checks), [tcp_host_test.c](tests/tcp_host_test.c) (303, 17), [wpa_host_test.c](tests/wpa_host_test.c) (192, 19), [ext2_host_test.c](tests/ext2_host_test.c) (313, 61). There are none for NVMe, AHCI, xHCI, HDA, igfx, iwlwifi (beyond WPA), FAT32, VFS or partitions.
- **QEMU system tests:** 25 tests, including storage, sata, hub, tablet, usbnet, network, sleep, install, video (NVMe) and bios — [run_tests.py:1081-1083](tests/qemu/run_tests.py). Hardware-specific code (igfx, iwlwifi, i2c_hid, cpupower, real ACPI) is not emulable and is checked by reading boot logs from the USB stick — [GPU.md:17-36](docs/GPU.md).

### Inferences
- The 2.5-week burst that produced most drivers (2026-10-10 to 11-07) and the single test laptop suggest robustness on varied real hardware has not been exercised. Error paths for device misbehaviour (NVMe reset, xHCI host-controller error, HDA codec timeouts) are mostly untested.

### Gaps
- There is no fuzzing of the parsers (USB descriptors, HID report descriptors, DHCP/DNS/TCP input, ext2 on-disk structures). Only the network host tests feed "random garbage frames" — [NETWORKING.md:69-70](docs/NETWORKING.md).

## 15. Preliminary scores (C6, C7, C8, C9 and the hardware parts of C10 and C14) and prioritized gaps

### Takeaway
On the shared 0-10 scale, Terminal OS's hardware, storage and network side sits in the early-hobby band (about 2.5-3.5). For its age (about one month of real development) the single-vendor depth is unusually good: NVMe/AHCI/xHCI with MSI, an ext3 journal checked by e2fsck, WPA2 Wi-Fi, and GPGPU compositing on Intel Gen12. But breadth, data-integrity guarantees, power and thermal handling, and protocol completeness remain far below SerenityOS, Haiku or Redox, and very far below mainstream OSes.

### Cited Findings
- **C6, Storage and filesystems: 3.5/10.**
  - For: NVMe, AHCI, USB BOT, GPT/MBR, FAT32+VFAT, and ext2/ext3 with journal replay checked by e2fsck (§§3-4).
  - Against: synchronous one-command I/O with bounce copies; no NVMe/USB flush or TRIM; no shared cache; whole-file writes and a 4 GB limit; no symlinks, hard links or ext4; FAT32 root without permissions on installed systems; no tmpfs/procfs/devfs (§§3-4).
- **C7, Networking and connectivity: 3/10.**
  - For: three wired drivers (one never run), CDC-ECM, Wi-Fi on one chip at legacy rates with WPA2-PSK, DHCP, DNS A, TCP with fast retransmit, kernel TLS 1.2 with validation (§§9-11).
  - Against: no IPv6, UDP for programs, congestion control, window scaling, SACK, mDNS or firewall; predictable ISN; Wi-Fi limited to ≤54 Mbit/s and no WPA3/enterprise (§§9-11).
- **C8, Hardware support breadth and drivers: 3/10, at the low edge.**
  - For: an Intel Alder Lake laptop plus QEMU and VirtualBox work along the happy path (§1).
  - Against: one xHCI, one GPU family, one Wi-Fi chip, Intel-only touchpads, no IOAPIC, no virtio, no Bluetooth/webcam/SD/printers, no driver framework (§§1-2, 13).
- **C9, Power management and thermal: 2.5/10.**
  - For: uACPI S5, EC battery, lid and power button, HWP plus MWAIT C6 (§12).
  - Against: polled SCI, freeze-only sleep without S0ix residency or S3, no device D3/runtime PM, no thermal zones or fans, no brightness control, no AMD CPPC, no tickless idle (§12).
- **C10, hardware part (display/GPU): 3/10.**
  - For: deep Gen12 work (cursor plane, flips, blitter, validated GPGPU shaders).
  - Against: no modesetting, single pipe, no hotplug or multi-monitor, no brightness, no GL/Vulkan, framebuffer-only on all other GPUs and VMs (§7).
- **C14, hardware part (I/O and efficiency): 2.5/10.**
  - For: GPU offload of compositing; xHCI interrupt moderation; FAT32 1 MiB runs (§§5, 7; [git:2be7e9d]).
  - Against: queue-depth-1 storage with memcpy bounces; MSIs to one core; polling at 100 Hz/1 kHz; a TCP window capped at 64 KiB; Wi-Fi at legacy rates; no page cache for file data (ext2.c:14-16) (§§3, 9, 11, 12).

### Inferences
Top gaps in priority order, with rough person-week (pw) estimates for one experienced developer familiar with this codebase. These are estimates, not sourced.

1. **Block-layer and data integrity.** NVMe FLUSH/FUA and USB SYNCHRONIZE CACHE (1 pw). An async request queue with multiple outstanding commands, NVMe multi-queue with per-queue MSI-X, AHCI NCQ and zero-copy DMA via PRP/PRD lists (5-7 pw). TRIM (1 pw). Total **7-9 pw**.
2. **POSIX-grade filesystem semantics.** A page/buffer cache shared with mmap, streaming partial writes, files over 4 GB, unlink-while-open, symlinks and hard links (8-12 pw). Make ext3 (or ext4) the root on installed internal disks, keeping FAT32 for the ESP only (1-2 pw). ext4 extents (4-6 pw). tmpfs/devfs/procfs (2-3 pw). Total **15-23 pw**.
3. **Driver framework and interrupt routing.** IOAPIC with ACPI `_PRT` routing, MSI-X multi-vector with affinity, ECAM, a probe/bind/match model with multiple instances, suspend/resume hooks. **4-6 pw.** This unblocks items 6, 8 and 9.
4. **Network stack completeness.** TCP congestion control, RTT estimation, window scaling, SACK, timestamps and an RFC 6528 ISN (3-4 pw). UDP/raw sockets for programs (1-2 pw). DNS with AAAA/CNAME/caching/multiple servers and wired DHCP renewal (1-2 pw). IPv6 with ND/SLAAC (4-6 pw). Multicast/mDNS (1-2 pw). A basic firewall (2 pw). Total **12-18 pw**.
5. **Entropy and TLS hygiene.** A kernel entropy pool (RDSEED/RDRAND/jitter) with a getrandom system call, pinned BearSSL, a trust-store update path (2-3 pw). TLS 1.3 via a different library (3-6 pw). Total **5-9 pw**.
6. **Power and thermal.** Interrupt-driven SCI (after item 3), ACPI thermal zones with critical shutdown and fans, ACPI/PWM backlight brightness (3-4 pw). Real s2idle (LPS0 `_DSM`, device D3/runtime PM, PMC residency check) and S3 where offered (4-6 pw). AMD CPPC and tickless idle (2-3 pw). Total **9-13 pw**.
7. **Wi-Fi breadth and speed.** HT/VHT/HE rates with A-MPDU/BlockAck, multi-chain (4-6 pw). WPA3-SAE with PMF (2-3 pw). The other Intel families' firmware and bring-up (Qu, Gf, AX200/AX210/BE200) (4-8 pw). Active 5 GHz scanning, regulatory domains and roaming (2-3 pw). Total **12-20 pw**. Each additional vendor (Realtek, MediaTek) costs 8-15 pw.
8. **VM and graphics breadth.** virtio-blk/net/input/gpu (3-5 pw, high payoff for testers). Intel display modesetting: DP/HDMI link training, multiple pipes, hotplug, EDID (8-12 pw). Older and newer Intel generations (4-8 pw). AMD or NVIDIA native: 50+ pw each, or a DRM compatibility shim at 20-40 pw.
9. **USB completeness.** All xHCI controllers (1 pw). Isochronous transfers (2-3 pw). UAS (2 pw). HID report-protocol keyboards with consumer keys and NKRO (1 pw). USB audio class (2-3 pw). UVC (3-4 pw). CDC-NCM/RNDIS/ASIX (2-3 pw). Total **13-17 pw**. A Bluetooth HCI plus stack is a further 15-25 pw.
10. **Audio.** A mixer/multi-client sound server, capture, jack detection, HDMI/DP audio and a codec quirk table: **6-10 pw**. SOF/SoundWire DSP: a further 15-25 pw.
11. **Input.** Keyboard layouts, I2C touchscreens and multitouch, GPIO-interrupt I2C-HID, AMD I2C controllers, 3-finger gestures: **3-5 pw**.
12. **IOMMU (VT-d) DMA remapping:** **4-6 pw**. A prerequisite for safe Thunderbolt/PCIe hotplug.

### Gaps
- The scores are preliminary and code-based. They are not calibrated against hands-on testing of SerenityOS, Haiku or Redox on the same hardware, and no throughput, latency or power measurements exist in the repo to anchor C14 or C9.
- The person-week figures are the auditor's estimates for a developer already familiar with this codebase.
