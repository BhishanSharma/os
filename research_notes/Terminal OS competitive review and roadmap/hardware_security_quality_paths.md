# Hardware enablement, platform security and engineering quality: external strategies and precedents for Terminal OS (as of 7 November 2026)

## 1. Hardware priorities by market share: what to support first (C8, C7, C10)

### Takeaway
By unit volume, x86 laptops are about 71% Intel and 29% AMD (Mercury Research, Q2 2026). Macs are about 9.5% of PC shipments, and Arm Windows laptops are still a small minority. Since Terminal OS already targets Intel (Gen12 graphics, HDA, AX2xx Wi-Fi, HWP), the cheapest coverage gains come from deepening Intel-platform support first: newer Intel display generations, Intel Wi-Fi at HT/VHT/HE rates, SOF/DMIC audio and s2idle. The AMD stack comes second (MediaTek mt76 Wi-Fi first, then AMD display). Virtio should be done early because it is cheap. The Steam survey is skewed towards gaming PCs and no longer gives vendor totals, so the CPU-share and PC-shipment data are the better guide for laptop priorities.

### Cited Findings
- Mercury Research, Q2 2026 notebook CPU unit share: Intel 71.1%, AMD 28.9% (AMD up 8.4 points year on year). AMD's combined desktop and notebook client share was 30.3%, its first time above 30% — [TechSpot](https://www.techspot.com/news/113569-amd-crosses-30-client-cpu-market-share-against.html); [basic-tutorials](https://basic-tutorials.com/news/cpu-market-share-q2-2026-amd-breaks-the-30-percent-mark-intel-loses-ground-across-the-board/)
- AMD's mobile unit share was 28.3% in Q1 2026, up from 26% in Q4 2025 — [Tom's Hardware](https://www.tomshardware.com/pc-components/cpus/amd-reaches-46-percent-of-server-x86-cpu-revenue-intel-still-controls-70-percent-of-the-consumer-pc-market-share)
- IDC preliminary figures for Q3 2026: worldwide PC shipments fell 20.1% year on year to 62.7 M units. Shares were Lenovo 23.8%, HP 16.5%, Dell 12.1%, Apple 9.5% (5.9 M Macs, down 11.3%) and ASUS 8.7%. IDC blames channel buying earlier in the year to beat memory-driven price rises. Omdia counted 6.8 M Macs instead — [9to5Mac](https://9to5mac.com/2026/10/08/idc-mac-shipments-drop-11-in-q3-as-global-pc-market-slides-20/); [MacTech on Omdia](https://www.mactech.com/2026/10/09/the-omdia-research-group-reports-higher-mac-sales-than-idc/)
- Steam Hardware Survey, October 2026: Windows 93.70%, Linux 3.95% (+0.90), macOS 2.35%. The most common primary display is 1920×1080 at 50.23%. The most common RAM sizes are 16 GB (41.42%) and 32 GB (37.06%). The summary page no longer lists GPU vendor totals, only individual models — [Steam Hardware Survey](https://store.steampowered.com/hwsurvey/)
- Top Steam GPUs in October 2026: RTX 3060 3.62%, RTX 4060 Laptop 3.60%, RTX 5070 3.56%, RTX 4060 3.51%, RTX 3050 3.25%. Coverage warns that a shift in which users were sampled caused much of the month-to-month swing — [Guru3D](https://www.guru3d.com/story/steam-october-survey-puts-the-rtx-3060-back-at-the-a-sampling-caveat/). One machine-translated report puts desktop RX 90-series cards at 2.69% (low confidence) — [GameGPU](https://en.gamegpu.com/news/zhelezo/dolya-videokart-serii-nvidia-rtx-50-v-steam-snizilas-na-5-v-oktyabre-2026-goda)
- Arm Windows laptops are a small share: Snapdragon X systems were under 1.5% of the Windows market (about 0.8% of Q3 2024 shipments) — [ITdaily](https://itdaily.com/news/business/arm-op-windows-marktaandeel-in-q3/). Qualcomm cited Circana data claiming more than 10% of US retail Windows laptops priced above $800 in December 2024, a premium-only segment — [HotHardware](https://hothardware.com/news/qualcomm-10pct-windows-laptop-share). ABI Research forecast that Arm PCs, Macs included, would be about 13% of 2025 shipments — [TechNews (zh)](https://technews.tw/2025/01/06/arm-based-pc-will-not-be-more-than-a-minority-segment-in-2025/). Counterpoint forecasts more than a third of laptops will be Arm by 2029, which is a forecast, not data — [Counterpoint](https://counterpointresearch.com/en/reports/windows-on-arm-to-capture-over-one-third-of-the-market-in-ai-pc-era)
- I found no laptop-specific Wi-Fi chipset share data. For the overall Wi-Fi chipset market, MarketsandMarkets puts 2024 revenue shares at Broadcom 20–25%, Qualcomm 18–23%, MediaTek 15–20%, Intel 5–10% and Realtek 4–8% — [MarketsandMarkets](https://www.marketsandmarkets.com/ResearchInsight/wi-fi-chipset-market.asp). By units shipped in 2024, Realtek ranked first, then MediaTek, Broadcom and Qualcomm — [EE Times Asia](https://www.eetasia.com/wi-fi-chipset-market-update/)
- The FreeBSD Foundation's laptop project chose which Linux Wi-Fi drivers to port: iwlwifi (Intel), mt76 (MediaTek MT7922, with MT7925 next), rtw88 and rtw89 (Realtek). That choice is a good proxy for the chip families that matter on current laptops — [FreeBSD status report Q4 2025](https://www.freebsd.org/status/report-2025-10-2025-12/lkpi-wireless/)
- AMD-branded RZ608 and RZ616 Wi-Fi cards, common in AMD laptops, are MediaTek MT7921K and MT7922 chips that use the mt7921e driver. This comes from a low-quality buying guide — [Mountainside guide](https://git.mountainside.com/?p=4521)
- In linux-hardware.org's public USB dataset, Intel Bluetooth USB IDs have very large report counts. On Intel Wi-Fi/Bluetooth combo cards, Bluetooth is a USB HCI device — [linuxhw/LsUSB](https://github.com/linuxhw/LsUSB)
- Audio: on some Intel platforms Linux uses the SOF driver only when a DMIC (digital microphone) or SoundWire is present, and SoundWire is enabled from CannonLake on — [kernel patch (P.-L. Bossart)](https://lore.kernel.org/patchwork/patch/1128135/). SOF's troubleshooting guide says HDA codecs wired to amplifiers often need amplifier configuration that Windows does but Linux codec drivers do not, and that DMICs are a frequent problem on Chromebooks — [SOF docs](https://thesofproject.github.io/latest/getting_started/intel_debug/suggestions.html)
- The FreeBSD Foundation and Quantum Leap Research pledged $750,000 to the Laptop Support and Usability project (announced September 2024). It runs into 2026 with similar size and scope, and covers graphics, Wi-Fi 4/5, audio and sleep states — [LWN](https://lwn.net/Articles/1051394/); [FreeBSD Foundation year-one update](https://freebsdfoundation.org/blog/freebsd-closes-the-laptop-gap-year-one-project-update/). The project asks testers to run hardware probes and submit anonymised results to build a public compatibility chart — [Linuxiac](https://linuxiac.com/freebsd-opens-public-testing-for-its-laptop-support-push/)

### Inferences
- **Proposed order for the smallest driver set that covers most 2022–2026 laptops** (my synthesis of the data above and Terminal OS's README-level baseline):
  1. **Intel graphics display and modesetting for newer generations.** Terminal OS has Gen12 (Tiger Lake to Raptor Lake). Next are Meteor Lake, Lunar Lake, Arrow Lake and Panther Lake. About 71% of laptops have Intel CPUs, so nearly all of those have an Intel iGPU driving the internal panel, even with a discrete GPU.
  2. **Intel Wi-Fi at HT/VHT/HE rates** across AX200/201/210/211 and BE200/201, which share one firmware family. This adds the most real-world value per hour (see section 3).
  3. **USB Bluetooth HCI plus Intel Bluetooth firmware loading.** It reuses the existing xHCI stack.
  4. **SOF-based DMIC capture on Intel.** HDA alone gives speakers and headphones, but on many SoundWire/DMIC designs the internal microphones need SOF.
  5. **s2idle/S0ix** (section 4).
  6. **MediaTek MT7921/7922/7925 (mt76)**, which covers much of the 29% AMD share.
  7. **Realtek rtw88/rtw89**, for budget laptops.
  8. **AMD display**, which is very large (section 2).
  9. **Qualcomm ath11k/ath12k**, later.
- **Hybrid laptops (Intel or AMD iGPU plus NVIDIA dGPU).** Without NVIDIA acceleration, the realistic target is to drive the panel from the iGPU and power the NVIDIA dGPU down through ACPI runtime power management. A powered-on idle dGPU hurts battery life. This is an inference, not verified for specific models.
- **Steam data** is useful for display and RAM baselines (half of users at 1080p; 16–32 GB RAM typical), not for laptop silicon. Laptop panels with 1.25–2× scaling make fractional HiDPI scaling a C10 priority (inference).
- **Ethernet** is less important on laptops: many no longer have a port and use USB-C adapters. Terminal OS's CDC-ECM support already covers adapters that expose standard USB classes (inference, unverified; see Gaps).
- **Adopt FreeBSD's probe-driven method.** Ask testers to submit a hardware probe (or reuse linux-hardware.org device IDs) and rank driver work by the count of devices seen. The FreeBSD laptop project does exactly this.
- **Mapping to the framework.** For C8 (hardware breadth), reaching the 5–6 band ("like Haiku/SerenityOS") means working networking and Wi-Fi on most common chip families. Haiku gets there by reusing FreeBSD drivers (section 2). Terminal OS's all-native approach means each family is a separate project.

### Gaps
- No 2025–2026 laptop-specific shares for Wi-Fi chipsets, Ethernet controllers, touchpad controllers, NVMe vendors or audio codecs. linux-hardware.org and the linuxhw/Trends repositories hold device counts, but I could not retrieve per-vendor numbers.
- Steam no longer shows NVIDIA/AMD/Intel GPU vendor totals on its summary page, and I found no reliable secondary figure for October 2026.
- I found no data on what share of current laptops support S3 versus only S0ix.
- The claim that Intel I219 (e1000e), I225/I226 (igc) and Realtek RTL8125/RTL8153 dominate Ethernet is an unverified assumption.

## 2. Driver-reuse strategies: coverage, effort, maintenance and licensing (C8, C17)

### Takeaway
There are four proven models:
- **A full Linux API emulation layer** (FreeBSD's LinuxKPI with drm-kmod and the wireless drivers). This gives the broadest coverage, but it needs continuous, funded maintenance and pulls in GPL questions.
- **Per-driver native ports**, as OpenBSD does with DRM and iwx, and Haiku does with FreeBSD and OpenBSD network drivers. The port is cheaper per driver, but each driver must be re-synced by hand.
- **Driver-environment kits** (Genode DDE-Linux, NetBSD rump kernels). These are elegant but have niche maintenance.
- **User-space driver frameworks** (Fuchsia DFv2). These isolate drivers well but are an architectural commitment.

For an MIT-licensed, one-person kernel in 2026, the evidence favours selective ports of permissively or dual-licensed Wi-Fi drivers and wpa_supplicant, plus a tightly scoped Linux-compatibility shim only if the project later commits to AMD/Intel DRM.

### Cited Findings
- **FreeBSD iwlwifi via LinuxKPI.** 802.11n and 802.11ac support for the 22000 and later chip generations first appeared in FreeBSD 14.3. 802.11ax/be and 6 GHz are still only "planned" (man page dated 15 May 2026) — [iwlwifi(4)](https://man.freebsd.org/cgi/man.cgi?manpath=FreeBSD&query=iwlwifi&sektion=4). In Q1 2026 all LinuxKPI-based wireless drivers were updated to Linux v6.19 in main and stable/15, with FreeBSD-specific fixes needed so the iwlwifi mld sub-driver would load — [FreeBSD status report Q1 2026](https://www.freebsd.org/status/report-2026-01-2026-03)
- **The Q4 2025 FreeBSD report shows the maintenance burden in detail** — [FreeBSD Q4 2025 LinuxKPI 802.11 report](https://www.freebsd.org/status/report-2025-10-2025-12/lkpi-wireless/):
  - mt76 (MediaTek) passed packets at basic rates on an MT7922 "within 10 days", but stayed blocked on a LinuxKPI `struct page` conflict, with 11n/11ac still in progress.
  - rtw88 has a chipset that will not associate or scan properly.
  - rtw89 "can cause a panic at times" and transmit is still stuck on basic rates.
  - The Linux v6.16 update stalled for a quarter because of conflicts with drm-kmod and an nvidia-drm problem.
  - The FreeBSD Foundation sponsors the work, and the named contact is one developer (Bjoern A. Zeeb).
- **FreeBSD drm-kmod.** The drm-612-kmod port (Linux 6.12 DRM, with amdgpu, i915 and radeon) was at 6.12.85_1 on 10 June 2026 and requires FreeBSD 15 or 16. drm-66-kmod (6.6.25_9, May 2026) is the older branch — [FreshPorts drm-612-kmod](https://www.freshports.org/graphics/drm-612-kmod/); [FreshPorts drm-66-kmod](https://www.freshports.org/graphics/drm-66-kmod)
- **OpenBSD ports Linux DRM (amdgpu, inteldrm, radeon) into its own tree.** The mirrored commit logs show roughly yearly updates by one maintainer (jsg): 5.10.47 (July 2021), 5.15.14 (January 2022), 6.1.2 (January 2023), 6.6.12 (January 2024) — [OpenBSD drm commit history mirror](https://grok.dragonflybsd.org/history/openbsd/sys/dev/pci/drm/amd/amdkfd/kfd_crat.c)
- **iwx's path from OpenBSD through Haiku to FreeBSD.** A FreeBSD Foundation-sponsored port began in Q4 2024 — [FreeBSD Q4 2024 iwx report](https://www.freebsd.org/status/report-2024-10-2024-12/wireless-iwx). The FreeBSD commit of 31 March 2025 says the driver came from OpenBSD and was ported by Future Crew LLC. It targets AX200/AX210 and AX201/AX211, and probes at lower priority than iwlwifi. Early reports said FreeBSD's iwx did 11a/g only, while OpenBSD's does 11a/b/g/n/ac as a station but no ax rates — [Reddit thread (secondary)](https://lr.in.psf.lt/r/freebsd/comments/1jwbms6/intel_wifi_driver_iwx4_now_in_current/mmio028/?context=3)
- **Haiku.** Hugo Santos added a FreeBSD network-driver compatibility layer to Haiku in 2007 so FreeBSD drivers could compile "with few, if any changes" — [OSNews](https://www.osnews.com/story/17883/haiku-gets-freebsd-network-driver-compatibility-layer/). Haiku later rebuilt the layer on FreeBSD's iflib and ported FreeBSD 12's Ethernet and Wi-Fi drivers — [Phoronix](https://www.phoronix.com/news/Haiku-More-FreeBSD-Network). A Haiku developer says the OpenBSD iwm/iwx and wireless-stack code is already incorporated (forum post, not a changelog) — [Haiku forum](https://discuss.haiku-os.org/t/openbsd-7-5-new-updated-wireless-drivers/14910)
- **Genode DDE-Linux** runs unmodified Linux driver code inside Genode components. Its `lx_emul` layer re-implements parts of the Linux kernel API as C functions, and each driver gets its own tailored environment. Genode credits this approach for its Intel wireless and Intel graphics support — [Genode 21.08 release notes](https://genode.org/documentation/release-notes/21.08); [Genode porting guide](https://genode.org/documentation/developer-resources/porting_device_drivers)
- **NetBSD rump kernels.** Antti Kantee's "anykernel" design lets NetBSD drivers (for example the TCP/IP stack and file systems) run outside the NetBSD kernel. NetBSD's own tutorial says rump is portable to other systems "although requires some amount of work" — [NetBSD rump tutorial](https://netbsd.org/docs/rump/sptut.html); [FOSDEM 2013](https://archive.fosdem.org/2013/schedule/event/operating_systems_anykernel/). Genode is cited as a user of rump drivers — [Wikipedia: Rump kernel](https://www.wikipedia.org/wiki/Rump_kernel)
- **Fuchsia DFv2.** Drivers are user-space components loaded into "driver hosts" (processes with their own address space). A driver can choose to share a host with its parent. Drivers talk over FIDL on Zircon channels, and the framework is not yet ABI-stable — [Fuchsia DFv2](https://fuchsia.dev/docs/concepts/drivers/driver_framework)
- **Terminal OS's own licence is MIT** ("Copyright (c) 2026 Bhishan Sharma and the Terminal OS contributors"), checked in the repository LICENSE file — local file `C:\Users\hp\Desktop\os\LICENSE` (no URL)

### Inferences
- **Coverage versus effort, judged for one maintainer:**

  | Strategy | Coverage gained | Up-front effort (my estimate) | Ongoing burden | Licence risk for an MIT kernel |
  |---|---|---|---|---|
  | LinuxKPI-style shim plus Linux DRM (FreeBSD model) | Intel and AMD GPUs, many Wi-Fi chips | Very high: FreeBSD needs funded staff and still lags (HT/VHT for mt76/rtw89 unfinished late 2025) | Continuous rebases; one Linux bump stalled a quarter | Mixed. Each imported driver's licence must be checked; any GPL-only file makes the combined binary GPL-bound |
  | Per-driver native port (OpenBSD DRM, iwx) | One family per port | Medium per driver (iwx: about 1–2 quarters for basic a/g) | Manual re-sync; OpenBSD manages about one DRM bump a year with one maintainer | Low if limited to permissively or dual-licensed drivers |
  | Reuse BSD drivers through a FreeBSD compat layer (Haiku model) | Many NICs and Wi-Fi chips | Medium-high once (the layer), then low per driver | Tied to FreeBSD's coverage, which now itself comes partly from LinuxKPI | Low (BSD/ISC) |
  | DDE/rump environments | Selected drivers, stacks, file systems | High per environment | Tied to the source OS version | Rump is BSD; DDE-Linux carries GPL |
  | User-space driver framework (Fuchsia/Redox model) | None by itself; it is an isolation architecture | Very high to retrofit into a monolithic, BKL-based kernel | n/a | n/a |

- **Recommended for Terminal OS:**
  - **Wi-Fi.** Keep the native Intel driver and add HT/VHT/HE. Then port OpenBSD-style or Linux mt76 (ISC-licensed in Linux, to be verified) and rtw89 natively with a thin per-driver shim instead of a general LinuxKPI.
  - **Graphics.** Native Intel display work is the realistic path. A full amdgpu is impractical without a large Linux-compatibility layer: FreeBSD and OpenBSD both reuse Linux's code rather than writing their own.
- **GPL.** Linking GPLv2-only driver code into the MIT kernel would make distributed kernel images subject to GPLv2. Isolating such code in separate user-space driver processes (the Genode/DDE pattern) may reduce but not clearly remove derivative-work concerns. Get legal advice before relying on it (inference, not legal advice).
- **Maintenance is the hidden cost.** The FreeBSD reports show that keeping parity with Linux driver releases (6.16 → 6.17 → 6.19 in about two quarters) is a permanent job. For a single developer, a one-off port of a stable driver version that is then frozen and patched only for bugs is more sustainable than chasing Linux upstream.

### Gaps
- I did not verify per-driver SPDX licences (iwlwifi, mt76, rtw88/rtw89, ath11k/ath12k, i915/xe, amdgpu) in this session; check the `SPDX-License-Identifier` lines before importing anything.
- I found no published size (lines of code) or person-year figures for LinuxKPI, drm-kmod or Haiku's compat layer.
- No current status for Genode's Intel Wi-Fi or graphics ports, or for Redox's user-space driver model.
- OpenBSD's own Qualcomm Wi-Fi driver (a port of ath11k) was not verified here.

## 3. Wireless (HT/VHT/HE, WPA3, 802.1X, regulatory) and Bluetooth stacks (C7, C8)

### Takeaway
Two things are needed to move Terminal OS's Intel Wi-Fi beyond legacy 802.11a/g rates.

**HT/VHT/HE operation:**
- capability advertisement;
- Block Ack/A-MPDU aggregation;
- rate control, which Intel firmware largely handles.

Even FreeBSD's LinuxKPI drivers struggled with this in 2025.

**WPA3-SAE with protected management frames (PMF):** this is required for 6 GHz and Wi-Fi 7.

The cheapest route to WPA3, 802.1X/EAP and the rest of the security handshakes is porting wpa_supplicant, which is BSD-licensed and designed around a driver-wrapper interface. For Bluetooth, Apache-2.0 NimBLE (BLE only) or the Zephyr host are licence-safe. BTstack needs a commercial licence for commercial use, and BlueZ is GPL and tied to Linux.

### Cited Findings
- FreeBSD iwlwifi got 802.11n/ac only for 22000+ chips (in 14.3), and 802.11ax/be and 6 GHz are still "planned" in May 2026 — [iwlwifi(4)](https://man.freebsd.org/cgi/man.cgi?manpath=FreeBSD&query=iwlwifi&sektion=4). In Q4 2025 FreeBSD fixed a firmware crash on 1×1 cards such as the AX101 "with 11n and 11ac". Upstream iwlwifi split its mvm sub-driver into mvm and mld — [FreeBSD Q4 2025 report](https://www.freebsd.org/status/report-2025-10-2025-12/lkpi-wireless/)
- Ported drivers get stuck on rates and aggregation: rtw89 transmit was stuck on basic rates, rtw88 needed a BlockACK fix, and mt76 11n/11ac was still in progress (Q4 2025) — [FreeBSD Q4 2025 report](https://www.freebsd.org/status/report-2025-10-2025-12/lkpi-wireless/)
- wpa_supplicant/hostap is BSD-licensed (advertising clause removed). Hostap chose the BSD alternative for future releases and dropped the GPLv2 option. All hardware- and driver-specific code sits behind a "well-defined driver API" (driver_wrapper), and the WPA logic is portable C — [wpa_supplicant README](https://android.googlesource.com/platform/external/wpa_supplicant_8/+/73b889335132164fdb87c5d8ebccad1bd5620b33/wpa_supplicant/README); [hostap developer docs](https://gitea.auro.re/jeltz/hostap/src/commit/1a9c618d3f465b974a724dc2cb3e4030837e2982/doc/mainpage.doxygen)
- The Wi-Fi Alliance made WPA3 mandatory for 6 GHz and Wi-Fi 7. PMF is mandatory for Wi-Fi 6E, and SAE with Hash-to-Element (H2E) is described as mandatory for 6E — [Cisco WPA3 deployment guide](https://www.cisco.com/c/en/us/products/collateral/wireless/catalyst-9100ax-access-points/wpa3-dep-guide-og.html). WPA2 is not permitted in 6 GHz, and WPA3 is mandatory for Wi-Fi 7 devices — [Meraki WPA3 guide](https://documentation.meraki.com/MR/Encryption_and_Authentication/WPA3_Encryption_and_Configuration_Guide). The H2E requirement comes from Wi-Fi Alliance certification, not the IEEE 802.11ax-2021 standard — [hostap patch discussion](https://patchwork.ozlabs.org/project/hostap/patch/20220407114943.2665222-1-nico.escande@gmail.com/)
- NimBLE (Apache Mynewt) is a portable Bluetooth LE stack under the permissive Apache 2.0 licence, with a host that can be qualified by the Bluetooth SIG. Its controller can also run without the NimBLE host ("blehci"), over UART H4 — [NimBLE, RIOT Summit 2018](https://summit.riot-os.org/wp-content/uploads/sites/10/2018/09/1_1-Szymon-Janc-NimBLE.pdf)
- Zephyr's Bluetooth host is "portable to all architectures supported by Zephyr" and supports host-only setups over UART, SPI and IPC. Zephyr's docs note the host side can also be BlueZ on Linux — [Zephyr Bluetooth overview](https://docs.zephyrproject.org/3.5.0/connectivity/bluetooth/overview.html)
- BTstack's public version uses a BSD-style licence with a non-commercial restriction, and the maintainer offers commercial licences (older mailing-list post) — [btstack-dev](https://groups.google.com/g/btstack-dev/c/nLZzs7tPUhI)

### Inferences
- **Wi-Fi sequence for Terminal OS** (effort figures are my estimates for one experienced developer):
  1. **HT/VHT on the existing Intel driver: about 1–2 months.** Advertise HT/VHT capabilities in the association request, set up the firmware station with HT/VHT parameters, use firmware rate control on 22000+ parts, and handle A-MPDU receive reordering and ADDBA (Block Ack setup).
  2. **HE (802.11ax): about 1 more month.**
  3. **wpa_supplicant port: about 1–2 months.** Write a Terminal OS driver_wrapper, an event loop and a control socket. This brings WPA3-SAE with H2E, PMF, OWE, 802.1X/EAP-TLS/PEAP and roaming in one step and replaces home-grown WPA2 code.
  4. **6 GHz.** Needs SAE-H2E, PMF and regulatory/6 GHz scanning rules.
- **Regulatory.** Reusing the wireless-regdb database plus Intel's firmware-side regulatory logic (LAR) is the usual approach (inference, unverified here).
- **wpa_supplicant's crypto backend.** It needs one. Terminal OS has BearSSL, so check which hostap crypto backends implement the elliptic-curve operations SAE needs. OpenSSL, wolfSSL or mbedTLS may be needed for the supplicant process.
- **Bluetooth.** On Intel combo cards Bluetooth sits on USB (linuxhw's data shows many Intel Bluetooth USB IDs), so the path is a USB HCI transport, Intel firmware patch download, then a host stack. NimBLE fits LE peripherals such as mice and keyboards, but has no BR/EDR, so no A2DP audio (inference from "BLE stack"). Classic audio headsets would need a BR/EDR-capable stack; BTstack (commercial terms) or a BlueZ-derived user-space port (GPL, which is acceptable for a separate user-space daemon) are the candidates.

### Gaps
- I found no primary Wi-Fi Alliance document. The 6 GHz, WPA3 and H2E rules come from vendor guides.
- No data on which hostap crypto backends support SAE without OpenSSL.
- No source on Zephyr host BR/EDR maturity or how hard it is to port the Zephyr host outside Zephyr.
- Not verified: the current BTstack licence terms, and whether the iwlwifi firmware rate-control API differs between the mvm and mld firmware generations.

## 4. Power management: S0ix versus S3, runtime power management, CPU frequency and battery life (C9)

### Takeaway
Many current laptops no longer offer S3 sleep, so "sleep" on new machines means s2idle/S0ix (Modern Standby). S0ix only reaches low power when every device is in a low-power runtime state and the SoC reaches its deepest package state. That makes runtime device power management (including powering off discrete GPUs) the real cost. Microsoft's guidance of under 5% battery loss over a 16-hour idle is a useful target; even Linux on a Framework laptop has been measured at about double that. On AMD, the equivalent of Terminal OS's HWP work is amd-pstate in EPP mode, which needs ACPI CPPC.

### Cited Findings
- Dell's current notebooks have S3 disabled in firmware with no BIOS option to re-enable it. When S3 is unavailable Windows uses S0ix — [openSUSE kernel bug thread](https://lists.opensuse.org/archives/list/kernel-bugs@lists.opensuse.org/thread/I22PQR2XB7GED5VIBQZGDUYD4EQHAOGZ). An HP EliteBook 845 G7 owner reports S3 missing and s2idle buggy under Linux — [HP forum](https://h30434.www3.hp.com/t5/Business-Notebooks/Buggy-s2idle-in-HP-EliteBook-845-G7-Notebook-PC-in-linux/td-p/9558500)
- A Framework 13 (AMD) user measured about 10.4% battery drain over 16 hours in s2idle, against Microsoft's recommendation of under 5% over 16 hours of idle — [Framework community](https://community.frame.work/t/is-framework-13-amd-s2idle-modern-standby-efficient-enough/46431/3)
- FreeBSD is working on S0ix: there is an AsiaBSDCon 2026 paper on it — [AsiaBSDCon 2026 S0ix paper](https://2026.asiabsdcon.org/media/entry/question_uploads/asiabsdcon_s0ix_paper_xv1utxb.pdf). Sleep states are part of the funded FreeBSD laptop project — [LWN](https://lwn.net/Articles/1051394/)
- Linux 6.5 made amd-pstate's "active" EPP mode the default on Zen 2 and newer with ACPI CPPC. EPYC servers stay on acpi-cpufreq, and a Kconfig option (X86_AMD_PSTATE_DEFAULT_MODE) sets the default — [Phoronix](https://www.phoronix.com/news/Linux-6.5-Power-Management). In active mode only the powersave and performance governors exist, and both map to an EPP hint for the CPU's internal governor — [ZenWatt docs](https://docs.teejeetech.com/zenwatt/pages/cpu-drivers.html)
- Even very mature ports leave out sleep for a while: Asahi Linux's official M3 support (September 2026) still has no sleep because DCP (the display coprocessor) is not supported — [Phoronix](https://www.phoronix.com/news/Asahi-Linux-Official-M3)

### Inferences
- **S0ix prerequisites on Terminal OS's ACPI base (uACPI):**
  - call the LPS0 device-specific methods (`_DSM`) on entry and exit;
  - put every PCI device into D3hot or D3cold (`_PR3` power resources) with PCIe ASPM L1 substates enabled;
  - quiesce xHCI, NVMe (APST or a host-controlled power state), audio, Wi-Fi (firmware D3 commands) and the GPU (display off, render power gating);
  - check that package C10 and the SLP_S0 residency counters are actually reached.

  Effort: about 3–6 months of part-time work on a few reference laptops (my estimate, based on Linux and FreeBSD still debugging per model).
- **Plan the order of work:**
  1. Runtime power management of idle devices first. It saves power while awake, which helps battery life anyway.
  2. Measure package C-state residency.
  3. Then s2idle.
  4. Keep S3 as a fallback where the firmware still offers it.
- **CPU frequency.** HWP is already present. Next: AMD CPPC/EPP (MSR- or shared-memory-based), and energy-aware choices in the scheduler such as idle-core packing.
- **Thermal management.** Use ACPI thermal zones and passive cooling, plus vendor fan-control interfaces through ACPI or the EC (inference; not researched in depth).
- **Mapping to the framework.** For C9, the 7–8 band needs reliable suspend and resume on common laptops and idle power within about 1.5× of Windows. That is the main gap a funded team (FreeBSD) is still working on in 2026.

### Gaps
- I found no authoritative 2025–2026 data on the share of laptops without S3, or on how Linux battery life compares with Windows.
- I did not retrieve Intel's or AMD's S0ix debugging guidance, or the intel_pstate/HWP kernel documentation.
- The FreeBSD S0ix paper's contents were not read, only its existence confirmed.

## 5. Exploit mitigations and platform security: checklist, cost and when to adopt (C5, C3, C4)

### Takeaway
The Kernel Self-Protection Project (KSPP) list is the practical reference checklist. Mitigations that depend on how the kernel is built (W^X/strict RWX, NX, SMEP/SMAP/UMIP, stack protector, guard pages, zeroing allocations, freelist hardening, a single user-copy path, a single DMA API) are cheap in a young kernel and expensive to retrofit once code assumes otherwise. Hardware CFI is a two-speed case: kernel IBT is cheap early, but user shadow stacks took Linux about five years. KPTI-class transient-execution mitigations, IOMMU enforcement, driver isolation and memory-safe languages are larger projects. Secure updates (TUF) and capability-style sandboxing (pledge/unveil, as SerenityOS did) are affordable early wins.

### Cited Findings
- KSPP's recommended settings, grouped — [KSPP Recommended Settings](https://kspp.github.io/Recommended_Settings):
  - **Memory layout and permissions:** STRICT_KERNEL_RWX and DEBUG_WX, STRICT_MODULE_RWX, PAGE_TABLE_ISOLATION, RANDOMIZE_BASE and RANDOMIZE_MEMORY, VMAP_STACK (guard pages around kernel stacks), a minimum mmap address of 65536, and no legacy vsyscall.
  - **Allocator hardening:** HARDENED_USERCOPY, SLAB_FREELIST_RANDOM/HARDENED, RANDOM_KMALLOC_CACHES and SLAB_BUCKETS, no slab merging, a shuffled page allocator, FORTIFY_SOURCE, KFENCE, and full refcount checks.
  - **Initialisation and wiping:** INIT_ON_ALLOC and INIT_ON_FREE, INIT_STACK_ALL_ZERO, ZERO_CALL_USED_REGS, stack wiping on syscall exit, and randomised kernel stack offsets.
  - **Control flow:** STACKPROTECTOR_STRONG, CFI_CLANG (kCFI), X86_KERNEL_IBT, X86_USER_SHADOW_STACK, and straight-line-speculation mitigation.
  - **Runtime checks:** hardened lists, BUG_ON_DATA_CORRUPTION, UBSAN bounds checks with trap, and panic on oops.
  - **Attack-surface reduction:** no /dev/mem or kmem, no ACPI_CUSTOM_METHOD, no kexec or hibernation, forced module signing, and seccomp.
  - **Access control:** Yama, Landlock, and Lockdown in confidentiality mode.
  - **DMA:** an IOMMU in strict mode with passthrough off, and EFI_DISABLE_PCI_DMA.
  - **SMT:** core scheduling for SMT.
- Linux merged kernel IBT (Intel CET indirect-branch tracking) in 5.18. User-space shadow stacks were turned down for 6.4 because of code problems and merged in 6.6 under X86_USER_SHADOW_STACK — [Phoronix (6.6)](https://www.phoronix.com/news/Intel-Shadow-Stack-Linux-6.6); [Phoronix (6.4 attempt)](https://www.phoronix.com/news/Linux-6.4-Shadow-Stack-Coming). Shadow-stack patches date back to June 2018 — [LKML 2018](https://lkml.iu.edu/hypermail/linux/kernel/1806.0/05373.html) — and reached v26 in 2021 — [lore v26](https://lore.kernel.org/all/YIpgB5HbnNPWX4FP@grain/T/). Building needs Binutils 2.29 or LLVM 6, and `nousershstk` turns it off at boot — [kernel CET docs patch](https://lkml.rescloud.iu.edu/2307.2/05298.html)
- Rust in Linux: on 13 December 2025 the "Rust experiment" was declared concluded and the experiment section was removed from the docs, with Ojeda noting Rust is already used in production. LWN reported Rust code in the kernel grew about five-fold in a year. The kernel aims to always build with the Rust version in Debian stable — [LWN patch](https://lwn.net/Articles/1050308/); [Patchew](https://patchew.org/linux/20251213000042.23072-1-ojeda@kernel.org); [LWN session report](https://lwn.net/SubscriberLink/1050174/6b6d55c90ce1100f)
- Rust in Windows: Microsoft ships windows-drivers-rs and cargo-wdk. Much driver code still needs `unsafe` Rust, and some safe kernel abstractions already ship in the Windows kernel — [Microsoft Windows driver blog, Sept 2025](https://techcommunity.microsoft.com/blog/windowsdriverdev/towards-rust-in-windows-drivers/4449718). `win32kbase_rs.sys` is a Rust part of the GDI kernel component — [Techzine](https://www.techzine.eu/news/devops/108978/microsoft-releases-more-rust-features-for-windows-kernel/). Check Point found a denial-of-service bug in that Rust GDI code (via NtGdiSelectClipPath), fixed in the May 2025 preview update — [GBHackers](https://gbhackers.com/windows-gdi-vulnerability-in-rust-kernel/amp/)
- TUF (The Update Framework) protects users even when the repository or signing keys are compromised. It graduated from the CNCF in December 2019 and is used by Amazon, Microsoft, Google, Cloudflare and Docker; its Uptane variant is used in automotive over-the-air updates — [TUF](https://theupdateframework.io/); [LWN](https://lwn.net/Articles/807777); [Help Net Security](https://www.helpnetsecurity.com/2019/12/23/update-framework-linux-foundation/)
- SerenityOS, a hobby-origin OS, implements OpenBSD-style `pledge` and `unveil`; both appear among the syscalls moved off its big lock in 2022–2023 — [SerenityOS syscall commit history](https://git.rgbcu.be/RGBCube/serenity/commits/commit/b3b51a5001c2920e30a6d4549422278602357cac/Kernel/Syscalls?page=2)
- Microsoft's UEFI signing rules require NX compatibility (4 KB section alignment) for all submissions from 30 November 2022, with an exception for shim — [Microsoft file-signing requirements](https://learn.microsoft.com/windows-hardware/drivers/dashboard/file-signing-reqs); [Microsoft UEFI signing requirements blog](https://techcommunity.microsoft.com/blog/hardwaredevcenter/updated-uefi-signing-requirements/1062916)
- A Microsoft representative warned that delivering db updates through firmware updates causes PCR7 (the TPM's Secure Boot measurement) mismatches, and said the OS should update db instead. This matters for TPM-sealed disk encryption — [Microsoft Tech Community (comments)](https://techcommunity.microsoft.com/t5/hardware-dev-center/signing-with-the-new-2023-microsoft-uefi-certificates-what/ba-p/4455787)

### Inferences
- **Checklist with cost and timing.** Costs are my estimates for a C/NASM monolithic kernel with its own UEFI loader. The CPU-generation notes are approximate and need checking.

  | Mitigation | Cost now | Cost to retrofit later | Notes |
  |---|---|---|---|
  | NX on all data and stacks; kernel W^X (text RX, rodata R, data RW NX); user W^X | Days | Weeks (code that patches itself, trampolines, JITs) | Also required for any Microsoft-signed UEFI image (NX attestation) |
  | SMEP and UMIP (CR4 bits) | Hours | Low | Present on all CPUs Terminal OS targets (approximately Ivy Bridge+/Zen+ for SMEP) |
  | SMAP (stac/clac around user memory) | Days, if all user access goes through copy_from/to_user | Weeks to months if syscall code dereferences user pointers directly | Do it before mlibc and Ladybird widen the syscall surface |
  | Stack protector (-fstack-protector-strong) with per-CPU or per-task canary | Days | Low | Needs canary setup in early boot and on AP startup |
  | Kernel stack guard pages | 1–2 weeks | Moderate | Needs a kernel virtual-memory area; turns stack overflows into clean faults instead of silent memory corruption |
  | Zero on allocate/free, freelist pointer hardening, randomised slab layout, hardened user copy | 1–2 weeks in the in-house allocator | Moderate | The cheapest heap-exploit friction |
  | KASLR (loader picks a random base; relocatable or PIE kernel) | 2–4 weeks | Grows with hard-coded addresses (asm, linker script) | The custom UEFI loader is the natural place to do it |
  | User ASLR (stack, mmap, heap, PIE binaries) | 1–3 weeks | Low to moderate | Depends on PIE support in the mlibc and binutils toolchain |
  | Kernel IBT (endbr64 via -fcf-protection=branch; enable in S_CET) | 1–2 weeks | Grows with every hand-written asm indirect-branch target | CET-capable CPUs: approximately Tiger Lake+/Zen 3+ |
  | kCFI (clang -fsanitize=kcfi) | Weeks (needs a clang build, typed indirect calls) | Grows with function-pointer casts | KSPP-recommended; a GCC equivalent is newer |
  | User shadow stacks | Months (XSAVES CET state, signals, threads, fork) | High | Linux took about 2018–2023; defer |
  | Transient-execution mitigations: Spectre v1 barriers, eIBRS/retpoline, IBPB on switch, RSB fill, MDS VERW, SLS | Weeks, with ongoing audits | Moderate | On modern CPUs eIBRS plus microcode covers much of it |
  | KPTI | Weeks to months (separate user page tables, entry trampoline) | Moderate-high | Only needed on Meltdown-affected (older Intel) CPUs, which can be detected |
  | IOMMU DMA protection (VT-d/AMD-Vi) | Months | High unless all DMA already goes through one DMA API | Route every DMA allocation through one API now, enforce later. Needed against Thunderbolt/USB4 DMA attacks |
  | pledge/unveil-style syscall and path restriction | 2–4 weeks | Grows with syscall count | SerenityOS precedent; best fit for an int 0x80 ABI |
  | Capability fds (Capsicum-like) or handle-based IPC | Months | Very high in a POSIX-first design | Consider only for new IPC paths (window server, IPC) |
  | Mandatory access control | Months | High | Low value for a single-user desktop at this stage |
  | Full-disk encryption (block-layer AES-XTS, Argon2/PBKDF2, optional TPM sealing) | 2–3 months | Moderate | Seal to PCR7 plus a boot-manager PCR (watch the db-update PCR7 issue above) |
  | TPM 2.0 measured boot (loader extends PCRs via EFI_TCG2_PROTOCOL; kernel CRB/TIS driver) | 3–6 weeks | Low | Prerequisite for sealed FDE and remote attestation |
  | Signed, TUF-style updates with rollback protection | 1–2 months | High once an unsigned update channel exists | Start before the first public package repository |
  | Memory-safe language for new drivers and parsers | High (FFI, toolchain, allocator integration) | Same | Linux needed about 2020–2025 to declare Rust permanent. A cheaper start is Rust or bounds-checked C for network, filesystem and image parsers |

- **Priority order for Terminal OS's situation** (ring-3 processes, BKL, HTTPS networking, Ladybird port in progress):
  1. NX/W^X, SMEP/SMAP/UMIP, stack protector and guard pages.
  2. Allocator hardening and zeroing.
  3. KASLR and ASLR.
  4. A pledge/unveil-style sandbox for the browser and network-facing processes.
  5. Kernel IBT.
  6. Signed updates.
  7. TPM measured boot and FDE.
  8. IOMMU.

  The Ladybird browser running remote content is the largest attack surface. Process sandboxing for its WebContent processes matters more than most kernel hardening.
- **Rust is not a guarantee.** The Windows Rust GDI denial of service shows that memory safety turns memory corruption into panics, which are denial-of-service bugs rather than code execution.

### Gaps
- I found no quantified cost figures (person-months) for SMAP or KPTI retrofits in hobby kernels.
- No 2025–2026 data on transient-execution mitigation overheads (for example Retbleed or BHI) was retrieved.
- The CPU generations for SMEP, SMAP, UMIP and CET were not re-verified in this session.
- FORTIFY_SOURCE needs libc support; mlibc's FORTIFY status is unknown.

## 6. Booting with Secure Boot enabled in 2026: options, requirements and the 2011→2023 certificate transition (C1, C5, C16)

### Takeaway
Since 26–27 June 2026, Microsoft signs third-party UEFI binaries (including shim) only with the Microsoft UEFI CA 2023. A newly signed Terminal OS boot path would therefore boot only on PCs whose db already contains the 2023 third-party certificate. Secured-core PCs may not trust any third-party CA by default.

Getting a shim signed requires all of the following:
- a legal entity;
- an EV code-signing certificate and a Microsoft Hardware Dev Center account;
- two security contacts verified by PGP;
- a reproducible Docker build;
- keys held in a hardware security module (HSM);
- SBAT metadata;
- NX compliance;
- a kernel that enforces "lockdown";
- typically a 2–3-month queue at the volunteer shim-review board, whose reviewers know GRUB2 and systemd-boot best.

A non-shim loader signed directly by Microsoft now needs an annual third-party security audit. The realistic sequence for Terminal OS:
1. Support user-enrolled keys now.
2. Optionally offer a MOK-based chain through an existing signed shim.
3. Pursue its own shim review only once it has an organisation and a kernel that enforces lockdown.

### Cited Findings
- Expiry dates and replacements — [Microsoft KB5062710](https://support.microsoft.com/kb/5062710):
  - Microsoft Corporation KEK CA 2011 expired 24 June 2026; replaced by Microsoft Corporation KEK 2K CA 2023 (in KEK; signs db/dbx updates).
  - Microsoft UEFI CA 2011, which signs third-party boot loaders and option ROMs, expired 27 June 2026; replaced by Microsoft UEFI CA 2023 for boot loaders and EFI applications and Microsoft Option ROM UEFI CA 2023 for option ROMs.
  - Microsoft Windows Production PCA 2011 expired 19 October 2026; replaced by Windows UEFI CA 2023.
  - Devices without the 2023 certificates "will continue to start and operate normally" but may miss new early-boot protections, and the expiry "may affect scenarios that rely on Secure Boot trust, such as … third-party bootloaders".
- How signing changed — [Microsoft Tech Community: Signing with the new 2023 Microsoft UEFI certificates](https://techcommunity.microsoft.com/t5/hardware-dev-center/signing-with-the-new-2023-microsoft-uefi-certificates-what/ba-p/4455787):
  - From 20 October 2025, each approved submission returned two separately signed binaries (2011 CA and 2023 CA). Microsoft does not issue dual-signed binaries.
  - Update of 26 June 2026: "Approved Signing Submissions now only return binaries signed with the 2023 UEFI CA."
  - Installers should check db for the 2023 certificates and fall back to the 2011-signed binary.
  - Binaries already signed with the 2011 CA remain trusted where db still holds that CA, and a Microsoft representative said (23 March 2026) there are "no plans to revoke the 2011 UEFI CA using the DBX".
  - From October 2025, third-party UEFI CA submitters must pass an annual security audit by a Microsoft-approved independent firm. Shim submissions satisfy this through the Shim Review Board unless the board flags risks such as proprietary non-shim code.
  - Some devices, such as Secured-core PCs, may not trust third-party CAs by default.
- Shim-review requirements — [rhboot/shim-review README](https://github.com/rhboot/shim-review):
  - Start from the shim 16.1 release tarball.
  - Provide a Dockerfile so that `docker build .` reproduces the exact binary.
  - Name two security contacts and verify both through a PGP-encrypted email challenge.
  - Be verifiable "as a legal entity, to prevent abuse".
  - Embed certificates with RSA keys of at least 4096 bits, and describe how keys are protected (HSM, smartcard, air-gapped vault).
  - Include SBAT entries for every binary shim loads.
  - Add old vulnerable hashes to the vendor DBX.
  - Confirm the whole boot stack is NX-compatible if NX is set.
  - Explain how the signed kernel enforces lockdown under Secure Boot: "If it does not, we are not likely to sign your shim." Kernel modules must be signed with an ephemeral key (or loading across builds otherwise prevented), and the chain must not load unsigned kernels.
  - A wait of 2–3 months is "reasonable". Reviewers are volunteers, and submitters are expected to review others' submissions.
  - Microsoft signing is limited to the UEFI CA 2023 from 27 June 2026.
  - The review exempts submitters from the yearly audit only if shim hands off to open-source boot loaders. Reviewers mainly know GRUB2 and systemd-boot, so other loaders need more justification.
- Microsoft's UEFI submissions require an EV certificate and an Azure AD (Entra ID) account on the Hardware Dev Center. A shim must clear shim review before Microsoft signs it — [Microsoft file-signing requirements](https://learn.microsoft.com/windows-hardware/drivers/dashboard/file-signing-reqs)
- EV certificate cost evidence is old: the cheapest EV certificate OSR found in 2015 was about $450 — [OSR blog](https://www.osr.com/blog/2015/03/18/microsoft-signatures-required-km-drivers-windows-10/). DigiCert has delivered EV certificates on a SafeNet eToken USB hardware token — [iPXE appnote](https://www.ipxe.org/appnote/etoken)

### Inferences
- **Options for Terminal OS, cheapest first:**
  1. **User-enrolled keys (custom PK/KEK/db).** No cost. The installer or docs guide the user through firmware "Setup Mode" and enrol a Terminal OS key; the loader and kernel are signed with it.
     - Keep the Microsoft 2023 certificates in db for Windows dual-boot and for GPU and other option ROMs. KB5062710 notes that systems needing option ROMs can add the Option ROM UEFI CA 2023, and removing Microsoft certificates can stop option ROMs from loading.
     - This works today on most PCs and gives genuine protection for the user's own machine.
  2. **MOK via an existing Microsoft-signed shim.** Chain-load Terminal OS's loader signed with a project key that the user enrols in MokManager at the next boot, which requires being physically present. Free, but:
     - shim enforces SBAT, so Terminal OS's loader needs a `.sbat` section;
     - the shim vendor's policies apply;
     - after June 2026 it only works on firmware that trusts whichever CA signed that shim.

     USB multiboot tools have used this pattern (not verified in this session).
  3. **Own shim through shim-review.** This needs:
     - an organisation (legal entity);
     - an EV certificate on a hardware token, plus Partner Center/Entra ID;
     - an HSM or smartcard key process;
     - two PGP-verified contacts;
     - reproducible builds;
     - SBAT and dbx hygiene;
     - a kernel lockdown policy: no unsigned ring-0 code, signed or no loadable modules, no /dev/mem writes, no kexec of unsigned kernels, no ACPI table or method injection from user space (the KSPP attack-surface list is the template).

     Rough cost: hundreds to low thousands of US dollars a year for the EV certificate, months of engineering, a 2–3-month review, and ongoing peer-review duty.
  4. **Direct Microsoft signing of Terminal OS's own (non-shim) loader.** It now triggers the annual independent audit, which is unrealistic for a volunteer project.
- **The 2026 transition hurts newcomers more than incumbents.** Existing distributions keep 2011-signed shims that still boot on old machines, while any new shim gets only the 2023 signature. Terminal OS would need the 2023 Microsoft UEFI CA in db. Windows updates are adding the 2023 certificates on Windows machines. On machines without Windows, Terminal OS would have to apply Microsoft's KEK-signed db update itself through authenticated variables, which is what Microsoft expects of the OS rather than firmware. This mechanism is an inference that needs checking.
- **Measured boot is independent of Secure Boot signing.** The loader should extend TPM PCRs (EFI_TCG2_PROTOCOL) and log events whatever the signing route, so FDE sealing and attestation can come later.
- **Recommendation.** Ship option 1 (with a signing tool in the SDK, part of C12) and document option 2. Plan option 3 only when C17 governance (a legal entity) and C5 lockdown are in place. In the meantime, require NX compliance and 4 KB section alignment in the loader build now so it is signable later.

### Gaps
- I could not confirm whether Windows Update adds the third-party Microsoft UEFI CA 2023 only on devices whose db already contains the 2011 third-party CA; KB5062710 does not say.
- No current (2026) EV certificate pricing or Hardware Dev Center fees were found.
- No data on how many shipped PCs had the 2023 third-party CA in db by November 2026.
- Not verified: whether shim-review would accept a non-Linux OS kernel at all, and the exact SBAT rules shim applies to MOK-signed second stages.

## 7. Quality engineering: fuzzing, sanitizers, static analysis, CI, conformance and reproducibility (C13, C12)

### Takeaway
The highest-yield, lowest-cost steps for a one-person kernel are:
- **Host-side fuzzing of parsers.** Build the FAT32, ext2/3, TCP/IP, DHCP/DNS, USB descriptor and image parsers for the host and run them under libFuzzer/AFL++ with ASan/UBSan. SerenityOS does this through its "Lagom" host build on OSS-Fuzz.
- **An in-kernel UBSan runtime**, which needs only handler functions.
- **QEMU/OVMF boot tests on every commit.**
- **Coverity Scan**, which is free.

Full-system fuzzers are possible but cost more:
- **syzkaller** officially supports seven OSes and needs a per-OS executor, syscall descriptions, VM/image support and an SSH-reachable guest.
- **kAFL/Nyx** is OS-agnostic and works with anything that runs in QEMU/KVM, but needs Intel PT hardware and a patched host kernel.

KASAN-style shadow memory is noticeably harder; SerenityOS's first attempt crashed at boot.

### Cited Findings
- syzkaller's README lists FreeBSD, Fuchsia, gVisor, Linux, NetBSD, OpenBSD and Windows as supported, and mentions Darwin/XNU and Starnix — [google/syzkaller](https://github.com/google/syzkaller)
- Adding a new OS to syzkaller requires — [syzkaller "adding new OS support" doc (Fuchsia mirror)](https://fuchsia.googlesource.com/third_party/syzkaller/+/refs/heads/upstream/master/docs/adding_new_os_support.md):
  - an `executor/executor_GOOS.h` for OS-specific setup and syscall execution, wired into `executor.cc`;
  - registration in `pkg/build`, plus per-OS image building and SSH keys;
  - a crash-report parser under `pkg/report`;
  - a supported-syscall check under `pkg/host`;
  - target definitions under `sys/GOOS/init.go`, plus registration in `syz-extract`, `sys/targets` and `vm/qemu`;
  - syscall descriptions and pseudo-syscalls.
- kAFL (USENIX Security 2017) fuzzes kernels independently of the OS by combining a hypervisor with Intel Processor Trace for coverage — [USENIX](https://www.usenix.org/conference/usenixsecurity17/technical-sessions/presentation/schumilo). Nyx uses a modified QEMU and KVM that takes and resets VM snapshots thousands of times per second. It suits "anything that runs inside a QEMU/KVM VM", including kernels and full operating systems, and Intel's maintained kAFL now uses Nyx as its backend. It needs a 6th-generation or newer Intel CPU and a patched host kernel — [nyx-fuzz](https://github.com/nyx-fuzz/nyx)
- SerenityOS fuzzes its libraries on the host via Lagom, using clang with ASan recommended. Fuzzers named `Fuzz*` run automatically on OSS-Fuzz — [Lagom ReadMe](https://git.rgbcu.be/RGBCube/serenity/raw/commit/da170997d5754880255e2527a861aed304ed76ee/Meta/Lagom/ReadMe.md). In 2021 its CI enabled UBSan for tests run on the target OS — [SerenityOS commits](https://git.rgbcu.be/RGBCube/serenity/commit/29e4dc76341a4592682271d3955efe992967c3e7)
- SerenityOS's first kernel KASAN integration (2021) only stubbed the `__asan_load/store` callbacks and crashed at boot with a triple fault in strlen. The remaining work was intercepting the loads and stores, picking a shadow-memory mapping, and hooking the allocators — [SerenityOS AddressSanitizer.h history](https://git.rgbcu.be/RGBCube/serenity/commits/commit/a68d31debd103695c9d97e8db5b8a317d185ed35/Kernel/AddressSanitizer.h); [AddressSanitizer.cpp](https://tangled.org/jcs.org/serenity/blob/master/Kernel/AddressSanitizer.cpp)
- Coverity Scan is free for open-source C/C++ (and other languages). Projects under 100K lines can submit up to 28 builds a week (at most 4 a day); projects over 1M lines get 7 a week (1 a day) — [Coverity Scan](https://scan.coverity.com/); [FAQ](https://scan.coverity.com/faq)
- The Open POSIX Test Suite is GPL-licensed and meant to be usable on any POSIX OS, though development and testing happen mainly on Linux; Emscripten maintains a fork — [Open POSIX Test Suite](https://posixtest.sourceforge.net/); [README](https://grok.dragonflybsd.org/source/xref/dports/misc/posixtestsuite/posixtestsuite/README); [Emscripten fork](https://awesome.ecosyste.ms/projects/github.com%2Femscripten-core%2Fposixtestsuite)
- The shim-review process treats reproducible builds (a Docker build that recreates the exact signed binary) as a hard requirement for boot components — [rhboot/shim-review](https://github.com/rhboot/shim-review)

### Inferences
- **Affordable quality stack for Terminal OS, in order:**
  1. **Every-commit CI.** Build, boot in QEMU with OVMF (UEFI firmware for QEMU) on 1, 2 and 4 vCPUs, and check serial-log markers: reaching login, running a test runner, clean shutdown. Add device-model variants: AHCI and NVMe; e1000, rtl8139 and virtio-net; xHCI with usb-tablet; intel-hda.
  2. **A user-space test runner** run inside the guest, covering the syscall surface (fork, exec, mmap, signals, pipes, threads) and the file systems (fsck after crash tests).
  3. **Host-built libFuzzer harnesses** for the FAT32, ext2/ext3 and journal replay code, the TCP/IP input path, DHCP/DNS, the HTTP client, the image and audio decoders and the USB descriptor parser. OSS-Fuzz accepts open-source projects (SerenityOS precedent).
  4. **A kernel UBSan runtime.** About 1–2 days to write the `__ubsan_handle_*` handlers.
  5. **A lock-order checker** (FreeBSD WITNESS-style) and per-CPU assertions. Do this before BKL removal starts (section 9).
  6. **KASAN later.** About 3–6 weeks: shadow memory mapped in the higher half, allocator redzones and quarantine.
  7. **syzkaller only after** an SSH server (for example a dropbear port), a stable syscall ABI and a KCOV-like coverage export exist. That is roughly 1–3 months of porting, and it pays off once mlibc and Ladybird stress the syscall surface.
  8. **kAFL/Nyx** as an alternative for fuzzing specific drivers or parsers in the guest, on an Intel PT host.
- **Hardware-in-the-loop on a budget.** Use 3–5 reference laptops (one Intel 11th–14th gen, one Core Ultra, one AMD Ryzen with a MediaTek card, one with Realtek Wi-Fi). Boot them over the network or from USB, power-cycle them with a smart plug, and capture logs over USB serial, netconsole or a framebuffer camera. Run them nightly rather than per commit.
- **Crash reporting.** Write panic reports to serial and to an EFI variable or reserved RAM ("pstore"-like), then upload them at the next boot with user consent. This replaces "it froze" bug reports with stack traces.
- **Conformance.** Run the Open POSIX Test Suite once mlibc is stable, as a separate GPL program, which does not affect the MIT kernel. Track the pass rate as a C4 metric.
- **Reproducible builds.** Pin the toolchain in a container, as the shim-review model requires. This also makes CI results comparable over time.

### Gaps
- I did not verify: LTP's portability to non-Linux systems, current OSS-Fuzz acceptance rules for hobby OSes, GitHub Actions KVM availability and limits, CodeQL's free tier for public repositories, KCSAN-equivalent implementations in hobby kernels, and managarm's sanitizer support.

## 8. Benchmarking against Linux: methodology and tools (C14)

### Takeaway
Benchmark Terminal OS against Linux on the same hardware and settings: same firmware, CPU frequency policy, SMT and mitigation settings, and both on bare metal or both on KVM with the same device models. Use a small fixed suite:
- lmbench microbenchmarks: `lat_syscall` for system call cost and `lat_ctx` for context switches;
- boot-to-login time;
- fio for storage;
- iperf3 for networking;
- application-level timings once mlibc and Ladybird run.

lmbench reports latencies in microseconds per operation with built-in repetitions (11 by default), which makes it a natural cross-OS baseline once it builds against mlibc.

### Cited Findings
- lmbench is a suite of micro-benchmarks for basic OS and hardware metrics (bandwidth, latency and others) by Carl Staelin and Larry McVoy. `lat_ctx` measures context switching while varying the number and size of processes, and `lat_syscall` measures the cost of entering the kernel. Latencies are mostly in microseconds per operation, the default is 11 repetitions, and the docs warn that comparing bandwidth figures needs care about what is being measured — [lmbench(8)](https://manpages.ubuntu.com/manpages/xenial/lmbench.8.html)
- Phoronix shows the value of reproducible cross-OS testing on identical hardware: its year-end 2025 Snapdragon X Elite Linux review reported uneven, model-dependent results — [Phoronix](https://www.phoronix.com/review/snapdragon-x-elite-linux-eoy2025)

### Inferences
- **Suggested "Terminal OS vs Linux" scorecard** (same laptop, the same mitigations where Terminal OS has them, and an ext2 or FAT32 test partition on both):
  - **Boot time:** firmware hand-off to kernel entry, kernel entry to login, login to desktop. Take TSC timestamps at stage boundaries and compare with Linux `systemd-analyze`.
  - **Microbenchmarks:**
    - lmbench `lat_syscall` (null and read);
    - `lat_ctx` with 2, 8 and 16 processes;
    - `lat_pipe`, `lat_proc` (fork, exec, shell);
    - `lat_pagefault` and `lat_mmap`;
    - `bw_mem` to check that memory bandwidth is not OS-bound.
  - **Storage:** fio with 4K random read/write at queue depth 1 and 32, and 1M sequential, on NVMe, with direct I/O where supported.
  - **Network:** iperf3 TCP over the RTL8168 or e1000 link, and through virtio-net in VMs.
  - **Desktop:** time to first frame, compositor frame time (p50 and p99) and input-to-photon latency if measurable; later, Ladybird page-load times.
  - **Power:** idle package power, plus video-playback and web-browsing battery tests (section 4).
- **System call path.** Terminal OS uses `int 0x80`. Measuring `lat_syscall` against a SYSCALL/SYSRET path quantifies the benefit of switching, though SYSRET has known safety pitfalls (non-canonical return addresses) to handle (inference).
- **Multi-core scaling.** Because Terminal OS runs under a BKL, also report scaling curves with 1, 2, 4 and 8 parallel `lat_syscall`, fork or file-I/O workers. These show the BKL's cost directly and track progress as it is removed (section 9).
- **Process.** Use the Phoronix Test Suite on the Linux side for reproducible automation. Publish the raw results and configuration so others can check them.

### Gaps
- No primary sources were retrieved for fio, iperf3 or the Phoronix Test Suite, or for published lmbench numbers from hobby OSes such as SerenityOS, Haiku or Redox.

## 9. SMP scalability: how big-kernel-lock removal was done elsewhere (C2, C14)

### Takeaway
Removing a big lock took years at every scale:
- **Linux:** the BKL dated from early SMP support and was finally removed in 2.6.39 (2011) after a concentrated 2010–2011 push led by Arnd Bergmann.
- **FreeBSD:** SMPng shipped in 5.0 (2003), was largely complete in 6.0, and Giant lingered in some drivers into the 8.x/9.x era.
- **OpenBSD:** in 2025 it is still pushing its KERNEL_LOCK down and unlocking the network stack.
- **SerenityOS:** the most transferable precedent for a small project. It instrumented every syscall with a "needs big lock" flag and removed syscalls from the lock one by one when their data already had finer locks.

Terminal OS should copy that method: measure, flag per syscall, start with leaf subsystems, keep a lock-order checker, and tackle the scheduler, memory manager, VFS and network stack as separate milestones.

### Cited Findings
- **Linux.** LWN described BKL removal as on the kernel's to-do list "almost since the lock was added for multiprocessor support", with hundreds of call sites left before Bergmann took on the job — [LWN 424657](https://lwn.net/Articles/424657):
  - March 2010: Bergmann announced a `bkl-removal` git branch with a 44-patch series — [LWN 380174](https://lwn.net/Articles/380174).
  - September 2010: coverage of the 2.6.37 target says the work included confirming that none of the 191 `default_llseek()` users needed the BKL — [LWN 406246](https://lwn.net/Articles/406246/).
  - 2.6.37 allowed building without the BKL, and 2.6.39 removed it — [Linux.com](https://www.linux.com/?p=504532).
  - The final commit "BKL: That's all, folks" credits many contributors — [commit 4ba8216c](https://android-kvm.googlesource.com/linux/+/4ba8216cd90560bc402f52076f64d8546e8aefcb%5E%21).
- **FreeBSD.** Before SMPng, FreeBSD 3.x/4.x ran kernel code on one CPU at a time under the Giant lock. SMPng shipped in FreeBSD 5.0 (January 2003), and 6.0 is generally treated as the point where most major subsystems were done. Some old drivers and file systems still needed Giant afterwards — [FreeBSD SMPng document (reviews.freebsd.org)](https://reviews.freebsd.org/R9%3Af73fd9a4b32c1e61e64e7c52fedd4d6ddfb5da23?diff=1). A 2012 thread notes that removing Giant entirely was an "overly ambitious" 5.0 goal, and that 8.2/9.0 still had Giant-locked drivers — [freebsd-hackers, January 2012](https://lists.freebsd.org/pipermail/freebsd-hackers/2012-January/037498.html)
- **OpenBSD 7.7 (28 April 2025).** TCP send and receive take only a shared netlock, `accept(2)` is unlocked for TCP, and TCP output and timers run in parallel. TCP input still takes the exclusive netlock. Signal delivery was unlocked with a mutex, and `KERNEL_LOCK()` calls were pushed down into `namei(9)` — [OpenBSD 7.7 announcement](https://ftp.riken.jp/OpenBSD/7.7/ANNOUNCEMENT). At EuroBSDCon 2025, Alexander Bluhm said the socket API is unlocked so threads can make socket calls in parallel on different CPUs — [EuroBSDCon 2025 talk](https://events.eurobsdcon.org/2025/talk/FGMYP9/)
- **SerenityOS.** All syscalls originally ran under `Process::m_big_lock`, "an obvious bottleneck". A commit added per-syscall instrumentation so the lock could be dismantled gradually — [SerenityOS commit 165a23b68c](https://github.com/SerenityOS/serenity/commit/165a23b68c). In 2022, calls such as `socketpair`, `pipe`, `getppid`, `unveil` and `msyscall` were moved off the lock because their data, such as the fd table, was already protected by mutexes. In 2023 more calls (thread and process names, `pledge`, inode watchers) followed, using spinlocks and mutexes — [SerenityOS syscall commit history](https://git.rgbcu.be/RGBCube/serenity/commits/commit/b3b51a5001c2920e30a6d4549422278602357cac/Kernel/Syscalls?page=2)

### Inferences
- **Method for Terminal OS** (a solo developer, kernel smaller than SerenityOS):
  1. **Instrument.** Measure time spent waiting for the BKL per syscall and per interrupt path, and add a per-syscall `needs_bkl` flag in the int 0x80 dispatch table, as SerenityOS did.
  2. **Add safety tooling before unlocking anything.** Lock-order (WITNESS-like) checking, "lock held" assertions, and the multi-core QEMU CI runs from section 7.
  3. **Unlock the easy, high-volume syscalls first.** Those whose state is per-process or already guarded: getpid, clock and time calls, pipe and socket I/O on private buffers, futex-style waits.
  4. **Make the scheduler per-CPU.** Per-CPU run queues, IPI-based wakeups and load balancing remove the most cross-core contention.
  5. **Memory manager.** Per-address-space locks, a page-allocator lock with per-CPU free-page caches, and TLB-shootdown batching.
  6. **VFS.** Per-inode and per-file locks plus a dentry/path-cache lock; file systems can stay under a "filesystem-giant" lock initially, as FreeBSD did with Giant-locked file systems.
  7. **Network stack.** Shared versus exclusive locks, following OpenBSD's netlock evolution.
  8. **Drivers.** Leave legacy drivers under a "driver giant" until rewritten.
- **Effort.** For a kernel of Terminal OS's size, steps 1–4 are perhaps 1–3 months, and reaching "most hot paths unlocked" perhaps 6–12 months of part-time work (my estimate). Precedents measured in years involved kernels of millions of lines.
- **Ordering with the rest of the roadmap.** Do the SMP work before the Ladybird port runs many threads and processes. Browsers spread work across multiple processes and threads, so a BKL caps their responsiveness and the C14 scores.

### Gaps
- No primary source was retrieved for the year Linux's BKL was introduced (commonly given as Linux 2.0, 1996), or for FreeBSD SMPng's start date (around 2000).
- Not determined: whether SerenityOS has finished removing its big lock (the commits found run to 2023), or OpenBSD 7.8's status (TCP input).

## 10. VM targets and virtio: cost and value (C8, C13)

### Takeaway
Virtio is the best-value driver family a hobby OS can add:
- **virtio-net, blk, scsi, gpu, input, rng and console** cover QEMU/KVM (including Proxmox, GNOME Boxes, UTM and most clouds) and VirtualBox's virtio-net adapter.
- **Effort:** each driver is small, a few weeks or less, because the virtqueue model is standardised in OASIS VIRTIO 1.x.
- **CI:** it speeds up the project's own QEMU tests.
- **Other hypervisors** need their own work:
  - VMware's paravirtual devices (vmxnet3, PVSCSI, SVGA) are separate, but VMware and VirtualBox also emulate legacy hardware that Terminal OS already supports (e1000-class NICs, AHCI).
  - Hyper-V Generation 2 VMs have no legacy NIC at all and need VMBus synthetic drivers.

### Cited Findings
- OASIS VIRTIO 1.3 is a Committee Specification Draft dated 6 October 2023. It supersedes 1.2, stays compatible with it, and adds features to existing net, blk and crypto devices — [OASIS VIRTIO v1.3](https://docs.oasis-open.org/virtio/virtio/v1.3/virtio-v1.3.html); [OASIS TC submission record](https://issues.oasis-open.org/browse/TCADMIN-4432)
- VirtualBox's virtio-net adapter uses the open-source KVM virtio networking drivers (native in Linux from 2.6.25) — [VirtualBox manual, network hardware](https://sources.debian.org/src/virtualbox/7.1.12-dfsg-2/doc/manual/en_US/dita/topics/nichardware.dita)
- Hyper-V Generation 2 VMs have no legacy network adapter. Their only NIC is the synthetic adapter over VMBus, which also supports PXE boot through UEFI. Gen 2 VMs support fewer guest OSes than Gen 1 — [Microsoft TechNet wiki](https://learn.microsoft.com/en-us/archive/technet-wiki/28274.optimizing-hyper-v-with-generation-2-virtual-machines); [Petri](https://www.petri.com/hyper-v-virtual-machine-virtual-network-adapters-overview)
- VMware's vmxnet3 is its preferred but not default NIC — [Virtualization Review](https://virtualizationreview.com/blogs/everyday-virtualization/2010/02/vmxnet3.aspx). VMware open-sourced its pvscsi and vmxnet3 drivers, which are in mainline Linux — [lonesysadmin](https://lonesysadmin.net/2013/12/13/use-paravirtualized-devices-virtual-machines)
- OpenBSD ships drivers for virtio (including vioscsi), vmx (vmxnet3) and vmwpvs (PVSCSI), which shows the usual set of paravirtual drivers an independent OS carries — [OpenBSD Handbook](https://www.openbsdhandbook.com/tags/intro/page/81)

### Inferences
- **Effort estimates for Terminal OS** (one developer, existing PCI and DMA infrastructure):

  | Driver | Effort | Notes |
  |---|---|---|
  | Shared virtio-pci "modern" transport plus virtqueue core | 1–2 weeks | |
  | virtio-blk | Days | |
  | virtio-net | About 1 week | Includes multi-queue later |
  | virtio-input | Days | Keyboard, mouse, tablet |
  | virtio-rng and virtio-console | Days | |
  | virtio-gpu, 2D only | 2–3 weeks | Plugs into the compositor as a scanout plus damage flush |
  | virtio-gpu, 3D (virgl/Venus) | Months | Needs a Mesa/Vulkan user-space stack |
  | VMware SVGA II | 2–4 weeks | VirtualBox's VMSVGA adapter also uses this (unverified) |
  | Hyper-V VMBus core plus netvsc, storvsc, synthetic video and HID | About 2–3 months | Needed only if Hyper-V is a target |

- **Value:**
  - Reviewers and new users usually try a new OS in a VM first. Working network, display resize and absolute pointer in QEMU, VirtualBox and VMware strongly affect first impressions (C16, C11).
  - virtio also makes CI faster and less flaky.
- **Priority:**
  1. virtio-blk, net and input, then virtio-gpu 2D, in QEMU/KVM.
  2. Check VirtualBox and VMware defaults with the existing e1000/AHCI drivers. VMware's default NIC for modern guests is often e1000e (82574), which may differ from e1000 (82540/82545); verify.
  3. VMware SVGA.
  4. Hyper-V last.

### Gaps
- I did not verify VirtualBox's virtio-scsi and VMSVGA details, VMware's current default devices, the virtio-gpu and virtio-input sections of the spec, or which cloud platforms require virtio-scsi or NVMe boot.

## 11. ARM64 port: cost, value and precedents (C1, C8, C17)

### Takeaway
An ARM64 laptop port has poor value for Terminal OS in 2026–2027.
- **Market:** Arm Windows laptops are a small share, and Macs (about 9.5% of PCs) need reverse engineering on the scale of Asahi Linux.
- **Snapdragon X:** Linux on X Elite was still incomplete at the end of 2025, even with Qualcomm upstreaming. TUXEDO abandoned its Snapdragon X1 Elite Linux laptop after more than 18 months. Qualcomm's 2026 Debian support covers only the X2 generation.
- **Apple Silicon:** Asahi made M3 "official" only in September 2026, still without GPU acceleration or sleep.
- **Hobby precedents:** SerenityOS and Redox show multi-year AArch64 efforts that still centre on the Raspberry Pi and QEMU.

The cheap, valuable step is to keep the code portable (an architecture abstraction layer), and optionally bring up an AArch64 build on QEMU's `virt` machine using virtio and ACPI or device tree. That is a portability exercise, not a hardware strategy.

### Cited Findings
- Snapdragon X Elite on Linux: Phoronix's December 2025 review found the best experience on Ubuntu but results varied by model, and it called the year-end state disappointing — [Phoronix](https://www.phoronix.com/review/snapdragon-x-elite-linux-eoy2025). A March 2026 test on Ubuntu 26.04 hit regressions, including an X1E kernel booting to a grey screen — [Pulse (relaying Phoronix)](https://www.pulse.bot/hardware/blogs/trying-out-snapdragon-x-elite-with-the-acer-swift-14-ai-laptop-on-ubuntu-2604-45ed15c7-217c-49a4-b8fe-1250cb7d68e7)
- Most X Elite laptops need firmware blobs copied from the Windows partition; only Lenovo ThinkPad firmware is in linux-firmware. Without it, Adreno GPU acceleration and other features are lost — [It's FOSS](https://news.itsfoss.com/snapdragon-x-elite-linux)
- TUXEDO stopped developing its Snapdragon X1 Elite Linux laptop after more than 18 months, saying Qualcomm was less suitable for Linux than expected — [Tom's Hardware](https://www.tomshardware.com/software/linux/snapdragon-x1-elite-linux-laptop-cancelled-due-to-performance-concerns-linux-pc-maker-says-qualcomm-is-less-suitable-for-linux-than-expected)
- At Snapdragon Summit 2026, Qualcomm announced Debian (and Ubuntu) support for the X2 Plus, X2 Elite and X2 Elite Extreme, with no support added for the first generation — [Digitec/Galaxus](https://www.digitec.ch/en/page/qualcomm-announces-debian-and-ubuntu-support-for-the-snapdragon-x2-43916)
- Asahi Linux officially supports Apple M3 Macs since 6 September 2026, except the Mac Studio M3 Ultra. The support is downstream, only basic boot is in mainline, and GPU acceleration, sleep and HDMI on M3 MacBooks are missing. M4/M5 bring-up has NVMe and PCIe detection but is not ready for daily use — [Phoronix (official M3)](https://www.phoronix.com/news/Asahi-Linux-Official-M3); [Phoronix (2026 progress)](https://phoronix.com/news/Apple-M3-Asahi-Linux-2026); [MacG (Aug 2026)](https://www.macg.co/macos/2026/08/asahi-linux-se-rapproche-dune-sortie-sur-les-mac-m3-et-progresse-deja-sur-les-m4-et-m5-310683)
- SerenityOS AArch64: an older version of its Raspberry Pi guide said Serenity "doesn't currently boot on Raspberry Pi" and supported only UART output, with no display — [SerenityOS RunningOnRaspberryPi.md (mirror)](https://git.rgbcu.be/RGBCube/serenity/src/commit/8a92573732fca3ec43ea3ab4cbecda500dfd9a8d/Documentation/RunningOnRaspberryPi.md). 2025 commits show continuing Pi work: a bare-metal framebuffer fix with the data cache on, UART exposed to user space, and a watchdog-based "halt" — [SerenityOS RPi commits (mirror)](https://git.rgbcu.be/RGBCube/serenity/commits/branch/master/Kernel/Arch/aarch64/RPi). An auto-generated summary site mentions 2025 work on AArch64 reliability and the EFI prekernel (low reliability) — [exceeds.ai](https://myteam.exceeds.ai/profile/spholz)
- Redox: the Redox book says ARM64 has "limited support" — [Redox book](https://doc.redox-os.org/book/aarch64.html). An early "RSoC" (Redox Summer of Code) series targeted Raspberry Pi 2/3 (Cortex-A53) with drivers matched by device-tree compatible strings — [Redox RSoC AArch64](https://redox-os.org/news/rsoc-arm64-0x01). A Korean aggregator relaying Phoronix reports Redox booting to the login screen on a Raspberry Pi 4, with USB and other features incomplete (date unclear) — [GeekNews](https://news.hada.io/topic?id=17622)
- Arm share data and forecasts are in section 1 (Snapdragon X under 1.5% of Windows in late 2024, ABI's 13% forecast for 2025 including Macs, Counterpoint's 2029 forecast) — [ITdaily](https://itdaily.com/news/business/arm-op-windows-marktaandeel-in-q3/); [Counterpoint](https://counterpointresearch.com/en/reports/windows-on-arm-to-capture-over-one-third-of-the-market-in-ai-pc-era)

### Inferences
- **Cost of an ARM64 hardware port (my estimate):**
  - **AArch64 core** (exception levels, MMU, GIC interrupt controller, generic timer, PSCI CPU bring-up, SMP): about 2–4 months to boot on QEMU `virt` with virtio.
  - **Per-SoC laptop enablement** (clocks, regulators, pin control, the display pipeline, the Adreno or Apple GPU, Qualcomm remote processors for Wi-Fi, audio and modem, firmware loading, power management): years. Funded Linux efforts (Linaro and Qualcomm; Asahi) are still incomplete.
- **Device tree versus ACPI.** Snapdragon X laptops ship ACPI tables meant for Windows. Linux support has relied on per-model device trees, and Apple Silicon is device-tree-only via m1n1 (the Asahi bootloader). A new OS would need a DT parser and per-model DTs in either case (inference; not verified in this session).
- **Value case:**
  - Small today.
  - Possibly growing if Arm Windows laptops expand (Counterpoint's 2029 forecast is speculative).
  - Real but indirect: portability discipline, cloud Arm servers, and Raspberry Pi hobbyist reach.
- **Recommendation.** Defer hardware ARM64. Do keep architecture-specific code (NASM entry stubs, int 0x80 ABI, APIC/HPET/TSC, ACPI assumptions) behind an `arch/` boundary now; that is cheap early and expensive to retrofit. Revisit after x86 C8, C9 and C2 reach 5+.
- **Mapping to the framework.** An ARM64 port mainly affects C1 and C8 breadth. The mature independent OSes cited (SerenityOS, Redox) have not reached laptop-grade ARM support either, so its absence does not lower Terminal OS's standing against the 5–6 "independent OS" benchmark.

### Gaps
- I found no 2026 primary source on how much Snapdragon X device-tree code is in mainline, or on X2 Linux readiness beyond the announcement.
- No data on Haiku's ARM64 port status.
- No person-year figures for SerenityOS's or Redox's AArch64 work.
