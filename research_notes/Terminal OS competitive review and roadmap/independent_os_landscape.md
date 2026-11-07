# Independent and alternative operating systems, November 2026: peer profiles, C1–C17 scores, and lessons for Terminal OS

Scope: general-purpose desktop OSes that are independent of Linux/Windows/macOS kernels, as the realistic peer group for a solo, from-scratch x86_64 hobby OS. HarmonyOS NEXT and Google's products are included only as cost references. Research date: 7 November 2026. Items older than 2025 are flagged **[pre-2025]**. GitHub numbers come from GitHub REST API queries on 2026-11-07: `contributors?anon=1` counts distinct commit authors, including old ones and mirror-only authors, so treat them as an upper bound on "people who ever committed", not as active team size.

---

## 1. Profiles: 2026 status, architecture, team, funding, software and hardware, milestones, and C1–C17 scores

### Takeaway
Every independent desktop OS in 2026 is still pre-1.0 or beta after 8–28 years. Haiku (R1/beta6, Aug 2026, now with official Firefox), ReactOS (0.4.16 alpha, Aug 2026), Redox (0.9.0 from Sept 2024, plus monthly progress), SerenityOS (no releases, founder gone to Ladybird) and Genode/Sculpt (26.10, own microkernel) are the mature tier. Managarm (KDE Plasma 6 and GNOME Web in Oct 2026), Asterinas (Xorg and self-hosting in Oct 2026) and Astral (Minecraft and Wine, solo) are the most striking recent achievers. They got there through compatibility layers (Linux ABI or source compatibility, mlibc, ported drivers) rather than by writing everything themselves. Fuchsia is production-grade but has narrowed to Nest Hub firmware. ToaruOS archived its GitHub repository on 26 Oct 2026.

### Cited Findings

#### SerenityOS (monolithic Unix-like kernel and full in-house userland, C++, started 2018)
- First git commit 2018-10-10. It was a solo project by Andreas Kling (ex-Nokia, ex-Apple WebKit), and he went full-time in 2021 on community donations — [Wikipedia](https://en.wikipedia.org/wiki/SerenityOS)
- First-year timeline **[pre-2025]**. Commit 2018-10-10; ran in a VM 2018-11-08; first GUI on its own kernel 2019-01-10; ping 2019-03-12; "TCP kinda worked" plus an IRC client 2019-03-15; first semi-successful GCC port 2019-04-22; basic sound 2019-07-14; DOOM 2019-09-09; basic CSS in LibHTML 2019-09-29; its own Browser rendered the anniversary page 2019-10-10 — [SerenityOS "Year 1"](https://serenityos.org/happy/1st/)
- Kling stepped down as lead on 3 June 2024 to focus on Ladybird. The Ladybird Browser Initiative (a 501(c)(3)) was announced on 1 July 2024 with Chris Wanstrath. Kling removed himself from the SerenityOS maintainer list on 4 Oct 2024. The last monthly video update was in April 2024 — [Wikipedia](https://en.wikipedia.org/wiki/SerenityOS)
- There is no release cycle and no binaries; users build from source. Ports include GCC, Git and Doom. Supported platforms are x86-64, with AArch64 and RISC-V at an early stage. The licence is BSD-2-Clause. Since July 2024 most browser development happens in Ladybird's repository, with some changes synced back — [Wikipedia](https://en.wikipedia.org/wiki/SerenityOS)
- Real hardware: it runs on x86-64 but "isn't ready for non-technical users", and there are no prebuilt images. USB support is xHCI and UHCI only. Networking is e1000 and RTL8168. There is **no Wi-Fi** and **no GPU support** (framebuffer only, no OpenGL or Vulkan). Audio covers AC'97 and Intel HDA. It boots via GRUB2 in BIOS or UEFI mode — [BareMetalInstallation.md](https://github.com/SerenityOS/serenity/blob/master/Documentation/BareMetalInstallation.md)
- The repository is still active (last push 2026-11-06), with 33,946 stars and about 1,411 distinct commit authors ever — [GitHub](https://github.com/SerenityOS/serenity). An undated Linux Foundation page lists "21 active maintainers" with merge rights — [LFX Insights](https://insights.linuxfoundation.org/project/serenityos-serenity)

#### Ladybird (SerenityOS's browser, spun out; the most important "independent engine" asset in the peer group)
- It began in 2019 inside SerenityOS and was spun off in Sept 2022. Shopify became the first corporate sponsor with $100,000 (2023). Wanstrath gave $1M in July 2024. In Feb 2026 the JS parser and bytecode generator were ported from C++ to Rust, about 25,000 lines, with Kling "using Claude Code and Codex". On 5 June 2026 the project stopped accepting public pull requests from non-maintainers, citing lower-quality contributions and security. The roadmap is alpha 2026, beta 2027, stable 2028, on Linux and macOS; Windows is not a priority — [Wikipedia](https://en.wikipedia.org/wiki/Ladybird_(web_browser)); [Ladybird on X](https://x.com/ladybirdbrowser/status/1895767072639951341)
- Sponsor tiers: Platinum ($100,000) is FUTO, Shopify and Cloudflare. Gold ($50,000) is the Human Rights Foundation and Proton VPN. Silver ($10,000) includes Mitchell Hashimoto, 37signals and CodeRabbit. The site says it is "funded entirely by donations and sponsorships", with no "default search deals", has "a small full-time engineering team" and that "our team size is stable for now". It also says the "first Alpha for macOS and Linux is coming later this year" — [ladybird.org](https://ladybird.org/)
- As of early Oct 2026 the alpha was still unreleased and aimed at the end of 2026. Trail of Bits did security testing — [PiunikaWeb (secondary)](https://piunikaweb.com/2026/10/06/ladybird-browser-alpha-update/)
- The repository has about 66,906 stars and about 1,547 distinct authors — [GitHub](https://github.com/LadybirdBrowser/ladybird)

#### Haiku (BeOS-compatible hybrid kernel forked from NewOS, C++, started 2001)
- OpenBeOS started the day after Palm bought Be (Aug 2001). The NewOS kernel was forked in 2002. Haiku, Inc. was founded in 2003 and the project was renamed Haiku in 2004. app_server was complete in 2005. Release dates: R1/alpha1 2009-09-14; alpha2 2010-05-10 (first with WebPositive); alpha4 2012-11 (first x86-64); package management Sept 2013; beta1 2018-09-28 (HaikuDepot and pkgman); beta2 2020-06-09; beta3 2021-07-26; beta4 2022-12-23; beta5 2024-09-13; beta6 2026-08-26. Wine was first ported in 2022 — [Wikipedia](https://en.wikipedia.org/wiki/Haiku_(operating_system))
- R1/beta6 came out on 2026-08-26, "about two years since the last beta" and about a week after Haiku's 25th birthday. Haiku mentored 3 GSoC students in 2026, 5 in 2024 and 3 in 2023 — [Haiku news](https://www.haiku-os.org/news/2026-08-26_haiku_r1_beta6/)
- **Beta6 release notes:**
  - Browsers: Firefox builds based on Mozilla's codebase appeared a few months after beta5, Mozilla now allows the official "Firefox" branding, and it is x86_64 only. WebPositive is still single-process WebKit.
  - Graphics: there is no 3D acceleration story. Systems without a modesetting driver fall back to an EFI framebuffer or VESA.
  - Networking: Wi-Fi and Ethernet use a "compatibility layer that runs FreeBSD and OpenBSD network drivers mostly unmodified", and USB Wi-Fi and USB Ethernet coverage was expanded.
  - Power and virtualization: PowerStatus handles multiple batteries, but suspend is not mentioned. NVMM hardware virtualization for QEMU is "still somewhat experimental".
  - Ports and platforms: Go 1.26.1 was ported. ARM64 boots to the desktop under virtualization, and an unmerged branch reaches the desktop on Apple M1.
  - Scale: over 530 tickets were resolved in the cycle; the tracker has over 3,900 open and over 16,000 closed.
  - Quality: compile performance is "~40% slower than Linux". It is beta-quality, with "no assurances against data loss".
  - People: one new committer joined. waddlesplash (Augustin Cavalier) was "employed as a contractor for the whole release cycle", funded by donations to Haiku, Inc., a 501(c)(3).
  — [R1/beta6 release notes](https://www.haiku-os.org/get-haiku/r1beta6/release-notes/)
- Finances: 2024 was a record donation year at "over $31,000", not counting GSoC income, after a 2022 deficit. Donations pay the contractor — [Haiku 2024 financial report post](https://www.haiku-os.org/news/2025-02-01_2024_financial_report) (seen through a search summary; the 2025 report was not found)
- The GitHub mirror has about 469 distinct authors — [GitHub](https://github.com/haiku/haiku)

#### Redox OS (Rust microkernel, user-space drivers, relibc, started 2015)
- First commit 2015-04-20 — [GitHub](https://github.com/redox-os/redox). 2025 marked 10 years since that commit — [This Month in Redox, Dec 2025](https://redox-os.org/news/this-month-251231/)
- **0.9.0 (2024-09-09) [pre-2025]:**
  - Apps: ports of COSMIC Files, COSMIC Edit and COSMIC Term. relibc is "almost 100% Rust".
  - USB was not yet included; the planned order was HID, then hubs, then storage.
  - There were no GPU drivers; games used LLVMpipe.
  - Self-hosting was not complete ("high on our agenda").
  - About 1,700 work-in-progress ports existed.
  - Funding: NLnet NGI Zero Core and Radworks grants paid a community manager and a student.
  — [Redox 0.9.0](https://www.redox-os.org/news/release-0.9.0/)
- DistroWatch still lists 0.9.0 as the current version, so there has been no versioned release in 2025–2026 — [DistroWatch](https://distrowatch.com/table.php?distribution=redox)
- **2025 year-end:**
  - Community: more than 1,000 Matrix members and "more than 40 significant contributors".
  - Money: $17,000 in community donations plus $20,000 from Jeremy Soller (funds received before the nonprofit was formed). It paid for RSoC students, a part-time build engineer, a community manager and operating costs.
  - Grants: three NGI Zero projects (Unix-style signals, capability-based security, io_uring-like I/O).
  - Governance: a Redox OS Nonprofit board with Ron Williams (chair), Jeremy Soller (treasurer and lead maintainer), Alberto Souza and Mathew John Roberts; directors are unpaid. A trademark policy was adopted.
  - Graphics: the first GPU driver, for Intel Tiger Lake and Kaby Lake, does modesetting only, with no acceleration. Basic read-only Linux DRM APIs were added, "big steps" were taken toward Wayland and "baby steps" toward hardware-accelerated graphics.
  - Testing: os-test POSIX compliance fixes.
  — [Dec 2025 report](https://redox-os.org/news/this-month-251231/)
- Grants: the RedoxOS Signals project got €26,500 from NGI Zero Core — [NLnet](https://nlnet.nl/project/RedoxOS-Signals/); capability-based security started June 2025 — [NLnet](https://nlnet.nl/project/Capability-based-RedoxOS/); the "Virtualized Redox" NGI Zero Commons grant was announced in June 2026 — [June 2026 report](https://redox-os.org/news/this-month-260630/). A €50k figure appears only in secondary press and is unconfirmed.
- 2026 monthly highlights:
  - March: an AI policy forbidding LLM-generated contributions — [Phoronix](https://www.phoronix.com/news/Redox-OS-March-2026)
  - April:
    - Moved to the upstream `acpi` crate, "improving support for many computers, and VirtualBox".
    - The xHCI driver now loads its configuration at runtime.
    - A Framework Laptop 16 hardware report came back "Broken".
    - Ports: tmux and OpenSSL 3. COSMIC versions were pinned to work around a mouse-click regression, and NetSurf gained partial pixel blit support.
    — [April 2026](https://www.redox-os.org/news/this-month-260430/)
  - June: a GTK3 backend for the Orbital desktop, fractional scaling and USB gamepads — [Phoronix](https://www.phoronix.com/news/Redox-OS-June-2026)
  - July: installer improvements and performance work — [Phoronix](https://www.phoronix.com/news/Redox-OS-July-2026)
  - September:
    - Performance: direct context switching cut IPC latency by 24%, 2 MiB kernel pages cut TLB misses by 75%, and dropping a needless AVX save cut IPC latency by 14%.
    - Hardware: Intel Tiger Lake external-display hot-plug, an HDA panic fix, an HP ENVY 16 report reading "Booting", and boot on Google Cloud.
    - Ports: WebKitGTK3 HTML layout fixes, SDL2 OpenGL speed-ups, BIND9 and OpenSSH 10.5p1, plus expanded dynamic linking for COSMIC apps.
    - Toolchain: Clang cross-compilation.
    - People: 20 contributors named.
    — [Sept 2026](https://www.redox-os.org/news/this-month-260930/)
- None of the 2025–2026 reports read mentions Wi-Fi or suspend.

#### ReactOS (NT-compatible hybrid kernel, Windows binary and driver compatibility, C, started 1996/1998)
- History: FreeWin95 began around 1996 and stalled. NT-style kernel work under the ReactOS name began in Feb 1998. A foundation in Moscow ran from 2002 to 2015. A 2006 claim of disassembly-derived code led to an internal audit, finished around 2007.
- Releases: 0.3.0 on 2006-08-27 (first with official networking); 0.4.0 on 2016-02-16; 0.4.14 on 2021-12-16; 0.4.15 on 2025-03-21; 0.4.16 on 2026-08-29. It is still described as "feature-incomplete" alpha.
- It reuses Wine code, including WineD3D. x86-64 support has made "slow progress".
- Team: in 2011 it had 33 active developers and 104 contributors ever **[pre-2025]**.
- Funding **[pre-2025]**: a €30k campaign in 2012 reached about 50%; the 2013–14 Thorium Kickstarter raised $48,965 of $120,000 and failed; the 2014 Indiegogo raised $25,141 of $50,000.
- Applications reported working include Adobe Reader 9.3, GIMP 2.6 and LibreOffice 5.4.
— [Wikipedia](https://en.wikipedia.org/wiki/ReactOS)
- **0.4.16 (2026-08-29):** 2,808 commits and 381 Jira issues, the oldest from 2009. Changes:
  - Installation: a new graphical installer, a unified boot/live CD, and a "Server Core" install type.
  - Storage: a new ATA driver replaces UniATA, so it boots on Hyper-V Gen 1.
  - Audio: HD audio through the sklhdaudbus bus driver, with KMDF imported, so Windows XP/2003-compatible HDA controllers "should now work".
  - Graphics: an NVIDIA slowdown fix and an AMD OpenGL fix.
  - Networking: a DC21X4 NIC driver.
  - Compatibility: the Wine fork is moving from Wine 2.x/3.x toward 10.0. Release images still export only Windows Server 2003 APIs, and Vista+ exports need a build flag. WineVDM, for 16-bit apps, is included.
  - Money: donations fund testing, hosting and "development contracts".
  — [ReactOS news](https://reactos.org/project-news/reactos-0416-released/)
- About 439 distinct authors and 18,457 stars — [GitHub](https://github.com/reactos/reactos)

#### Fuchsia (Zircon capability microkernel, C++/Rust/Dart, Google, started 2016)
- It appeared publicly as a Git repository in Aug 2016. Zircon (formerly Magenta) derives from Little Kernel. fuchsia.dev launched on 1 July 2019. The first consumer product was the first-gen Nest Hub on 2021-05-25, with all units by Aug 2021; Chrome was fully working by about Feb 2022. Jan 2023 layoffs affected 16% of Fuchsia employees, with no absolute number given. The 2nd-gen Nest Hub followed in May 2023. The latest release is F31 (22 July 2026) — [Wikipedia](https://en.wikipedia.org/wiki/Fuchsia_(operating_system))
- Oct 2026: a firmware update with "bug fixes and security updates" is rolling out to Nest Hub, Nest Hub (2nd Gen) and Nest Hub Max. These are maintenance-mode device updates — [9to5Google, 2026-10-20](https://9to5google.com/2026/10/20/google-rolling-out-minor-nest-hub-update/)
- Starnix runs unmodified Linux binaries by translating Linux syscalls to Zircon. It was first proposed in early 2021 — [OSnews Fuchsia topic](https://www.osnews.com/topic/fuchsia/). Google is also bringing Fuchsia to Android devices "not in the way you'd think"; the URL slug says "microfuchsia" (I saw only the headline) — [Android Authority](https://www.androidauthority.com/microfuchsia-on-android-3457788/)
- Google's PC OS went Linux-based instead. Googlebook OS (codename Aluminium) merges ChromeOS into Android: announced at I/O on 12 May 2026, released 4 Oct 2026 for x64 and ARM64 — [Wikipedia](https://en.wikipedia.org/wiki/Aluminium_OS)

#### Genode OS Framework and Sculpt OS (component-based capability OS on many microkernels, C++, Genode Labs, Dresden)
- It began as the "Bastei" research report at TU Dresden (paper dated 2007). The report's authors founded Genode Labs, and the first release was in 2008. Releases are quarterly; the latest framework release is 26.05 (29 May 2026), and the repository moved from GitHub to Codeberg on that date. It runs on NOVA, seL4, Fiasco.OC, OKL4, Pistachio and Linux, plus Genode's own kernel. Drivers cover "most common PC peripherals", including "Intel wireless, Intel GPUs". The licence is AGPL-3.0 plus commercial — [Wikipedia](https://en.wikipedia.org/wiki/Genode)
- **Sculpt OS 26.10 (2026-10-30):**
  - Kernel: the first Sculpt on Genode's own microkernel, which fixes long-standing CPU-scheduling and kernel-memory deficits.
  - Configuration: the move from XML to the HID format is complete.
  - Drivers: the Linux-based driver components went from **kernel 6.12 to 6.18**, and the USB host-controller driver can now be restarted without disturbing its clients.
  - Virtualization: VirtualBox 7.
  - Development: the Goa SDK runs natively on Sculpt (GCC, binutils, git, make, CMake), though this is "young and not yet time-tested".
  - Images: PC, PinePhone and MNT Reform, with a 41 MiB image.
  — [Genode news](https://genode.org/news/sculpt-os-release-26.10); [Phoronix](https://www.phoronix.com/news/Sculpt-OS-26.10)
- Genode Labs GmbH (Dresden) was founded in 2008 by Norman Feske and Christian Helmuth and is solely owned by them. It calls itself "a small and tight-knit team". The business model is consulting plus dual licensing of the framework, and no customers are named — [genode-labs.com](https://genode-labs.com/)
- Sculpt 26.04 (30 Apr 2026) "can finally be used to self-host" Genode development — [Phoronix](https://www.phoronix.com/news/Sculpt-OS-26.04)
- Suspend-to-RAM exists but is fragile. Sculpt 26.04 release candidates fixed "suspend to RAM regressions" and "driver restart and wifi issues". One tester's Intel AX210 Wi-Fi failed after resume; on a ThinkPad, HDMI survived resume — [Genode Discourse](https://genode.discourse.group/t/sculpt-26-04-release-candidates/308?page=2)
- **[pre-2025]** Sculpt 21.10 added experimental Intel GPU hardware acceleration via Mesa and Intel's Linux kernel driver — [Phoronix](https://www.phoronix.com/news/Sculpt-OS-21.10). 21.08 reworked how Linux driver code is ported — [Genode 21.08](https://genode.org/news/genode-os-framework-release-21.08). The Chromium-based Falkon browser was playing media by 21.11 — [Genode 2021 news](https://genode.org/news/2021)

#### Managarm (asynchronous microkernel, C++ with new Rust servers, mlibc's origin, started 2014)
- The 12th anniversary update (18 Oct 2026) implies a 2014 origin. The update's four authors are van der Grinten, Bonke, Richards and Friedrich.
  - Desktop: KDE Plasma 6 is "usable for daily use" via the `kde-desktop` metapackage, though Weston remains the default.
  - IOMMU-enabled drivers: xHCI, UHCI, EHCI, Intel GPU, Intel HDA, e1000 and RTL8168.
  - Storage: SATA, NVMe, ATA, USB mass storage and virtio-blk.
  - Laptop pieces: ACPI embedded-controller and battery drivers.
  - Architectures: ACPI on AArch64 (Radxa Orion O6, Rock 5B+), plus RISC-V hypervisor extension support.
  - Memory: swapping.
  - Linux compatibility: a Linux-compatible `/sys` and `/proc` with netlink uevents; epoll, timerfd and signalfd.
  - Wi-Fi is not mentioned.
  — [Managarm Oct 2026 update](https://managarm.org/2026/10/18/oct-update.html)
- **End of 2025 (13 Jan 2026):**
  - Packages and apps: **454 packages** for x86_64; Weston and Sway; GNOME parts; Konversation from KDE; basic Java; Minecraft Alpha.
  - Compatibility: "source-level compatibility with Linux software", with **systemd as init**, and os-test ported.
  - GPU: nvidia-open modesetting works on real hardware, but there is no 3D (NVIDIA's userland is closed and glibc-linked).
  - USB: real-hardware desktop USB still "will require the addition of some new drivers".
  - Boot: NVMe-over-Fabrics diskless boot.
  - Hardware CI on four devices: Raspberry Pi 4, Orion O6, Banana Pi BPI-F3 and an Intel N100 board.
  - People and funding: 32 contributors acknowledged for 2024–25; funding through GitHub Sponsors.
  — [End of 2025 update](https://managarm.org/2026/01/13/end-of-year-update.html)
- Browser port (Dennis Bonke, 13 Oct 2026): **Epiphany (GNOME Web, WebKitGTK)**, which needed the GTK4 stack.
  - Speed: cold start took about 1 minute at first, versus about 9 s on a Linux host. After I/O and caching work it is about 15 s with the ad blocker on, measured in a VM.
  - Video and audio: YouTube plays after GStreamer and FFmpeg work, and audio works through a PipeWire workaround.
  - Sites that work: GitHub, GitLab, Codeberg, Element (Matrix) and ChatGPT. Discord works for text only.
  - Funding: NLnet NGI0 Commons Fund (grant agreement 101135429) plus Swiss SERI.
  — [Porting Epiphany](https://managarm.org/2026/10/13/0-epiphany.html); [NLnet "Web on Managarm"](https://nlnet.nl/project/Managarm/)
- Activity: 194 commits by 7 contributors in the 30 days to 27 Feb 2026 — [Open Hub](https://openhub.net/p/managarm-os). The repository has 36 distinct authors and mlibc has 63 — [GitHub](https://github.com/managarm/managarm), [mlibc](https://github.com/managarm/mlibc)

#### Asterinas (Rust "framekernel" with Linux ABI, aimed at cloud and confidential VMs; started 2022)
- **0.19.0 (2026-10-13):**
  - People and size: >100 contributors ever and 55 in this release, 31 of them new. The codebase passed 250,000 lines, about 200,000 of them Rust.
  - Graphics: a new DRM subsystem lets Asterinas NixOS run Xorg on the modesetting driver with Mesa software OpenGL.
  - Architectures: AArch64 became the 4th, after x86-64, RISC-V and LoongArch.
  - Containers and VMs: secure-container work (overlayfs, the new mount API, Firecracker, faster TDX boot) and a VMX hypervisor foundation.
  - Drivers: aster-device, a safe-Rust device model.
  - Tooling: an AI "aster-code-review" skill.
  - **Self-hosting:** "Asterinas builds and boots Asterinas", with Nix, Cargo, rustc and QEMU running on it.
  — [Asterinas blog](https://asterinas.github.io/2026/10/13/announcing-asterinas-0.19.0.html)
- **LWN (19 June 2025):**
  - History: first released under the MPL in early 2024.
  - Safety design: all `unsafe` code is confined to the OSTD core library.
  - Backing: most commits come from PhD students at SUSTech, Peking University and Fudan. Ant Group is a sponsor, and Intel is listed among sponsors.
  - Linux compatibility: 180 Linux syscalls in Jan 2025, rising to 206 on x86 (Linux 6.7 has 368).
  - Verification: CertiK and Verus.
  - Target: a container host OS with an Aliyun focus. Loading Linux kernel modules is not planned.
  — [LWN](https://lwn.net/Articles/1022920/)
- A blogger's re-run of the 0.19 benchmarks found that 13 of 23 claims held up — [efraingaray.com](https://efraingaray.com/en/blog/asterinas-0-19-contra-linux/) (blog source). The repository has 122 distinct authors and was created 2022-05-17 — [GitHub](https://github.com/asterinas/asterinas)

#### Astral (solo, C, x86-64, mlibc and POSIX; repository from 2022)
- It is a 64-bit hobby OS written in C. Ports include X.org, fvwm, xbps "and tons more". Minecraft ran (blog post 31 Oct 2025). A Wine port runs "a few games", including the 32-bit Cogmind, after the author added 32-bit code support to the otherwise 64-bit-only OS. Others ran modern Minecraft and Factorio with a glibc-compatible libc. The posts are written in the first person by a single author — [OSnews, 29 June 2026](https://www.osnews.com/story/145388/astral-is-a-hobby-operating-system-with-x-org-minecraft-and-now-wine/)
- The author believes this was the first hobby OS to run Minecraft. Getting OpenJDK working meant fixing mlibc bugs: characters above 127 and dlopen promotion — [Astral blog](https://astral-os.org/posts/2025/10/31/astral-minecraft.html) (seen through a search summary)
- The repository was created 2022-07-28 and last pushed 2026-10-15. It has 636 stars and 14 distinct authors — [GitHub](https://github.com/mathewnd/Astral)

#### ToaruOS (solo, C, everything in-house: Misaka kernel, libc, Yutani compositor, Kuroko language; started Dec 2010)
- Development began in December 2010 under K. Lange, initially supported by the University of Illinois at Urbana–Champaign. After early 2012 it was mostly Lange alone. 1.0 came out on 30 Jan 2017, about 6 years in. Ported software includes GCC, Python, SDL and Vim — [Wikipedia](https://en.wikipedia.org/wiki/ToaruOS)
- 2.3.0 was announced in late April 2026 by Kevin Lange; it has its own bootloader, kernel and C library — [DistroWatch](https://distrowatch.com/?newsid=12816). The versions were 2.3.0 (27 Apr 2026), 2.3.1 (4 May) and 2.3.2 (12 May) — [Wikidata](https://www.wikidata.org/wiki/Q30645415). 2.3 adds terminal tabs and a manpage viewer; 2.0 was released on 12 Dec 2021 — [toaruos.org](https://toaruos.org/)
- **The GitHub repository was "archived by the owner on Oct 26, 2026"** and is read-only, with no notice of a move. Mirrors exist on GitLab, Bitbucket and sourcehut. It runs on x86-64 and ARMv8 VMs and has been "successfully tested on real hardware". The repository was created 2011-01-16 and has 25 distinct authors — [GitHub](https://github.com/klange/toaruos)

#### HelenOS (multiserver microkernel, C, Charles University origin)
- The initial release was on 11 June 2006. The core team is mainly staff and former and current students of the Faculty of Mathematics and Physics at Charles University in Prague. It was a GSoC mentoring organisation in 2011, 2012 and 2014 and an ESA Summer of Code in Space organisation in 2013 **[pre-2025]** — [Wikipedia](https://en.wikipedia.org/wiki/HelenOS)
- The latest release is 0.14.1 (19 May 2024) **[pre-2025]**. File systems, networking, drivers and the GUI run as separate user-space components. It supports IA-32, AMD64, IA-64, ARM, MIPS, PowerPC, SPARC V9 and RISC-V. The licence is BSD-3 — [Wikipedia](https://en.wikipedia.org/wiki/HelenOS); [helenos.org](https://helenos.org/). The repository is still active (pushed 2026-10-26) and has 97 distinct authors — [GitHub](https://github.com/HelenOS/helenos)

#### Notable newcomers and smaller peers (2025–2026)
- **Moxnox 1.0** (Oct 2026), by Mark Harmstone (author of WinBtrfs):
  - Design: an "NT-like, POSIX-compatible hobby OS" that runs on btrfs and self-hosts its own components.
  - Ports: Bash, coreutils, GCC, LLVM, Git, Python, Perl, Rust, OpenSSH, Vim. GCC and Rust cannot yet be built inside it.
  - Limits: **no GUI**, and it cannot run Windows apps.
  - Size and platforms: about 130,000 lines of C++, on amd64 and aarch64.
  — [README](https://raw.githubusercontent.com/maharmstone/moxnox/v1.0/README.md); [Phoronix](https://phoronix.com/news/Moxnox-OS)
- **Linux-ABI Rust kernels:**
  - **Moss**: repository created 2025-11-20, 1,967 stars, 16 authors, active — [GitHub](https://github.com/hexagonal-sun/moss-kernel)
  - **DragonOS**: 81 authors, pushed 2026-11-07 — [GitHub](https://github.com/DragonOS-Community/DragonOS)
  - **StarryOS**: created 2025-09, 25 authors — [GitHub](https://github.com/Starry-OS/StarryOS)
  - **Maestro**: 6 authors, active — [GitHub](https://github.com/maestro-os/maestro)
  - **Kerla**: 3,468 stars, 14 authors, last push Feb 2026 — [GitHub](https://github.com/nuta/kerla)
- **Other active hobby OSes:**
  - Vinix (V language): 44 authors, active — [GitHub](https://github.com/vlang/vinix)
  - SkiftOS: 16 authors, pushed July 2026 — [GitHub](https://github.com/skift-org/skift); [HN 2025](https://news.ycombinator.com/item?id=45229414)
  - PatchworkOS (non-POSIX, C and assembly, x86_64): active — [GitHub](https://github.com/KaiNorberg/PatchworkOS)
- **Quiet projects:**
  - Theseus: last push 2024-09 — [GitHub](https://github.com/theseus-os/Theseus)
  - Aero: last push 2025-04 — [GitHub](https://github.com/Andy-Python-Programmer/aero)
  - Essence: GitHub last push 2021-08 — [GitHub](https://github.com/nakst/essence). Its author began it around June 2017 to "replace Linux and Windows" — [Handmade Network](https://essence.handmade.network). An HN commenter in 2023 said development seemed to have slowed or stopped by April 2022 — [GeekNews/HN](https://news.hada.io/topic?id=11088)
- **AI-built ("vibe-coded") OSes are a 2026 phenomenon:**
  - NULLos (30 Oct 2026) is closed-source, with its own UEFI bootloader, kernel and self-hosting C compiler. Phoronix says a new OS "coded from scratch" appears each week, "predominantly via AI agents" — [Phoronix](https://www.phoronix.com/news/NULLos) (the page returned 403; this is from the search summary)
  - VibeOS (Jan 2026) was built with Claude prompts. It is C on AArch64 and runs on a Raspberry Pi Zero 2 W. Features: a macOS-like desktop and dock; TCP/IP, DNS and **TLS 1.2 HTTPS**; an HTML/CSS browser; an MP3/WAV player; Doom; and TCC and MicroPython — [Wikipedia](https://en.wikipedia.org/wiki/VibeOS)
  - "Vib-OS ... can't connect to the internet, browser app is an image viewer" — [Tom's Hardware](https://www.tomshardware.com/software/operating-systems/ai-vibe-coded-operating-system-is-so-bad-it-cant-even-run-doom-vib-os-cant-connect-to-the-internet-browser-app-is-an-image-viewer) (headline only)
  - YSERVER, a Rust X11 server written with Claude Code, gets regular Phoronix coverage — [Phoronix](https://www.phoronix.com/news/YSERVER-1.6-Released)
  - AlpineBSD aims to keep AI-generated code out of its base system — [HN digest, 28 Oct 2026](https://zeli.app/digest/2026-10-28)

### Inferences

**C1–C17 scores (0–10, using the shared scale).** These are my judgments from the findings above, not measurements. They assume a general-purpose desktop on real x86 laptops and desktops. A "?" means too little evidence. Under the scale's definition SerenityOS, Haiku and Redox sit at about 5–6 where they are strong. Terminal OS is **provisional and based on README claims only**, since its code is being audited separately.

| OS (version, Nov 2026) | C1 Boot | C2 Kernel/SMP | C3 MM | C4 Proc/IPC/POSIX | C5 Sec | C6 Storage/FS | C7 Net | C8 HW/drivers | C9 Power | C10 GUI/UX | C11 Apps | C12 DevX/SDK | C13 Quality | C14 Perf | C15 A11y/i18n | C16 Install/update | C17 Community |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| SerenityOS (rolling, no releases) | 5 | 5 | 5 | 6 | 5 | 4 | 4 | 3 | 2 | 6 | 5 | 5 | 5 | 4 | 3 | 2 | 5 |
| Haiku (R1/beta6) | 6 | 6 | 5 | 5 | 3 | 6 | 6 | 6 | 3 | 6 | 6 | 6 | 5 | 5 | 4 | 6 | 6 |
| Redox (0.9.0 + 2026 main) | 5 | 5 | 5 | 5 | 5 | 5 | 4 | 4 | 2 | 4 | 5 | 5 | 4 | 4 | 2 | 4 | 6 |
| ReactOS (0.4.16 alpha) | 4 | 4 | 4 | 5 | 2 | 4 | 4 | 5 | 2 | 4 | 4 | 4 | 3 | 4 | 4 | 4 | 5 |
| Fuchsia (F31; desktop lens) | 5 | 8 | 7 | 7 | 8 | 6 | 6 | 3 | 3 | 3 | 2 | 6 | 8 | 6 | 4 | 7 | 4 |
| Genode/Sculpt (26.10) | 6 | 6 | 6 | 5 | 8 | 5 | 6 | 6 | 5 | 4 | 5 | 5 | 6 | 5 | 3 | 5 | 6 |
| Managarm (nightly, Oct 2026) | 4 | 5 | 5 | 6 | 4 | 4 | 4 | 4 | 2 | 5 | 5 | 4 | 4 | 3 | 2 | 3 | 4 |
| Asterinas (0.19; server/VM lens) | 4 | 5 | 5 | 6 | 6 | 4 | 4 | 2 | 1 | 2 | 4 | 5 | 5 | 5 | 1 | 3 | 6 |
| Astral (solo) | 3 | 3 | 4 | 5 | 2 | 3 | ? | 2 | 1 | 4 | 5 | 3 | 2 | 3 | 1 | 2 | 2 |
| ToaruOS (2.3.2) | 4 | 4 | 4 | 4 | 2 | 3 | 3 | 2 | 1 | 5 | 3 | 3 | 3 | 3 | 2 | 3 | 2 |
| HelenOS (0.14.1, 2024) | 4 | 4 | 4 | 3 | 3 | 3 | 3 | 2 | 1 | 3 | 2 | 3 | 3 | 3 | 1 | 2 | 3 |
| Moxnox (1.0; low confidence) | 3 | 3 | 3 | 4 | 2 | 3 | 3 | 2 | 1 | 0 | 4 | 4 | 2 | ? | 1 | 2 | 2 |
| **Terminal OS (README claims, provisional)** | 4 | 3 | 3 | 4 | 3 | 4 | 5 | 4 | 3 | 4 | 3 | 2 | 3 | 3 | 1 | 3 | 1 |

Rationale for the less obvious scores:
- **Haiku**
  - C7 and C8 are 6 because its FreeBSD/OpenBSD driver layer gives it the broadest Wi-Fi and Ethernet coverage in the peer group.
  - C11 is 6, with a case for 7, now that official Firefox has joined Qt apps and HaikuPorts.
  - C5 is 3 because it is effectively single-user.
  - C9 is 3 because suspend isn't mentioned in beta6.
- **SerenityOS**: C16 is 2 (no images or installer) and C6 is 4 (ext2 only).
- **Redox**: C17 is 6, reflecting a nonprofit board, grants and monthly reports. C8 is 4 because it only has a modeset-only Intel GPU driver and no Wi-Fi.
- **Genode/Sculpt**: C5 is 8 (a capability architecture, sold commercially). C8 is 6 because it reuses Linux 6.18 driver code. C9 is 5 because suspend exists but regresses.
- **Fuchsia**: kernel, security, quality and update categories are production-grade at 7–8, but the desktop and app categories score 2–3 because no desktop product exists.
- **Managarm**: C4 is 6 because systemd, Plasma 6 and GTK4 run on it. C14 is 3 because Epiphany cold-starts in about 15 s, against 9 s on Linux.
- **Terminal OS**:
  - C7 is 5 because WPA2 Wi-Fi plus HTTPS exceeds SerenityOS and Redox, which have no Wi-Fi.
  - C2 is 3 because it runs under one big kernel lock.
  - C11 is 3 because there is no ports tree yet.
  - C17 is 1 (1–3 people, 3 stars).

### Gaps
- No verified current team sizes or budgets for Fuchsia (only the 16% layoff figure) or Genode Labs (it says only "a small and tight-knit team", founder-owned since 2008).
- No 2025 Haiku, Inc. financial report was found, and it is unclear whether the contractor started in 2021 or 2022.
- Wi-Fi status for Redox and Managarm wasn't found in their 2025–2026 reports; it is probably absent.
- Astral's real-hardware status is unknown.
- HelenOS's exact founding year wasn't found. Its first release was 11 June 2006, and its GUI date is unknown.
- SerenityOS's self-hosting status wasn't verified.
- The Phoronix NULLos page could not be fetched (HTTP 403).
- Sculpt's first-release date and current browser (Falkon/Chromium after 2021) weren't verified.

---

## 2. Compatibility strategies and their results

### Takeaway
Everything that runs mainstream apps in 2026 got there by impersonating an existing ABI or API, not through native APIs.
- POSIX plus a ports tree: SerenityOS, Haiku and Redox. The result is GCC, Git, Doom and COSMIC apps, and for Haiku, Firefox.
- mlibc with Linux source compatibility: Managarm and Astral. The result is systemd, KDE Plasma 6, Epiphany, Minecraft and Wine.
- Linux ABI emulation: Asterinas, Starnix and HongMeng. The result is NixOS userland, Docker-style containers and Xorg.
- Win32/NT reimplementation sharing Wine code: ReactOS. The result is old Windows apps, but still at the Windows Server 2003 API level after 28 years.

### Cited Findings
- **POSIX plus a ports tree:**
  - SerenityOS's Ports directory carries GCC, Git and Doom — [Wikipedia](https://en.wikipedia.org/wiki/SerenityOS). Its first GCC port came at month 6.4 (2019-04-22) — [Year 1](https://serenityos.org/happy/1st/)
  - Haiku's HaikuDepot and pkgman arrived with beta1 in 2018. It gained Qt4 in 2009, Wine in 2022 and Firefox in 2025–26, the last x86_64 only — [Wikipedia](https://en.wikipedia.org/wiki/Haiku_(operating_system)); [beta6 notes](https://www.haiku-os.org/get-haiku/r1beta6/release-notes/)
  - Redox has about 1,700 work-in-progress ports, and hundreds of Rust programs work unmodified — [0.9.0](https://www.redox-os.org/news/release-0.9.0/). COSMIC Files, Edit and Term were ported in 2024. A GTK3 Orbital backend followed in 2026 — [Phoronix](https://www.phoronix.com/news/Redox-OS-June-2026). WebKitGTK3 layout fixes and dynamic linking for COSMIC came in Sept 2026 — [Sept 2026](https://www.redox-os.org/news/this-month-260930/)
- **mlibc with Linux-like interfaces:**
  - Managarm claims "source-level compatibility with Linux software" and runs systemd as init. It had 454 packages in Jan 2026 — [End of 2025](https://managarm.org/2026/01/13/end-of-year-update.html). By Oct 2026 it had Plasma 6 "usable for daily use" and Linux-compatible `/sys`, `/proc` and netlink uevents — [Oct 2026](https://managarm.org/2026/10/18/oct-update.html). It also runs Epiphany/WebKitGTK with YouTube — [Epiphany](https://managarm.org/2026/10/13/0-epiphany.html)
  - Astral reached Minecraft by fixing mlibc locale and dlopen bugs — [Astral blog](https://astral-os.org/posts/2025/10/31/astral-minecraft.html). Its Wine port runs 32-bit Windows games — [OSnews](https://www.osnews.com/story/145388/astral-is-a-hobby-operating-system-with-x-org-minecraft-and-now-wine/)
  - mlibc itself has 63 distinct authors — [GitHub](https://github.com/managarm/mlibc). The brief names Vinix and Lyre as mlibc-based too; this wasn't independently verified, though both repositories are alive — [Vinix](https://github.com/vlang/vinix), [Lyre](https://github.com/lyre-os/lyre)
- **Linux ABI:**
  - Asterinas supported 206 of Linux 6.7's 368 syscalls on x86 by mid-2025 — [LWN](https://lwn.net/Articles/1022920/). By Oct 2026 it runs NixOS userland and Xorg and builds itself — [Asterinas 0.19](https://asterinas.github.io/2026/10/13/announcing-asterinas-0.19.0.html)
  - Fuchsia's Starnix translates Linux syscalls to Zircon — [OSnews](https://www.osnews.com/topic/fuchsia/)
  - HongMeng achieves "full Linux API/ABI compatibility" through an ABI-compliant shim that redirects Linux syscalls to IPC. It is deployed on tens of millions of devices — [OSDI '24](https://www.usenix.org/conference/osdi24/presentation/chen-haibo)
  - Moss, DragonOS, StarryOS, Maestro and Kerla are smaller Rust Linux-ABI kernels (GitHub data in §1)
- **Windows compatibility:**
  - ReactOS shares Wine code (WineD3D, plus a sync toward Wine 10.0 in progress). Release images still export only Server 2003 APIs, Vista+ exports need a build flag, and WineVDM covers 16-bit apps — [ReactOS 0.4.16](https://reactos.org/project-news/reactos-0416-released/)
  - Reported working apps are old ones: LibreOffice 5.4 and GIMP 2.6 — [Wikipedia](https://en.wikipedia.org/wiki/ReactOS)
  - Moxnox mimics NT syscalls but "cannot run existing Windows applications" — [Phoronix](https://phoronix.com/news/Moxnox-OS)
- **The cost of not having compatibility:** HarmonyOS NEXT dropped AOSP and "is incompatible with Android applications". It had to rebuild its app catalogue: more than 10,000 native apps in Sept 2024, more than 20,000 in Jan 2025, and more than 100,000 native apps by Aug 2026 — [Wikipedia](https://en.wikipedia.org/wiki/HarmonyOS_5); [TechNode](https://technode.com/2026/08/28/huawei-says-harmonyos-native-apps-top-100000-as-harmonyos-6-devices-surpass-80-million/)

### Inferences
- The fastest route from "own kernel" to a modern browser for a tiny team in 2026 is mlibc plus Linux-compatible interfaces plus porting an existing engine. Managarm (WebKitGTK) and Haiku (Firefox) both took that route. Writing an engine (Ladybird) needed a separately funded nonprofit with Platinum sponsors at $100k+, and it still has no alpha after about 7 years.
- Being Linux-like on the outside (`/proc`, `/sys`, netlink, epoll, timerfd, signalfd, DRM/KMS) is what unlocks systemd, GTK4, Plasma and Xorg's modesetting driver. Managarm, Asterinas and Redox (read-only DRM APIs) are all converging there. Terminal OS's choice of mlibc puts it on the same path. The int 0x80 custom ABI matters less than matching Linux semantics in the libc sysdeps and the device and file interfaces.
- Binary compatibility with Windows is a trap for small teams: ReactOS took 28 years to reach the Server 2003 API level. Wine-on-POSIX, as in Astral and Haiku, gets some Windows apps for far less effort.

### Gaps
- No precise 2026 package counts for SerenityOS Ports or HaikuPorts; the GitHub API rate limit was hit, and the `gh` CLI is unavailable.
- No measurement of how many Linux syscalls Managarm or Astral implement.

---

## 3. Driver strategies and their results

### Takeaway
Driver reuse beats from-scratch drivers by a wide margin. Haiku's BSD network-driver layer gives the peer group's best Wi-Fi coverage. Genode tracks Linux 6.18 driver code for Intel GPU, Wi-Fi and USB. Huawei's HongMeng reuses unmodified Linux drivers in "driver containers". ReactOS loads Windows XP/2003-era drivers. From-scratch projects (SerenityOS, Redox, Managarm) still have no Wi-Fi and little or no GPU acceleration after 8–12 years.

### Cited Findings
- **Haiku:** a compatibility layer "runs FreeBSD and OpenBSD network drivers mostly unmodified". Beta6 extends it to USB Wi-Fi and USB Ethernet. There is no hardware-accelerated GPU path; it falls back to an EFI framebuffer or VESA — [beta6 notes](https://www.haiku-os.org/get-haiku/r1beta6/release-notes/). The WLAN port from FreeBSD dates to about 2009 **[pre-2025]** — [Wikipedia](https://en.wikipedia.org/wiki/Haiku_(operating_system))
- **Genode/Sculpt:**
  - Linux-based driver components were upgraded from kernel 6.12 to 6.18, and the USB host-controller driver can be restarted when it faults — [Sculpt 26.10](https://genode.org/news/sculpt-os-release-26.10)
  - It has drivers for "Intel wireless, Intel GPUs" — [Wikipedia](https://en.wikipedia.org/wiki/Genode)
  - Intel GPU acceleration came via Mesa and ported i915 code in 2021 **[pre-2025]** — [Phoronix](https://www.phoronix.com/news/Sculpt-OS-21.10), [Genode 21.08](https://genode.org/news/genode-os-framework-release-21.08)
  - Weak spot: suspend and resume with Wi-Fi on an AX210 — [Discourse](https://genode.discourse.group/t/sculpt-26-04-release-candidates/308?page=2)
- **Redox (user-space drivers, written in Rust):**
  - The first GPU driver arrived in 2025, after about 10 years: Intel Tiger Lake and Kaby Lake, modesetting only — [Dec 2025](https://redox-os.org/news/this-month-251231/)
  - USB arrived after 0.9 (planned order HID, hubs, storage) — [0.9.0](https://www.redox-os.org/news/release-0.9.0/)
  - Laptop results are mixed: a Framework 16 "Broken" in April 2026 — [Apr 2026](https://www.redox-os.org/news/this-month-260430/); an HP ENVY 16 "Booting" in Sept 2026 — [Sept 2026](https://www.redox-os.org/news/this-month-260930/)
- **Managarm (user-space drivers):**
  - Drivers: xHCI, UHCI, EHCI, Intel GPU, Intel HDA, e1000, RTL8168, SATA and NVMe, all behind an IOMMU — [Oct 2026](https://managarm.org/2026/10/18/oct-update.html)
  - nvidia-open modesetting works on real hardware, but there is no 3D because NVIDIA's userland is closed and glibc-linked — [End of 2025](https://managarm.org/2026/01/13/end-of-year-update.html)
  - Hardware CI runs on four boards
- **SerenityOS (from scratch):** no Wi-Fi, no GPU, and two NIC families — [BareMetalInstallation.md](https://github.com/SerenityOS/serenity/blob/master/Documentation/BareMetalInstallation.md)
- **ReactOS (Windows driver ABI):** a new ATA driver, HD audio through a Windows-style bus driver with KMDF imported, and fixes for NVIDIA and AMD vendor drivers — [ReactOS 0.4.16](https://reactos.org/project-news/reactos-0416-released/)
- **HongMeng (production):** "reuses unmodified Linux drivers via a driver container". It started out 3.4× slower than Linux in the paper's baseline case, which motivated the optimizations — [OSDI '24](https://www.usenix.org/conference/osdi24/presentation/chen-haibo)
- **Fuchsia:** a production driver framework, but hardware breadth is limited to Google's Nest Hub products — [Wikipedia](https://en.wikipedia.org/wiki/Fuchsia_(operating_system)); driver framework details weren't retrieved

### Inferences
- Wi-Fi is the clearest dividing line. Only peers that reuse other OSes' drivers (Haiku from BSD, Genode from Linux, ReactOS from Windows) have it broadly. Terminal OS's from-scratch Intel AX2xx driver with WPA2 (per README, legacy a/g rates) is rare among from-scratch hobby OSes. It covers one chipset family, though, and scaling to Realtek, MediaTek or Qualcomm and to HT/VHT/HE rates is exactly the job Haiku solved with a compatibility layer.
- GPU acceleration remains essentially unsolved in the independent tier. Genode's 2021 Mesa/i915 port is the only one cited here. Terminal OS's "GPU-drawn effects on Intel Gen12" would be notable if confirmed, but a 3D API (Mesa/Vulkan) is a different scale of work.
- Restartable user-space drivers (Genode's USB host controller, Managarm's and Redox's user-space drivers) and IOMMU confinement are how these projects buy reliability. A monolithic kernel under one big lock, like Terminal OS, gets none of that isolation.

### Gaps
- No inventory of Fuchsia's DFv2 driver catalogue.
- No confirmation of whether Haiku has any 3D path in 2026.
- No list of which Wi-Fi chipsets Haiku or Genode support in 2026.

---

## 4. What stalled or redirected projects, and what kept successful ones moving

### Takeaway
Projects stall when they depend on a founder, when their scope outruns their staff, or when they run out of money. They keep moving with institutional homes (a nonprofit, a company or a university), a steady public reporting cadence, small reliable grants (NLnet/NGI Zero), mentoring programmes (GSoC, RSoC) and compatibility wins that attract users.

### Cited Findings
- **Founder dependency:**
  - SerenityOS's monthly videos ended in April 2024, and its founder left the maintainer list in Oct 2024. Browser work moved to Ladybird's repository — [Wikipedia](https://en.wikipedia.org/wiki/SerenityOS)
  - The repository is still active in Nov 2026 under maintainers, but there are still no releases — [GitHub](https://github.com/SerenityOS/serenity)
  - ToaruOS, solo since around 2011, archived its GitHub repository on 26 Oct 2026 — [GitHub](https://github.com/klange/toaruos)
  - Essence: no GitHub push since Aug 2021 — [GitHub](https://github.com/nakst/essence), [HN comment](https://news.hada.io/topic?id=11088)
  - Aero (last push Apr 2025) and Theseus (last push Sept 2024) have gone quiet — [Aero](https://github.com/Andy-Python-Programmer/aero), [Theseus](https://github.com/theseus-os/Theseus)
- **Scope versus resources:**
  - Fuchsia: 16% of staff laid off in Jan 2023. Its only products remain Nest Hub devices, now on maintenance updates — [Wikipedia](https://en.wikipedia.org/wiki/Fuchsia_(operating_system)), [9to5Google](https://9to5google.com/2026/10/20/google-rolling-out-minor-nest-hub-update/)
  - Google's 2026 PC OS is Linux-based Googlebook OS, not Fuchsia — [Wikipedia](https://en.wikipedia.org/wiki/Aluminium_OS)
- **ReactOS:**
  - The 2006 code-provenance audit froze the repository; crowdfunding failed in 2013–14 — [Wikipedia](https://en.wikipedia.org/wiki/ReactOS)
  - Release gaps run to years: 0.4.14 (Dec 2021) to 0.4.15 (Mar 2025) to 0.4.16 (Aug 2026), and it is still alpha
- **Haiku:** beta5 to beta6 took about 2 years, with no R1 date — [Haiku news](https://www.haiku-os.org/news/2026-08-26_haiku_r1_beta6/)
- **Redox:** no versioned release since Sept 2024 — [DistroWatch](https://distrowatch.com/table.php?distribution=redox)
- **What sustains them:**
  - Haiku: a 501(c)(3) with record donations of about $31k in 2024, which pays one developer. GSoC brought 3–5 students a year in 2023–2026 — [financial report](https://www.haiku-os.org/news/2025-02-01_2024_financial_report), [Haiku news](https://www.haiku-os.org/news/2026-08-26_haiku_r1_beta6/)
  - Redox:
    - Organisation: a nonprofit board, three NGI Zero grants in 2025 and another in June 2026, and $37k of 2025 income that paid a build engineer, a community manager and RSoC students — [Dec 2025](https://redox-os.org/news/this-month-251231/), [June 2026](https://redox-os.org/news/this-month-260630/)
    - Visibility: monthly "This Month in Redox" posts that Phoronix covers almost every month — [Phoronix March](https://www.phoronix.com/news/Redox-OS-March-2026), [June](https://www.phoronix.com/news/Redox-OS-June-2026), [July](https://www.phoronix.com/news/Redox-OS-July-2026), [Sept](https://www.phoronix.com/news/Redox-OS-September-2026)
  - Managarm: the NGI0 Commons grant (from April 2026) funded web usability work, and Epiphany followed in Oct 2026 — [NLnet](https://nlnet.nl/project/Managarm/), [Epiphany](https://managarm.org/2026/10/13/0-epiphany.html)
  - Genode: a company with commercial licences and quarterly releases since 2008 — [Wikipedia](https://en.wikipedia.org/wiki/Genode)
  - Asterinas: university PhD students plus Ant Group and Intel sponsorship — [LWN](https://lwn.net/Articles/1022920/)
  - Ladybird: about $1M or more from individuals and $100k Platinum sponsors — [ladybird.org](https://ladybird.org/)
- **2026 norms on AI-generated code (relevant to recruiting contributors):**
  - Redox forbids LLM-generated contributions (March 2026) — [Phoronix](https://www.phoronix.com/news/Redox-OS-March-2026)
  - Ladybird uses Claude Code and Codex internally but closed public PRs (June 2026) over contribution quality — [Wikipedia](https://en.wikipedia.org/wiki/Ladybird_(web_browser))
  - Asterinas ships an AI code-review skill — [Asterinas 0.19](https://asterinas.github.io/2026/10/13/announcing-asterinas-0.19.0.html)
  - Phoronix frames the weekly "vibe-coded OS" wave sceptically — [Phoronix](https://www.phoronix.com/news/NULLos)

### Inferences
- A solo project's biggest risk is the bus factor, not technology: ToaruOS (about 15 years, solo) just archived its GitHub repository, and Essence went quiet. SerenityOS survived its founder's exit only because it had already built a large contributor base, with about 1,400 authors and about 21 maintainers.
- Small, steady money matters more than big money for OS projects. NLnet/NGI Zero grants of tens of thousands of euros per work package recur across Redox and Managarm. That is the most realistic funding channel for Terminal OS if it is open source and EU-grant-eligible (applicants worldwide are accepted, but the work must benefit the Next Generation Internet).
- Public cadence compounds. Monthly reports or videos (Redox now, SerenityOS in 2019–2024) turn progress into press coverage and contributors.

### Gaps
- Contributor-retention numbers before and after the SerenityOS leadership change weren't measured.
- There is no stated reason for ToaruOS's GitHub archival; it may be a hosting move rather than an end.

---

## 5. What small teams have realistically achieved: milestone orderings, timelines and team sizes to "daily drivable"

### Takeaway
A single gifted developer working full-time can reach a GUI in about 3 months and an in-house browser rendering pages in about 12 months (SerenityOS, 2018–19). "Daily drivable for technical users" took 12–17+ years with dozens of contributors (Haiku beta1 in 2018; Managarm's Plasma 6 "usable for daily use" in 2026), and nobody in the volunteer tier has reached 1.0-quality mainstream use. Running big unmodified apps (Plasma, Epiphany, Firefox, Minecraft, Wine) is what marks the top achievers, and they reached it through compatibility, not through in-house rewrites.

### Cited Findings
Milestones, measured from first commit or project start:

| OS | Start | First GUI | Networking | Toolchain / self-host | Browser | Real HW | First release / beta | Daily-use claim |
|---|---|---|---|---|---|---|---|---|
| SerenityOS | 2018-10-10 | 2019-01-10 (3 mo) | TCP 2019-03-15 (5 mo) | GCC partial 2019-04-22 (6.4 mo) | Own browser renders page 2019-10-10 (12 mo) | x86-64 bare metal documented (date ?) | None as of 2026 | none |
| Haiku | Aug 2001 | app_server 2005 (~4 y) | Wi-Fi via FreeBSD ~2009 | (self-hosting date not verified) | WebPositive May 2010 (8.7 y); Firefox ~2025 (~24 y) | yes | alpha1 Sept 2009 (8.1 y); beta1 Sept 2018 (17.1 y) | beta-quality, 2026 |
| Redox | 2015-04-20 | Orbital (date ?) | yes | self-hosting not complete (2024) | NetSurf; WebKitGTK3 layout fixes 2026 | Intel GPU modeset 2025 (~10 y) | 0.9.0 Sept 2024 (9.4 y) | none |
| ReactOS | Feb 1998 | early (date ?) | 0.3.0 Aug 2006 (8.5 y) | — | old Windows browsers | yes | 0.4.16 alpha Aug 2026 (28.6 y) | none |
| Fuchsia | Aug 2016 (public) | — | — | — | Chrome Feb 2022 (5.5 y) | Nest Hub May 2021 (4.8 y) | product 2021 | product only |
| Managarm | 2014 | Weston (date ?) | yes | 454 pkgs Jan 2026 (~11.5 y) | Epiphany Oct 2026 (~12 y) | 4-board HW CI | nightly only | Plasma 6 "usable for daily use" (Oct 2026, ~12 y) |
| Asterinas | May 2022 | Xorg Oct 2026 (4.4 y) | virtio | self-hosted build Oct 2026 (4.4 y) | — | VM-focused | first release early 2024 (~1.7 y) | production target: VMs |
| Astral | repo Jul 2022 | X.org (date ?) | ? | GCC ported | — | ? | — | Minecraft Oct 2025 (3.3 y); Wine Apr 2026 (3.7 y) |
| ToaruOS | Dec 2010 | Yutani (date ?) | yes | own libc; GCC, Python ported | — | "tested on real hardware" | 1.0 Jan 2017 (6.1 y); 2.0 Dec 2021 (11 y) | GitHub archived Oct 2026 |
| HelenOS | before 2006 (year ?) | GUI (date ?) | yes | — | — | some | first release Jun 2006; 0.14.1 May 2024 | none |

Sources for the table: [SerenityOS Year 1](https://serenityos.org/happy/1st/); [Haiku Wikipedia](https://en.wikipedia.org/wiki/Haiku_(operating_system)); [Redox 0.9.0](https://www.redox-os.org/news/release-0.9.0/), [Redox Dec 2025](https://redox-os.org/news/this-month-251231/); [ReactOS Wikipedia](https://en.wikipedia.org/wiki/ReactOS); [Fuchsia Wikipedia](https://en.wikipedia.org/wiki/Fuchsia_(operating_system)); [Managarm Jan 2026](https://managarm.org/2026/01/13/end-of-year-update.html), [Managarm Oct 2026](https://managarm.org/2026/10/18/oct-update.html); [Asterinas 0.19](https://asterinas.github.io/2026/10/13/announcing-asterinas-0.19.0.html), [LWN](https://lwn.net/Articles/1022920/); [OSnews on Astral](https://www.osnews.com/story/145388/astral-is-a-hobby-operating-system-with-x-org-minecraft-and-now-wine/); [ToaruOS GitHub](https://github.com/klange/toaruos)

Team sizes:
- SerenityOS: about 1,411 authors ever and about 21 maintainers — [GitHub](https://github.com/SerenityOS/serenity), [LFX](https://insights.linuxfoundation.org/project/serenityos-serenity)
- Redox: more than 40 significant contributors in 2025 — [Dec 2025](https://redox-os.org/news/this-month-251231/)
- Managarm: 7 active per month (Feb 2026) and 32 acknowledged across 2024–25 — [Open Hub](https://openhub.net/p/managarm-os), [Jan 2026](https://managarm.org/2026/01/13/end-of-year-update.html)
- Asterinas: 55 people in one release and more than 100 overall — [0.19](https://asterinas.github.io/2026/10/13/announcing-asterinas-0.19.0.html)
- Haiku: one paid contractor plus volunteers — [beta6 notes](https://www.haiku-os.org/get-haiku/r1beta6/release-notes/)
- Astral and ToaruOS: effectively one person, with 14 and 25 authors respectively — [Astral](https://github.com/mathewnd/Astral), [ToaruOS](https://github.com/klange/toaruos)

Best small-team "proof points" of what is achievable:
- Managarm runs Plasma 6, Epiphany with YouTube, Weston, Sway and systemd on a microkernel — [Oct 2026](https://managarm.org/2026/10/18/oct-update.html), [Epiphany](https://managarm.org/2026/10/13/0-epiphany.html)
- Astral, built solo, runs Minecraft and Wine games — [OSnews](https://www.osnews.com/story/145388/astral-is-a-hobby-operating-system-with-x-org-minecraft-and-now-wine/)
- Redox runs COSMIC apps and a GTK3 backend — [0.9.0](https://www.redox-os.org/news/release-0.9.0/), [Phoronix](https://www.phoronix.com/news/Redox-OS-June-2026)
- Haiku has official Firefox plus HaikuDepot — [beta6 notes](https://www.haiku-os.org/get-haiku/r1beta6/release-notes/)
- SerenityOS built an in-house browser engine that became Ladybird — [Wikipedia](https://en.wikipedia.org/wiki/Ladybird_(web_browser))
- Moxnox self-hosts a GNU userland on its own NT-like kernel — [README](https://raw.githubusercontent.com/maharmstone/moxnox/v1.0/README.md)

### Inferences
- **Two orderings have worked:**
  - (a) *Cohesive in-house stack first*, as SerenityOS did. Kernel, then GUI at 3 months, then networking at 5 months, then a toolchain port at 6 months, then sound and games, then the browser at 12 months. This attracts contributors through demos, but apps outside the in-house set stay scarce.
  - (b) *Compatibility first*, as Managarm, Astral and Asterinas did. Kernel, then POSIX/Linux interfaces via mlibc, then a ports tree (Xorg or Wayland, GTK, SDL, Python, GCC), then big apps. This gets real software sooner and makes the OS testable with conformance suites; os-test was adopted by Redox and Managarm in 2025.
  - Projects that delayed both (HelenOS, ToaruOS) stayed niche.
- **Typical times:**
  - First boot to first GUI: weeks to months.
  - To a self-hosting toolchain or package ecosystem: 1–4 years for focused teams (Asterinas 4.4 years, with institutional staff), much longer otherwise (Genode about 18 years to self-host on Sculpt).
  - To "daily drivable for technical users": about 12–17 years with 7–40 regular contributors.
- **The hard tail is the same everywhere:** GPU acceleration, Wi-Fi breadth, suspend and resume, security hardening and installers or updates. These map to C8, C9, C5 and C16, where nearly every peer scores 1–5.

### Gaps
- First-GUI and real-hardware dates for Redox, Managarm, Astral and ToaruOS weren't retrieved.
- There is no rigorous definition or survey of "daily drivable" across projects.

---

## 6. Has any new OS reached mainstream competitiveness, and what did it cost? (BeOS, Android/ChromeOS, Fuchsia, HarmonyOS NEXT)

### Takeaway
No independent volunteer desktop OS has become commercially competitive. BeOS failed despite a venture-funded company. Google's successes (Android, ChromeOS and now Googlebook OS) all use the Linux kernel, and Fuchsia shipped only on smart displays. The one new kernel to reach mainstream scale is Huawei's HongMeng, in HarmonyOS NEXT. It took about a decade and a reported 10,000+ R&D staff per year, plus multi-billion-yuan ecosystem incentives and a captive, protected domestic market. Even so, it reused Linux drivers and a Linux ABI shim, and its PC version is still small.

### Cited Findings
- **BeOS [pre-2025]:**
  - Founding and size: Be Inc. was founded around Oct 1990 by Jean-Louis Gassée and Steve Sakoman and had 98 employees in 1998. The BeBox shipped in Oct 1995, and the company IPO'd in July 1999.
  - The Apple deal: Apple talked to Be in late 1996, with a rumoured asking price of $500M plus a post, but bought NeXT for $400M instead.
  - Why it failed: Microsoft's OEM agreements "largely prevented OEMs from offering BeOS". Be pivoted to BeIA in Feb 2000 and was sold to Palm for $11M in Aug 2001.
  - Aftermath: Microsoft settled Be's lawsuit for $23.25M in 2003.
  — [Wikipedia: Be Inc.](https://en.wikipedia.org/wiki/Be_Inc.)
- **Google:**
  - Fuchsia's only shipped products are Nest Hub devices (since 2021) — [Wikipedia](https://en.wikipedia.org/wiki/Fuchsia_(operating_system))
  - Google's new laptop OS (Oct 2026) is Linux-based and merges Android and ChromeOS — [Wikipedia](https://en.wikipedia.org/wiki/Aluminium_OS)
- **HarmonyOS NEXT / HongMeng:**
  - Timeline:
    - Announced 4 Aug 2023. Developer preview 18 Jan 2024; public beta 8 Oct 2024; launch 22 Oct 2024; commercial "stable" 19 Nov 2024.
    - The HarmonyOS PC debuted 8 May 2025, after Huawei's Windows licence expired in March 2025.
    — [Wikipedia: HarmonyOS 5](https://en.wikipedia.org/wiki/HarmonyOS_5)
  - Design:
    - It dropped AOSP and is incompatible with Android apps.
    - It uses ArkTS, Cangjie and ArkUI.
    - It uses an ABI shim from Linux syscalls to IPC, and Linux drivers run in driver containers.
    — [Wikipedia: HarmonyOS 5](https://en.wikipedia.org/wiki/HarmonyOS_5), [OSDI '24](https://www.usenix.org/conference/osdi24/presentation/chen-haibo)
  - The HongMeng microkernel project began internally in late 2016 — [Wikipedia](https://en.wikipedia.org/wiki/HongMeng_Kernel). Baidu Baike dates OS planning to 2012, with formal internal approval in May 2016 (secondary) — [Baidu Baike](https://baike.baidu.com/en/item/HUAWEI%20HarmonyOS/979231), [Baidu Baike NEXT](https://baike.baidu.com/en/item/HarmonyOS%20NEXT/1515911)
  - On 21 June 2024 Yu Chengdong said Huawei "took 10 years" to do what Western counterparts did in 30. He also said "more than 900 million devices" ran HarmonyOS, a figure that includes the earlier AOSP-compatible versions, and that more than 2.5 million developers were building for it — [Yahoo Finance/SCMP](https://finance.yahoo.com/news/tech-war-huawei-sees-harmonyos-093000614.html)
  - **Staffing (sources conflict):**
    - Yu Chengdong, in a CCTV interview, spoke of "over 10,000 personnel annually", with R&D spending described as "hundreds of billions" in translation. The Chinese "数百亿" more likely means tens of billions of yuan, and other summaries say "for six years" — [Futu News](https://news.futunn.com/flash/19251055/yu-chengdong-the-harmonyos-system-invests-over-10000-personnel-annually) (headline; body not loaded)
    - Earlier figures: about 4,000+ personnel (2019), and "over 10 years and over 4,000 R&D personnel" — [Baidu Baike](https://baike.baidu.com/en/item/HarmonyOS%20NEXT/1515911)
  - **Ecosystem spending:**
    - $1B pledged for the app ecosystem in Aug 2019 — [HuaweiCentral](https://www.huaweicentral.com/harmonyos-hongmeng-os-everything-you-need-to-know/)
    - A 15-billion-yuan ecosystem fund and a 15,000-person support team — [Baidu Baike](https://baike.baidu.com/en/item/HarmonyOS%20NEXT/1515911) (secondary)
  - **Adoption:**
    - Native apps: more than 10,000 (Sept 2024), more than 20,000 (Jan 2025), more than 100,000 (Aug 2026). More than 400,000 apps are available on HarmonyOS phones. More than 80M devices ran HarmonyOS 6 as of 20 Aug 2026 — [TechNode](https://technode.com/2026/08/28/huawei-says-harmonyos-native-apps-top-100000-as-harmonyos-6-devices-surpass-80-million/), [Wikipedia](https://en.wikipedia.org/wiki/HarmonyOS_5)
    - More than 11 million registered developers — [TMTPost (HDC 2026)](https://www.tmtpost.com/8032363.html) (search summary)
    - About 5% global mobile share in Q1 2025 — [Wikipedia](https://en.wikipedia.org/wiki/HarmonyOS_5)
  - **PC:**
    - Apps: 8,000+ PC apps by Sept 2025 and 10,000+ by Nov 2025 — [Wikipedia: AppGallery](https://en.wikipedia.org/wiki/Huawei_AppGallery); over 17,000 native PC apps by May 2026 — [Baidu Baike](https://baike.baidu.com/en/item/HarmonyOS%20Computer/1431546)
    - Shipments: Omdia counted 141,000 HarmonyOS PCs in 2025 and projects 1.4M in 2026 — [TechRadar](https://www.techradar.com/pro/an-obscure-os-could-surpass-chromeos-by-the-end-of-2027-huaweis-harmony-os-grows-9x-in-12-months-and-its-only-the-beginning)

### Inferences
- What mainstream competitiveness costs, using HarmonyOS NEXT as the existence proof: roughly 10 years, about 10^4 engineers a year, billions of yuan in direct developer incentives, a vertically integrated hardware vendor and a protected market. This is 3–4 orders of magnitude beyond any volunteer OS: Haiku runs on about $31k a year plus one contractor, Redox on about $37k a year plus grants.
- Even the best-funded new kernels reuse Linux driver code and the Linux ABI. "Write every driver yourself" is not a competitive strategy at any budget.
- For a hobby OS, "competitive" should therefore be redefined as becoming a credible, well-engineered peer in the independent tier, not mainstream adoption.

### Gaps
- Huawei's official, unambiguous staffing and investment figures (original CCTV transcript) weren't retrieved, and the translations of 数百亿 conflict.
- There is no reliable HarmonyOS PC user count.
- Fuchsia's staff count at its peak isn't known.

---

## 7. Where a project with Terminal OS's feature set sits on this spectrum

### Takeaway
On README claims, Terminal OS's *hardware-facing breadth* already reaches the 3–5 band, with Wi-Fi/WPA2 and HTTPS ahead of SerenityOS and Redox. That band is "typical early hobby OS" to "comparable to the mature independents". The categories that define maturity sit at 1–3: application ecosystem (no ports tree yet), SMP scalability (one big kernel lock), security, quality engineering, and community and governance. Its closest structural peers are Astral and ToaruOS, both solo and written in C. By feature list, its closest peers are the 2026 AI-era projects (VibeOS, NULLos). So the differentiators it can control are verified depth (conformance tests, hardware reports, real apps through mlibc ports) and transparency, not more features.

### Cited Findings
- **Terminal OS history** (local `git log` of [github.com/BhishanSharma/os](https://github.com/BhishanSharma/os), read 2026-11-07):
  - Activity: first commit 2025-10-02, 146 commits on 23 distinct days. There were 27 commits in Oct 2025, none from Nov 2025 to Sept 2026, 104 in Oct 2026 and 15 in 1–7 Nov 2026. Authors: 111, 27 and 8 commits under three identities.
  - Milestones in the second burst:
    - Oct 2026, first days:
      - 2026-10-08/09: HTTPS via BearSSL.
      - 2026-10-09: DHCP and RTL8168; UEFI boot.
      - 2026-10-10: "Run on a real laptop: APIC timer … USB, I2C touchpad"; Intel AX101 Wi-Fi with WPA2; HDA; NVMe, VFS, TCP sockets, MSI and SMP.
      - 2026-10-11: own UEFI loader and installer; "window server, desktop, Terminal and Files".
      - 2026-10-12: graphical login.
    - Ports, mid to late Oct 2026:
      - 2026-10-17: mlibc port.
      - 2026-10-18: Ladybird port started.
    - Early Nov 2026:
      - 2026-11-05/06: ext2/ext3 permissions; dynamic linking; pseudo-terminals.
      - 2026-11-07: AHCI, USB hubs, HWP/MWAIT C6, CDC-ECM, USB tablets.
  - Size: about 71k lines of C, H and assembly outside obvious third-party directories (rough count).
- **Repository:** 3 stars and 3 distinct authors on GitHub — [GitHub](https://github.com/BhishanSharma/os)
- **Peer comparisons:**
  - SerenityOS needed about 3 months to a GUI and 12 months to an in-house browser — [Year 1](https://serenityos.org/happy/1st/)
  - SerenityOS and Redox have no Wi-Fi — [BareMetalInstallation.md](https://github.com/SerenityOS/serenity/blob/master/Documentation/BareMetalInstallation.md), [Redox Sept 2026](https://www.redox-os.org/news/this-month-260930/)
  - Haiku gets Wi-Fi from BSD drivers — [beta6 notes](https://www.haiku-os.org/get-haiku/r1beta6/release-notes/)
- **The AI-era backdrop:**
  - VibeOS (Jan 2026, Claude-built) lists a GUI, TCP/IP and TLS 1.2 HTTPS, a browser and a music player — [Wikipedia](https://en.wikipedia.org/wiki/VibeOS)
  - Phoronix reports a new from-scratch OS "each week", mostly AI-agent-built — [Phoronix](https://www.phoronix.com/news/NULLos)
  - Redox bans LLM-generated contributions — [Phoronix](https://www.phoronix.com/news/Redox-OS-March-2026)

### Inferences
- **Placement:** Terminal OS's breadth is at the level of a 1–3-year-old serious hobby OS, while its project age is about 13 months calendar and about 5 weeks of the second burst. Its maturity is that of a project weeks old.
  - Most like SerenityOS 2019 in its in-house desktop and apps.
  - Most like Astral in its C, x86-64, mlibc route to ports.
  - Ahead of almost all peers on laptop-specific drivers: I2C-HID, AX2xx Wi-Fi, HWP and C-states.
  - Behind all mature peers on apps, scalability, isolation and process.
- **The pace needs a word of care.** The commit cadence is far outside historical solo norms: whole subsystems such as NVMe, VFS, TCP sockets, MSI and SMP landed in single commits on consecutive days, against SerenityOS's months per subsystem. It matches the 2026 AI-assisted pattern. Whether or not that applies here, reviewers and communities (Phoronix, Redox's policy, AlpineBSD) will *assume* it. Credibility will come from evidence that features survive stress:
  - conformance suites (os-test for POSIX, WPT for the browser);
  - published hardware compatibility reports in Redox's style;
  - a fuzzing and VM test matrix;
  - honest documentation of how the code was written.
- **Lessons from peers that apply directly:**
  1. Finish mlibc and treat Linux-compatible interfaces as the target, so ports of SDL, GTK, Xorg or Wayland, Python, GCC and an existing browser engine (WebKitGTK, as Managarm did) come cheaply. A ports tree is the force multiplier every successful peer has.
  2. Replace the big kernel lock before chasing more drivers. Every mature peer's kernel is fine-grained or microkernel-based.
  3. Generalise Wi-Fi and NIC coverage through a BSD-driver compatibility layer, as Haiku does, rather than one chipset at a time.
  4. Publish a monthly progress report, which is how Redox earns its near-monthly Phoronix coverage.
  5. If the project is open source and its scope fits, apply for an NLnet/NGI Zero grant, as Redox and Managarm have.
  6. Reduce bus-factor risk early with contributor docs, CI and a governance note. Solo projects such as ToaruOS (archived) and Essence (quiet) show the default outcome.
- **Realistic ceiling:** reaching the 5–6 band (Haiku, SerenityOS and Redox level) across most categories historically took 8–17 years and 10–40+ regular contributors. Reaching mainstream (7–10) has only been done with Huawei-scale resources.

### Gaps
- Terminal OS's actual code quality and the real depth of the README features (journaling correctness, TLS validation, SMP safety) are outside this researcher's scope; other researchers are auditing them.
- Whether Terminal OS was AI-assisted is not established here. The cadence analysis is inference only.
