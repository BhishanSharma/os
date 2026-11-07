# Mainstream desktop and laptop OS baseline, state as of 7 November 2026: the bar a new OS must clear to "compete toe to toe"

Conventions used in these notes:
- "Linux desktop" in the scores means a current mainstream distribution (Fedora 45 Workstation or Ubuntu 26.04 LTS / 26.10) running GNOME 50/51 or KDE Plasma 6.8 on a Linux 7.x kernel.
- Items marked "(general knowledge, not re-verified in this session)" are long-established facts I did not re-check against a source this session. They appear only in Inferences or Gaps, never in Cited Findings.
- Many 2026 facts come from news coverage rather than vendor pages. Where only one or a weak source exists, the finding says so.

## 1. Current versions and headline 2025-2026 features (Windows 11 and Windows 12 news, macOS, Linux kernel, GNOME, KDE Plasma, Ubuntu, Fedora, ChromeOS)

### Takeaway
As of 7 Nov 2026 the current releases are: Windows 11 version 26H2 (rollout from 29 Sep 2026, an enablement package on the shared 24H2/25H2 platform), with a separate 26H1 "Bromine" branch only on Snapdragon X2 PCs and no announced Windows 12; macOS 27 "Golden Gate" (14 Sep 2026, Apple silicon only, the last release with general Rosetta 2); Linux 7.3 (18 Oct 2026; 6.18 is the current LTS); GNOME 51 (16 Sep 2026); KDE Plasma 6.8 (14 Oct 2026, Wayland-only); Ubuntu 26.04 LTS (23 Apr 2026) and 26.10 (15 Oct 2026); Fedora 45 (3 Nov 2026). ChromeOS is being succeeded on premium laptops by the Android-based "Googlebook OS" (on sale from 4 Oct 2026), while existing Chromebooks remain supported. The 2026 headline themes are AI assistants and agents, Wayland-only Linux desktops, Apple's move to an all-ARM lineup, Rust in kernels, post-quantum cryptography defaults, and, at Microsoft, an admitted quality reset.

### Cited Findings

#### Windows 11 (and Windows 12 news)
- Windows 11 26H2 was released on 29 Sep 2026 with a gradual rollout. It is delivered as an enablement package that switches on code already present on 25H2/24H2 and resets the support clock. It brings no new hardware requirements. Listed features include an "Agent in Settings", NPU activity monitoring in Task Manager, Start/taskbar personalisation, and Search/File Explorer improvements, though many shipped to 25H2 earlier through cumulative updates — [Pureinfotech](https://pureinfotech.com/windows-11-26h2-released-force-install/). A later Pureinfotech piece says 26H2 became fully available to all eligible PCs (21 Oct 2026 per search summary) — [Pureinfotech](https://pureinfotech.com/?p=192987)
- 26H2 shares the 25H2 platform release, so it has no features that 25H2 lacks — [Yahoo Tech](https://tech.yahoo.com/computing/articles/windows-11-version-26h2-everything-003147304.html). Support is 24 months for Home/Pro and 36 months for Enterprise/Education. The update is small and needs one restart — [Thurrott](https://www.thurrott.com/?p=342194)
- 26H2 adds "Administrator protection" (just-in-time admin privileges with a separate admin profile). It is off by default and configurable through Intune or Group Policy — [FoneArena](https://www.fonearena.com/blog/?p=493781)
- Windows 11 26H1 ships only on Qualcomm Snapdragon X2 PCs (from spring 2026). It is built on the new "Bromine" platform (build 28000), not the "Germanium" platform shared by 24H2/25H2. It adds no visible features and will not move to 26H2. A merged release is expected around 27H2 — [Windows Central](https://www.windowscentral.com/microsoft/windows-11/windows-11-version-26h1-will-launch-exclusively-on-snapdragon-x2-devices-this-spring); [Windows Central](https://www.windowscentral.com/microsoft/windows-11/microsoft-confirms-that-windows-11-version-26h1-will-not-be-updated-to-version-26h2-this-fall-wont-get-a-new-version-until-2027); [igor'sLAB](https://www.igorslab.de/en/windows-11-26h1-platform-change-without-glitter-and-thats-why-its-important/); [Pureinfotech](https://pureinfotech.com/windows-11-26h2-released-force-install/)
- Windows 12: I found no 2026 announcement. Coverage says Microsoft has not officially announced Windows 12 or a date — [TweakTown](https://www.tweaktown.com/hubs/windows-12/). Windows 12 was effectively pushed back when Microsoft continued with Windows 11 25H2 — [Tom's Hardware](https://www.tomshardware.com/software/windows/windows-12-release-is-pushed-back-at-least-another-year-as-microsoft-announces-windows-11-version-25h2)
- On 20 Mar 2026, Windows chief Pavan Davuluri published a quality plan for Windows 11. It covers performance, reliability and "craft"; "reducing unnecessary Copilot entry points" (Snipping Tool, Photos, Widgets, Notepad); top and side taskbar positions; a "faster and more dependable" File Explorer with lower latency; a lower baseline memory footprint; a one-reboot-a-month update option; faster installs; a new Feedback Hub; and WSL file and network performance improvements — [The Register](https://theregister.com/2026/03/23/windows_quality_commitment/); [Windows Central](https://www.windowscentral.com/microsoft/windows-11/microsoft-promises-2026-will-be-a-better-year-for-windows-11-confirms-plans-to-address-pain-points-across-the-os); [Redmond Magazine](https://redmondmag.com/articles/2026/03/24/microsoft-dials-back-copilot.aspx)
- On 31 Jul 2026, under its "Windows K2" effort, Microsoft made memory optimisation for 8 GB PCs a top priority. One measure: on PCs with 8 GB or less, Widgets load only when opened, freeing more than 200 MB — [Pureinfotech](https://pureinfotech.com/windows-11-8gb-memory-optimization/)
- "Agentic OS" work: Windows gained Model Context Protocol (MCP) support, a preview On-Device Registry of "agent connectors" (which are MCP servers), and an "agent workspace" described as isolated, policy-controlled and auditable — [Neowin](https://www.neowin.net/amp/microsoft-is-making-ai-agents-a-first-class-component-of-windows/); [Techspective](https://techspective.net/2025/11/18/windows-becoming-operating-system-for-ai-agents/). Microsoft remains committed to an "agentic OS" despite criticism — [ITPro](https://www.itpro.com/technology/artificial-intelligence/microsoft-is-unwavering-on-making-windows-an-agentic-os-); critics object that agents can access user files — [PC Gamer](https://www.pcgamer.com/software/windows/apparently-windows-11-becoming-agentic-ai-means-letting-the-bots-rummage-through-some-of-your-files)
- Windows 10: Microsoft has extended free consumer Extended Security Updates to 12 Oct 2027 — [Help Net Security](https://www.helpnetsecurity.com/2026/06/26/microsoft-windows-10-free-security-updates-esu-program/); [Thurrott](https://www.thurrott.com/?p=337939)

#### macOS
- macOS Tahoe 26 (announced June 2025) introduced the Liquid Glass design; Spotlight actions ("hundreds of actions directly from Spotlight"); Metal 4 with MetalFX Frame Interpolation and Denoising; the Apple Games app; the Phone app on Mac; on-device Live Translation; and new accessibility features (Magnifier for Mac, Accessibility Reader, Braille Access, Vehicle Motion Cues) — [Apple Newsroom](https://www.apple.com/newsroom/2025/06/macos-tahoe-26-makes-the-mac-more-capable-productive-and-intelligent-than-ever/)
- Apple's open-source Containerization framework (WWDC 2025, Swift) runs each Linux container in its own lightweight VM. It uses a Swift init process (vminitd) and needs macOS 26 for full functionality — [The Register](https://www.theregister.com/2025/06/10/apple_tries_to_contain_itself/); [InfoQ](https://www.infoq.com/news/2025/06/apple-container-linux)
- macOS 27 "Golden Gate" was released on 14 Sep 2026, and 27.2 followed on 26 Oct 2026. It is Apple silicon only: Macs from 2020 on, plus the new MacBook Neo with an A18 Pro chip. macOS 26 Tahoe was the last release for Intel Macs. Headline features: Siri AI (beta; dedicated app, Spotlight integration, on-screen context, Visual Intelligence), with the most advanced features needing an M3 or later Mac with at least 12 GB; system-wide Apple Intelligence tools; refined Liquid Glass with adjustable transparency; faster app launches, Spotlight indexing, AirDrop and networking; 5K at 120 Hz on external displays; Game Porting Toolkit 4 (GTA V on an M4 Pro MacBook Pro reportedly went from about 106 to 176 fps); new parental controls; and SynthID marking of AI images (27.2) — [Macworld](https://www.macworld.com/article/3139330/macos-27-golden-gate-has-arrived-heres-what-s-new-on-your-mac.html)
- macOS 27 is the last version that supports Rosetta 2 for general use. Intel-only apps stop working in macOS 28, though limited Rosetta stays for some older games. Some Intel Macs may get security patches for older macOS versions until Sep 2028 — [Macworld](https://www.macworld.com/article/3139330/macos-27-golden-gate-has-arrived-heres-what-s-new-on-your-mac.html)
- Apple's 2026 Apple Foundation Models were reportedly built in collaboration with Google's Gemini family, and Apple can distil smaller on-device models from Gemini. These are secondary reports — [MacRumors](https://www.macrumors.com/2026/03/25/apple-google-gemini-distill-models/); [Firebase blog](https://firebase.blog/posts/2026/06/apple-foundation-models-gemini)

#### Linux kernel
- Linux 7.0 was released on 12 Apr 2026. Rust is no longer labelled experimental. The release adds a generic filesystem error-reporting framework, XFS self-healing, NULLFS (an empty immutable root filesystem), and enablement for Intel Nova Lake, AMD Zen 6, ARM64 64-byte atomic loads and stores, and the Apple USB Type-C PHY. It became the default kernel in Ubuntu 26.04 LTS and Fedora 44. Linux 6.18 remains the LTS branch, supported through Dec 2028 — [Slashdot/Linuxiac](https://linux.slashdot.org/story/26/04/13/1857240/linux-70-released); [AlternativeTo](https://alternativeto.net/news/2026/4/linux-7-0-released-with-xfs-self-healing-agent-agnostic-ai-keys-and-major-hardware-support/); [OSTechNix](https://ostechnix.com/linux-kernel-7-0-released/)
- Linux 7.2 was released on 16 Aug 2026, on schedule despite heavy patch and bug-report volume from AI tools. It adds Cache Aware Scheduling, the USB4STREAM protocol, AMDGPU HDMI 2.1 FRL, and Intel and AMD driver work — [Phoronix](https://www.phoronix.com/news/Linux-7.2-Released). Torvalds called the unusually large rc7 fix batch a "new normal" — [Let's Data Science](https://letsdatascience.com/news/torvalds-releases-linux-72-rc7-amid-ai-fixes-1d6ee190)
- Linux 7.3 was released on 18 Oct 2026. Changes:
  - Scheduling: sched_ext cgroup sub-schedulers (a BPF scheduler per cgroup) and a cgroup scheduling rework.
  - Sandboxing: the failfs pseudo-filesystem plus a new fchroot(2) syscall.
  - Filesystems: Btrfs fsync about 5x faster with no-holes; faster ext4 parallel direct I/O and fallocate; NTFS read support for Windows System Compression; idmapped overlayfs mounts; freevxfs and EFS removed.
  - Memory and GPU: MGLRU protection for executable pages; KSM worst-case lock hold cut from over 500 ms to under 2 ms; VRAM eviction via the dmem cgroup for low-VRAM GPUs.
  - Rust: netlink, fwctl and SRCU abstractions.
  — [KernelNewbies](https://kernelnewbies.org/Linux_7.3). Also: ksmbd now works with Apple Time Machine; Steam Controller and MSI Claw drivers; initial IBM Power12; Rust on PowerPC. Ubuntu 26.04 LTS gets 7.3 through its HWE stack in early 2027 — [OMG! Ubuntu](https://www.omgubuntu.co.uk/2026/10/linux-7-3-features); [Gigazine](https://www.gigazine.net/gsc_news/en/20261020-linux-kernel-7-3/)
- bcachefs was marked "externally maintained" in 6.17 (Aug 2025) and removed from mainline in 6.18. It now ships as a DKMS module; Fedora offers it only through COPR, and Debian packaging was orphaned — [Linux Journal](https://www.linuxjournal.com/content/bcachefs-ousted-mainline-kernel-move-dkms-and-what-it-means)

#### GNOME
- GNOME 50 removed the X11 session code from Mutter and GNOME Shell, leaving Wayland only, with X11 apps running through XWayland. It was scheduled for 18 Mar 2026 and powers Ubuntu 26.04 LTS and Fedora Workstation 44 — [Phoronix](https://www.phoronix.com/news/GNOME-50-Alpha); [Phoronix](https://www.phoronix.com/news/GNOME-Mutter-Shell-50-Alpha); [Linuxiac](https://linuxiac.com/gnome-50-ends-the-x11-era-after-decades/)
- GNOME 51 "A Coruña" was released on 16 Sep 2026. Changes:
  - Graphics: Mutter frame-clock changes to reduce dropped frames; per-monitor brightness and colour mode, including HDR, saved and restored; GTK fractional-scale rendering; SVG cursors.
  - Login and lock screen: FIDO2/passkey sign-in, web-based login (QR codes and URLs) and smartcard support in GDM.
  - Accessibility: a Reduced Motion toggle and improved screen-reader behaviour.
  - Software app: Flatpak end-of-life warnings and clearer permissions.
  - NVIDIA: legacy driver interfaces removed.
  - Apps: offline Maps; Sushi previewer ported to GTK 4.
  — [OMG! Ubuntu](https://www.omgubuntu.co.uk/2026/09/gnome-51-released); [GNOME Discourse](https://discourse.gnome.org/t/gnome-51-released/38820). Search coverage also reports GNOME 52 scheduled for 17 Mar 2027 (not fetched directly) — [UbuntuHandbook](https://ubuntuhandbook.org/index.php/2026/09/gnome-51-released-to-be-default-in-ubuntu-26-10-fedora-45/)

#### KDE Plasma
- Plasma 6.8 was released on 14 Oct 2026, KDE's 30th anniversary. Changes:
  - Session: starting X11 sessions is retired, though X11 apps still run through XWayland and KDE apps keep X11 support.
  - Colour: better HDR with ICC profiles; HLG transfer support.
  - Accessibility: a built-in dwell clicker; an Amharic on-screen keyboard.
  - Performance: faster start and lower memory use; NVIDIA triple buffering on by default.
  - Remote desktop: exclusive mode with a virtual screen; two-way clipboard sync.
  — [KDE](https://kde.org/announcements/plasma/6/6.8.0/)
- Also: Kup backup integrated into Plasma; Union theming (a tech preview) extended to Qt Widgets; System Monitor can pin processes to CPU cores; Spectacle records microphone and system audio; Bikram Sambat calendar support. Kubuntu 26.10 does not ship 6.8 — [OMG! Ubuntu](https://omgubuntu.co.uk/2026/10/kde-plasma-6-8-released)

#### Ubuntu
- Ubuntu 26.04 LTS "Resolute Raccoon" was released on 23 Apr 2026. It gets five years of standard support (to Apr 2031), and Ubuntu Pro extends that to ten years. Desktop requirements are a 2 GHz dual-core CPU, 6 GB RAM and 25 GB of storage; server starts at 1.5 GB RAM and 4 GB of storage — [Ubuntu release notes](https://documentation.ubuntu.com/release-notes/26.04/)
- Secondary coverage of 26.04 reports Linux 7.0, GNOME 50 (Wayland-only), systemd 259, a Dracut initramfs and APT 3. It also reports hybrid post-quantum algorithms as defaults in OpenSSL and OpenSSH 10.2; this wording may be garbled, and ML-KEM key exchange is the likely default. TPM-backed full-disk encryption is moving toward general availability, and AppArmor gains profiles plus user-facing permission prompts for snaps — [OSTechNix](https://ostechnix.com/ubuntu-26-04-lts-resolute-raccoon-features-release-notes/); [DebugPoint](https://debugpoint.com/ubuntu-26-04-lts); [Cherry Servers](https://www.cherryservers.com/blog/ubuntu-2604-lts-features-release-date)
- Ubuntu 26.10 "Stonking Stingray" was released on 15 Oct 2026, with support until Jul 2027. It ships Linux 7.3 (an RC kernel on the ISO, with the final via update) and GNOME 51, plus a separate amd64v3 image. Other changes: full adoption of Rust-based coreutils, post-quantum-ready package signing, expanded TPM-backed FDE, an opt-in speech-to-text feature ("Myna"), and a mid-drag window-tiling widget — [UbuntuHandbook](https://ubuntuhandbook.org/index.php/2026/10/ubuntu-26-10-released-with-kernel-7-3-gnome-51-amd64v3-image/); [OMG! Ubuntu](https://www.omgubuntu.co.uk/2026/10/ubuntu-2610-new-features). dbus-broker replaces the 22-year default dbus-daemon — [Warp2Search](https://www.warp2search.net/story/ubuntu-linux-2610-beta-freeze-october-15-launch-kernel-73-and-a-22year-change/)

#### Fedora
- Fedora 45 was released on 3 Nov 2026, after slipping twice from 20 Oct because of blocker bugs. It ships GNOME 51, KDE Plasma 6.8 and Linux 7.2, across Workstation, KDE, Server, Cloud, CoreOS/IoT, the Atomic Desktops and the Asahi Remix for Apple silicon (2 Nov). Fedora 43 reaches end of life in early Dec 2026 — [GamingOnLinux](https://www.gamingonlinux.com/2026/11/fedora-45-arrives-to-continue-advancing-the-linux-desktop/); [Comss.ru](https://www.comss.ru/page.php?id=22714); [Linuxiac](https://linuxiac.com/?p=221866); [Phoronix](https://phoronix.com/news/Fedora-45-Released)
- The Fedora 45 Beta introduced the KMSCON console — [Phoronix](https://www.phoronix.com/news/Fedora-45-Beta)

#### ChromeOS and Googlebook OS (the merger into Android)
- Googlebook was announced in May 2026 at "The Android Show: I/O Edition", alongside Android 17. Preorders opened on 21 Sep 2026; it went on sale in the US on 4 Oct 2026 and in the UK, Germany, France, Canada, Australia and Ireland on 5 Oct. The OS is "Googlebook OS", an "Android-based desktop operating system" ("Aluminium" was only a codename). Launch OEMs are Acer, Asus, Dell, HP and Lenovo. Hardware requirements are a computer-class Arm64 or x64 SoC, UEFI boot and NVMe storage. Googlebooks are positioned at the premium end, Chromebooks continue to be supported, and some Android apps reportedly ran poorly on Intel Googlebooks at launch (citing XDA) — [Wikipedia](https://en.wikipedia.org/wiki/Googlebook); [PCWorld](https://www.pcworld.com/article/3241100/googles-new-googlebook-os-is-basically-android-for-laptop.html)
- One source reports prices from $899 (single source) — [Mezha](https://mezha.ua/en/news/googlebook-os-stane-novoyu-os-google-315394/amp/). Court filings reportedly suggest ChromeOS may stay on the market until 2034 during a multi-year transition (secondary) — [StarryHope](https://www.starryhope.com/chromebooks/chromeos-android-merger-aluminium-os/)

### Inferences
- Windows "versions" are now mostly marketing and servicing labels: 26H2 is an enablement package on the 2024 platform. The real platform change (Bromine) is fenced off on Snapdragon X2 until roughly 27H2. A newcomer is competing with a mature platform that changes slowly underneath, on a yearly naming cadence.
- 2026 is the year mainstream Linux desktops went Wayland-only (GNOME 50 in March, Plasma 6.8 in October). For a new OS, a compositing display server with per-window buffers, rather than X11-style shared drawing, is now the expected architecture. Terminal OS's compositing window server is aligned with this (per its README).
- Apple completed its ARM transition: macOS 27 is Apple-silicon-only, and Rosetta 2 ends with it. All three major vendors now treat ARM64 laptops as first-class (Windows 26H1 on Snapdragon X2, every Mac, Googlebook on Arm64 or x64). An x86_64-only newcomer covers most of the installed base but not where the market is heading.
- Google has, in effect, merged ChromeOS into Android on the PC. Its new OS requires UEFI and NVMe, which shows that even a "simple" consumer OS assumes a modern UEFI, NVMe and Arm64-or-x64 platform.

### Gaps
- No official Microsoft statement on Windows 12 or a "next Windows" platform was found for 2026. The 26H2 build number and the exact full-availability date (29 Sep start versus 21 Oct "fully released") were not pinned down from a Microsoft page.
- The Linux 7.1 release date and features were not researched. GNOME 50's final release date (18 Mar 2026 was the schedule) was not confirmed.
- The current ChromeOS version number and its support policy for existing Chromebooks were not found. Googlebook OS technical details (Linux app containers, Chrome extensions, update model) were not verified.
- Apple's own documentation on the Gemini relationship and the Foundation Models framework was not fetched. Treat the Gemini claims as secondary reporting.

## 2. Table stakes versus differentiators in each category (C1-C17), and 0-10 scores for Windows 11, macOS and the Linux desktop

### Takeaway
Best-in-class in 2026 means the following. Verified Secure Boot on UEFI x86_64 and ARM64, with TPM- or enclave-sealed full-disk encryption. Fine-grained SMP with hybrid-core-aware, even pluggable, scheduling. Sandboxed, signed apps, passkeys and post-quantum crypto defaults. Copy-on-write or journaling filesystems with snapshots and backup. Wi-Fi 6E/7, WPA3, IPv6, TLS 1.3 and LE Audio. Broad GPU, Wi-Fi, audio and input drivers. Reliable modern standby and 15+ hour battery life on ARM. An HDR-, VRR- and fractional-scaling-capable compositor on Vulkan, DirectX 12 or Metal 4-class APIs. Thousands of maintained apps plus compatibility layers. A complete toolchain with containers and VMs. A screen reader covering the shell, plus captions and voice control. Staged and atomic updates with rollback and MDM. A sustainable contributor base. On the shared scale, Windows 11, macOS and the Linux desktop sit at 9-10 in most categories. The scores below 9: Windows on power, reliability and performance (8); macOS on hardware breadth (8); Linux on security defaults (8), installation and administration (8), applications (8), power (7) and accessibility (7).

### Cited Findings

#### C1 Boot, firmware and platform
- Windows 11 baseline: a 64-bit CPU with POPCNT, 4 GB RAM, 64 GB storage, UEFI with Secure Boot, and TPM 2.0 — [Pureinfotech](https://pureinfotech.com/windows-11-26h2-released-force-install/)
- Secure Boot certificate rotation in 2026: the Microsoft Corporation UEFI CA 2011, which signs third-party boot components including the Linux shim, expired on 26-27 Jun 2026. The KEK CA 2011 expired on 24 Jun 2026, and the Windows Production PCA 2011 expires in Oct 2026. Replacement 2023 CAs are valid until 2038. Firmware does not check expiry at boot, so existing installs still boot. But future shims signed only with the 2023 key need the 2023 CA in the firmware db, and some new OEM machines ship with only the 2023 certificates — [Red Hat](https://www.redhat.com/es/blog/expiration-secure-boot-signing-certificates-2026); [LWN](https://lwn.net/Articles/1079808/); [AlmaLinux wiki](https://wiki.almalinux.org/documentation/secure-boot-2023-certificates); [Debian wiki](https://wiki.debian.org/SecureBoot/CAChanges)
- ARM64: Windows 11 26H1 is exclusive to Snapdragon X2 — [Windows Central](https://www.windowscentral.com/microsoft/windows-11/windows-11-version-26h1-will-launch-exclusively-on-snapdragon-x2-devices-this-spring). macOS 27 is Apple silicon only — [Macworld](https://www.macworld.com/article/3139330/macos-27-golden-gate-has-arrived-heres-what-s-new-on-your-mac.html). Googlebook requires an Arm64 or x64 SoC, UEFI and NVMe — [Wikipedia](https://en.wikipedia.org/wiki/Googlebook)
- Linux on Snapdragon X Elite laptops ended 2025 "disappointing". Linux support and performance are better on AMD Ryzen AI and Intel Core Ultra laptops — [Phoronix](https://www.phoronix.com/review/snapdragon-x-elite-linux-eoy2025). A single source says Qualcomm promised official Linux support for Snapdragon X2 (Debian first, with Ubuntu certification) — [Ecosistema Startup](https://ecosistemastartup.com/?p=110851)
- Linux 7.0 added Intel Nova Lake and AMD Zen 6 enablement and ARM64 64-byte atomics — [OSTechNix](https://ostechnix.com/linux-kernel-7-0-released/). Ubuntu 26.10 offers an amd64v3-optimised image — [UbuntuHandbook](https://ubuntuhandbook.org/index.php/2026/10/ubuntu-26-10-released-with-kernel-7-3-gnome-51-amd64v3-image/)
- Boot timing on identical Dell XPS 13 laptops (Intel Core 5 320, 8 GB; PhoneBuff, Sep 2026): cold boot took 23 s on Windows 11 and 24 s on Fedora; median restart took 61 s on Windows and 27 s on Fedora. This is a single automated test — [It's FOSS](https://itsfoss.com/news/fedora-beats-windows-11/); [Comss.ru](https://www.comss.ru/page.php?id=21870)

#### C2 Kernel architecture, scheduling and SMP
- Linux 7.2 added Cache Aware Scheduling — [Phoronix](https://www.phoronix.com/news/Linux-7.2-Released). Linux 7.3 added sched_ext cgroup sub-schedulers (each cgroup can run its own BPF scheduler); reworked cgroup scheduling ("flatten the pick", a single runqueue); lowered latency for short-slice tasks; and fixed cluster scheduling on asymmetric-capacity CPUs — [KernelNewbies](https://kernelnewbies.org/Linux_7.3)
- Desktop tools now expose CPU affinity: Plasma 6.8's System Monitor can assign processes to specific cores — [OMG! Ubuntu](https://omgubuntu.co.uk/2026/10/kde-plasma-6-8-released)

#### C3 Memory management
- Linux 7.3: userfaultfd working-set tracking (UFFDIO_REGISTER_MODE_RWP); MGLRU protection for mapped executable pages; per-cgroup proactive zswap writeback; KSM rmap lock hold cut from over 500 ms to under 2 ms; dmem-cgroup VRAM eviction for low-VRAM GPUs — [KernelNewbies](https://kernelnewbies.org/Linux_7.3); [OMG! Ubuntu](https://www.omgubuntu.co.uk/2026/10/linux-7-3-features)
- Windows memory footprint: a secondary report puts a fresh 25H2 install at about 3.3 GB idle, about 0.8-1 GB more than Windows 10 22H2 — [unwire.hk](https://unwire.hk/2026/03/24/windows11-update-ram/fun-tech/). Microsoft pledged to lower the baseline footprint — [The Register](https://theregister.com/2026/03/23/windows_quality_commitment/) — and launched the K2 work for 8 GB PCs — [Pureinfotech](https://pureinfotech.com/windows-11-8gb-memory-optimization/). Users report lower idle RAM after 26H2 (anecdotal) — [Guru3D](https://www.guru3d.com/story/windows-11-26h2-users-report-lower-idle-ram-use-and-faster-launches/)
- After boot on 8 GB, Windows 11 (with Dell utilities, Defender and WebView2) used 85.2% of RAM versus about 31% for Fedora (PhoneBuff) — [It's FOSS](https://itsfoss.com/news/fedora-beats-windows-11/)
- Apple brought Memory Integrity Enforcement (MIE), hardware memory safety built on Arm's Memory Tagging Extension, to M5 Macs — [Computerworld](https://www.computerworld.com/article/4124435/apple-touts-unparalleled-protection-for-m5-macs.html); [9to5Mac](https://9to5mac.com/2026/05/14/calif-team-details-how-anthropic-mythos-helped-build-a-working-macos-exploit-in-five-days/)

#### C4 Processes, threads, IPC, signals and syscall/POSIX surface
- Linux keeps growing the syscall and sandbox surface: Linux 7.3 added failfs plus fchroot(2) for sandboxing, made syscall_user_dispatch configurable, and improved the Rust binder IPC driver — [KernelNewbies](https://kernelnewbies.org/Linux_7.3)
- Windows' POSIX and Linux surface comes through WSL, which Microsoft open-sourced on 19 May 2025 (some pieces such as lxcore.sys remain in the Windows image) — [Windows Developer Blog](https://blogs.windows.com/windowsdeveloper/2025/05/19/the-windows-subsystem-for-linux-is-now-open-source/). Microsoft committed in 2026 to faster Linux-Windows file performance and network throughput in WSL — [The Register](https://theregister.com/2026/03/23/windows_quality_commitment/)
- macOS runs Linux workloads through Containerization, one lightweight VM per container — [The Register](https://www.theregister.com/2025/06/10/apple_tries_to_contain_itself/)

#### C5 Security and privacy
- Windows: Smart App Control, which combines app-intelligence reputation with code-integrity enforcement and has existed since 22H2, could be turned on or off without reinstalling from the April 2026 update for 25H2 — [Anoop C Nair](https://www.anoopcnair.com/windows-11-april-2026-update-smart-app-control/); [Microsoft Learn](https://learn.microsoft.com/en-us/windows/security/book/application-security-application-and-driver-control). The October 2026 update KB5127648 broke the Smart App Control toggle on 24H2, 25H2 and 26H2 for some PCs — [BleepingComputer](https://bleepingcomputer.com/news/microsoft/kb5127648-windows-security-update-may-break-smart-app-control). Administrator protection is in 26H2 but off by default — [FoneArena](https://www.fonearena.com/blog/?p=493781)
- Windows Resiliency Initiative (Nov 2024, after CrowdStrike): Quick Machine Recovery and moving antivirus and endpoint protection out of the kernel into user mode, with a private preview for security partners slated for July 2025 — [The Hacker News](https://thehackernews.com/2024/11/microsoft-launches-windows-resiliency.html); [heise](https://heise.de/-10462904)
- Microsoft and Rust: one Microsoft engineer's stated goal is to eliminate C/C++ at Microsoft by 2030 with AI-assisted translation. Microsoft denied plans for a full Rust port of Windows by 2030 — [Windows Central](https://www.windowscentral.com/microsoft/windows-11/my-goal-is-to-eliminate-every-line-of-c-and-c-from-microsoft-by-2030-microsoft-bets-on-ai-to-finally-modernize-windows); [igor'sLAB](https://www.igorslab.de/en/microsoft-puts-the-brakes-on-speculation-about-a-full-rust-port-of-windows/)
- macOS: MIE on M5 is hardware memory tagging. Researchers at Calif built the first public macOS kernel memory-corruption exploit on M5, a local privilege escalation to root, in five days with help from Anthropic's Mythos Preview — [9to5Mac](https://9to5mac.com/2026/05/14/calif-team-details-how-anthropic-mythos-helped-build-a-working-macos-exploit-in-five-days/); [Calif](https://blog.calif.io/p/first-public-kernel-memory-corruption); [Tom's Hardware](https://www.tomshardware.com/tech-industry/cyber-security/apple-m5-architecture-suffers-first-privilege-escalation-exploit-anthropics-claude-mythos-helps-researchers-bypass-memory-integrity-enforcement). macOS 27.2 adds SynthID marking for AI-generated images — [Macworld](https://www.macworld.com/article/3139330/macos-27-golden-gate-has-arrived-heres-what-s-new-on-your-mac.html)
- Linux: at the 2025 Maintainers Summit, the consensus was that Rust in the kernel is no longer experimental; the label was removed in 7.0. Rust is used mostly for new drivers. Greg Kroah-Hartman said Rust drivers are proving safer, and DRM maintainer Dave Airlie said DRM is about a year from requiring Rust for new drivers — [LWN](https://lwn.net/Articles/1047293); [Phoronix](https://www.phoronix.com/news/Rust-To-Stay-Linux-Kernel); [Linux Magazine](https://www.linuxpromagazine.com/index.php/layout/set/print/Online/News/Kernel-7.0-Is-a-Bit-More-Rusty)
- Ubuntu: 26.04 brings post-quantum hybrid crypto defaults, TPM-backed FDE progressing toward general availability, and AppArmor permission prompts for snaps (secondary) — [OSTechNix](https://ostechnix.com/ubuntu-26-04-lts-resolute-raccoon-features-release-notes/). 26.10 brings Rust coreutils, post-quantum-ready package signing and expanded TPM FDE — [OMG! Ubuntu](https://www.omgubuntu.co.uk/2026/10/ubuntu-2610-new-features)
- GNOME 51 adds FIDO2 passkey and smartcard login in GDM. GNOME Software surfaces Flatpak permissions and end-of-life warnings — [OMG! Ubuntu](https://www.omgubuntu.co.uk/2026/09/gnome-51-released)

#### C6 Storage and filesystems
- Linux 7.0: generic filesystem error reporting to userspace, XFS self-healing, NULLFS — [OSTechNix](https://ostechnix.com/linux-kernel-7-0-released/); [AlternativeTo](https://alternativeto.net/news/2026/4/linux-7-0-released-with-xfs-self-healing-agent-agnostic-ai-keys-and-major-hardware-support/)
- Linux 7.3:
  - Btrfs direct I/O through an iomap bounce buffer, and fsync with no-holes about 5x faster.
  - ext4 faster parallel direct I/O and fallocate.
  - NTFS read support for Windows System Compression.
  - NFS directory-delegation callbacks.
  - Idmapped overlayfs mounts.
  - XFS FALLOC_FL_WRITE_ZEROES.
  - The freevxfs and EFS filesystems removed.
  — [KernelNewbies](https://kernelnewbies.org/Linux_7.3). The in-kernel ksmbd SMB server now works as an Apple Time Machine target — [OMG! Ubuntu](https://www.omgubuntu.co.uk/2026/10/linux-7-3-features)
- bcachefs was removed from mainline in 6.18 and is now available only through DKMS; it can still be the root filesystem with an initramfs that loads the module — [Linux Journal](https://www.linuxjournal.com/content/bcachefs-ousted-mainline-kernel-move-dkms-and-what-it-means)
- Plasma 6.8 integrates the Kup tool for scheduled backups to external or remote storage — [OMG! Ubuntu](https://omgubuntu.co.uk/2026/10/kde-plasma-6-8-released)

#### C7 Networking and connectivity
- Windows: Wi-Fi 7, including Multi-Link Operation across 2.4, 5 and 6 GHz, is available from Windows 11 24H2 with a capable chipset and driver. Windows 10 will not get Wi-Fi 7 — [Windows Central](https://www.windowscentral.com/software-apps/windows-11/microsoft-brings-wi-fi-7-support-to-windows-11-with-latest-preview-build-coming-to-everyone-later-this-year); [Windows Latest](https://windowslatest.com/2024/06/21/microsoft-doc-says-wi-fi-7-is-limited-to-windows-11-24h2-at-least-for-now). Enterprise Wi-Fi 7 needs 24H2 plus the Sep 2025 preview update or later, Wi-Fi 7 Enterprise access points and validated drivers — [Neowin](https://www.neowin.net/news/windows-11-unlocks-wi-fi-7-enterprise-support-boosting-speed-and-security/)
- Linux: Wi-Fi 7 MLO needs a mainline kernel of 6.16 or later, current iwlwifi firmware and wpa_supplicant 2.12. Community guide — [Excentis](https://support.excentis.com/knowledge/article/1005). Users report an Intel BE201 dropping MLO links on Fedora while the same network works on Windows 11 — [Fedora Discussion](https://discussion.fedoraproject.org/t/how-to-enable-wi-fi-7-multi-link-operation-mlo-on-linux-for-intel-be201/198428); [Level1Techs](https://forum.level1techs.com/t/how-to-enable-wi-fi-7-multi-link-operation-mlo-on-ubuntu-or-linux-for-intel-be201/253294)
- Bluetooth LE Audio and Auracast:
  - Linux implements them through BlueZ plus PipeWire and needs kernel 6.4 or later; WirePlumber enables LE Audio by default, and the work is expected to be "under control within 2026" — [Collabora](https://www.collabora.com/news-and-blog/blog/2025/11/24/implementing-bluetooth-le-audio-and-auracast-on-linux-systems).
  - Windows 11 offers LE Audio "audio sharing" to two devices only on selected models; open Auracast broadcast is not generally available — [Windows Central](https://www.windowscentral.com/microsoft/windows-11/windows-11-moves-toward-multi-device-bluetooth-streaming-but-your-pc-probably-isnt-invited); [Microsoft Tech Community](https://techcommunity.microsoft.com/discussions/windowsinsiderprogram/when-will-windows-11-natively-support-auracast-broadcasting/4510868).
  - macOS has no native support, per a forum post — [Algoriddim community](https://community.algoriddim.com/t/auracast-bluetooth-le-support-inside-the-app/42286)
- Ubuntu 26.04 makes hybrid post-quantum key exchange the default in OpenSSH 10.2 and OpenSSL (secondary) — [OSTechNix](https://ostechnix.com/ubuntu-26-04-lts-resolute-raccoon-features-release-notes/)
- macOS 27 reports faster networking and AirDrop — [Macworld](https://www.macworld.com/article/3139330/macos-27-golden-gate-has-arrived-heres-what-s-new-on-your-mac.html). KDE Plasma 6.8's RDP server prioritises latency on slow links — [KDE](https://kde.org/announcements/plasma/6/6.8.0/)

#### C8 Hardware support breadth and drivers
- The Linux tree reached about 43.9 million total lines in the 7.2 merge window: 33.65M lines of code, 5.03M of comments and 5.21M blank, across 108,158 files — [Phoronix](https://www.phoronix.com/news/Linux-7.2-43-Million-Lines). Linux 7.0 had 39.6M lines, and the AMD GPU driver alone exceeds 6 million — [Phoronix](https://www.phoronix.com/news/Linux-Kernel-Nearly-40M)
- Per-release hardware churn: 7.2 brought Intel and AMD driver improvements, USB4STREAM and AMDGPU HDMI 2.1 FRL — [Phoronix](https://www.phoronix.com/news/Linux-7.2-Released). 7.3 brought the Steam Controller, the MSI Claw gamepad and IBM Power12 — [OMG! Ubuntu](https://www.omgubuntu.co.uk/2026/10/linux-7-3-features)
- GNOME 51 removed legacy NVIDIA driver interfaces, so users of older cards may need Nouveau — [OMG! Ubuntu](https://www.omgubuntu.co.uk/2026/09/gnome-51-released)
- Linux on Snapdragon X lags (see C1) — [Phoronix](https://www.phoronix.com/review/snapdragon-x-elite-linux-eoy2025). Some Android apps ran poorly on Intel Googlebooks at launch — [Wikipedia](https://en.wikipedia.org/wiki/Googlebook)

#### C9 Power management and thermal
- MacBook Air 13 M5: Notebookcheck's Wi-Fi test at 150 nits gave 16 h 11 min (M4: 16 h 13 min); at maximum brightness it lasted 6 h 39 min — [Notebookcheck](https://www.notebookcheck.net/MacBook-Air-13-M5-Vs-M4-battery-life-New-MacBook-suffers-from-6-shorter-endurance-on-max-brightness.1245763.0.html). Tom's Guide's web-surfing test gave about 15.5 h for the Air M5 (figure from search summary; the page is Tom's Guide's MacBook Neo battery test) — [Tom's Guide](https://www.tomsguide.com/computing/macbooks/macbook-neo-battery-life-tested-we-cant-believe-the-results)
- Snapdragon X2 Elite: in early tests a Lenovo Yoga Slim 7x with a 70 Wh battery nearly reached 25 h of local video playback — [Notebookcheck](https://www.notebookcheck.net/Lenovo-Yoga-Slim-7x-with-Qualcomm-Snapdragon-X2-Elite-shows-impressive-battery-life-and-performance-in-early-tests.1263818.0.html). The Asus Zenbook A16 with X2 Elite Extreme had weaker battery life than rivals — [Tom's Hardware](https://www.tomshardware.com/laptops/ultrabooks-ultraportables/asus-zenbook-a16-snapdragon-x2-elite-review); [PCWorld](https://www.pcworld.com/article/3119396/qualcomms-snapdragon-x2-elite-cant-keep-up-in-our-streaming-test.html)
- Windows: slow wake from sleep and Bluetooth connection problems are long-standing complaints that Microsoft's 2026 plan only now addresses — [The Register](https://theregister.com/2026/03/23/windows_quality_commitment/)
- Linux: on identical Lenovo Legion Go S handhelds, SteamOS gave more than twice the battery life of Windows 11 (2025) — [Slashdot](https://tech.slashdot.org/story/25/07/14/2313239). There is no rigorous 2026 same-laptop Linux-versus-Windows battery test, and anecdotes conflict — [yomotherboard](https://yomotherboard.com/question/does-linux-usually-get-better-or-worse-battery-life-than-windows/)

#### C10 Graphics, windowing and desktop UX
- Apple: Metal 4 with MetalFX Frame Interpolation and Denoising — [Apple Newsroom](https://www.apple.com/newsroom/2025/06/macos-tahoe-26-makes-the-mac-more-capable-productive-and-intelligent-than-ever/). macOS 27 adds 5K at 120 Hz external display modes, Game Porting Toolkit 4 and Liquid Glass with adjustable transparency. Users report stuttering and black previews in Mission Control and Spaces — [Macworld](https://www.macworld.com/article/3139330/macos-27-golden-gate-has-arrived-heres-what-s-new-on-your-mac.html)
- Linux: Wayland-only GNOME 50 — [Phoronix](https://www.phoronix.com/news/GNOME-50-Alpha) — and Plasma 6.8 — [KDE](https://kde.org/announcements/plasma/6/6.8.0/). Plasma 6.8 adds HDR with ICC profiles, HLG and NVIDIA triple buffering — [KDE](https://kde.org/announcements/plasma/6/6.8.0/). GNOME 51 brings frame-clock improvements, per-monitor HDR persistence and GTK fractional-scale rendering — [OMG! Ubuntu](https://www.omgubuntu.co.uk/2026/09/gnome-51-released). Ubuntu 26.10 adds a mid-drag tiling widget — [OMG! Ubuntu](https://www.omgubuntu.co.uk/2026/10/ubuntu-2610-new-features)
- Mesa 26.0 (Feb 2026) shipped Vulkan 1.4 drivers with large RADV ray-tracing gains — [Phoronix](https://phoronix.com/news/mesa-26.0-released). Mesa 26.2 (Aug 2026) added NVK mesh shaders and exposes Vulkan 1.4 in KosmicKrisp, a Vulkan driver for Apple platforms — [Phoronix](https://phoronix.com/news/Mesa-26.2-Released). Mesa drivers had launch-day Vulkan 1.4 support in late 2024 — [Phoronix](https://www.phoronix.com/news/Vulkan-1.4-Mesa-Drivers)
- Windows: Microsoft is only now restoring top and side taskbar positions and promising lower-latency File Explorer search, navigation and context menus — [The Register](https://theregister.com/2026/03/23/windows_quality_commitment/)

#### C11 Applications and software ecosystem
- Flathub in 2025: 438.2M downloads and 727.4M app updates across 239 countries — [Flathub](https://flathub.org/year-in-review/2025). About 3,249 apps, 446 of them new in 2025 — [Der Standard](https://www.derstandard.at/story/3000000303619/flathub-der-linux-app-store-erfreut-sich-starken-wachstums). Linuxiac's "well over 2,800" figure conflicts — [Linuxiac](https://linuxiac.com/flathub-sees-over-435-million-downloads-in-2025/)
- Debian 13 "trixie" (9 Aug 2025): 69,830 packages, over 14,100 new and over 8,840 removed — [Debian](https://www.debian.org/news/2025/20250809). LWN gives a lower total (64,419) — [LWN](https://lwn.net/Articles/1033474)
- macOS: Intel-only apps will stop working in macOS 28, after Rosetta 2 ends with macOS 27 — [Macworld](https://www.macworld.com/article/3139330/macos-27-golden-gate-has-arrived-heres-what-s-new-on-your-mac.html)
- Steam platform shares (see section 3): Windows 93.70%, Linux 3.95%, macOS 2.35% in Oct 2026 — [Steam](https://store.steampowered.com/hwsurvey/Steam-Hardware-Software-Survey-Welcome-to-Steam)

#### C12 Developer experience and SDK
- WSL was open-sourced in May 2025 and can be built from source, with pull requests accepted — [Windows Developer Blog](https://blogs.windows.com/windowsdeveloper/2025/05/19/the-windows-subsystem-for-linux-is-now-open-source/). Apple's Containerization is built with Swift's Static Linux SDK and musl; its companion container CLI lives at github.com/apple/container — [InfoQ](https://www.infoq.com/news/2025/06/apple-container-linux)
- Apple exposes its on-device model to developers through the Foundation Models framework, and from macOS 27 other model providers can plug in through a LanguageModel protocol (secondary) — [Firebase blog](https://firebase.blog/posts/2026/06/apple-foundation-models-gemini)
- Windows exposes agent connectors (MCP servers) through an On-Device Registry — [Neowin](https://www.neowin.net/amp/microsoft-is-making-ai-agents-a-first-class-component-of-windows/). Copilot Studio added MCP-compliant tools to agent workflows, generally available on 15 Jul 2026 (secondary) — [HANDS ON tek](https://m365admin.handsontek.net/?p=23860)
- Apple's gaming SDK: Game Porting Toolkit 4 — [Macworld](https://www.macworld.com/article/3139330/macos-27-golden-gate-has-arrived-heres-what-s-new-on-your-mac.html)

#### C13 Reliability, testing and quality engineering
- Microsoft's own 2026 plan concedes declining Windows 11 quality. The Register lists Bluetooth problems, slow wake and intrusive widgets as old, unaddressed complaints — [The Register](https://theregister.com/2026/03/23/windows_quality_commitment/). A critic notes repeated pledges without an apology — [Implicator](https://www.implicator.ai/microsoft-pledges-windows-11-quality-push-cuts-copilot-entry-points-after-backlash/)
- Windows servicing exceptions in 2026: hotpatch-enabled PCs needed restarts in June (a standard update for CVE-2026-45585), September (components that cannot be hotpatched) and October (a baseline month). Reboot-free hotpatches are expected to resume in Nov 2026 — [Windows Report](https://windowsreport.com/windows-11-hotpatch-pcs-need-restarts-in-september-and-october/); [M365 Message Center MC1461574](https://mc.merill.net/message/MC1461574). Smart App Control was broken by KB5127648 — [BleepingComputer](https://bleepingcomputer.com/news/microsoft/kb5127648-windows-security-update-may-break-smart-app-control)
- Quick Machine Recovery, part of the Windows Resiliency Initiative, pushes targeted fixes to machines that cannot boot through Windows RE and Windows Update — [Windows Report](https://windowsreport.com/?p=1440436); [The Hacker News](https://thehackernews.com/2024/11/microsoft-launches-windows-resiliency.html)
- Linux: the 7.2 cycle saw heavy fix volume from AI-found bugs, with a late DRM scheduler revert after a regression — [Phoronix](https://www.phoronix.com/news/Linux-7.2-Released); [Let's Data Science](https://letsdatascience.com/news/torvalds-releases-linux-72-rc7-amid-ai-fixes-1d6ee190). Fedora 45 slipped twice on blocker bugs, showing its release gates working — [Linuxiac](https://linuxiac.com/?p=221866)
- macOS 27: automatic password changes and personalised Smart Reply were pulled before release, and parental controls showed incorrect device status — [Macworld](https://www.macworld.com/article/3139330/macos-27-golden-gate-has-arrived-heres-what-s-new-on-your-mac.html)

#### C14 Performance and efficiency
- PhoneBuff (Sep 2026, identical XPS 13s): Fedora won 6 of 11 timed tests, 4 tied and Windows won 1. A VS Code build-and-test run took 2:20.5 on Fedora versus 3:41.99 on Windows. Windows loaded Cyberpunk 2077 slightly faster. Boot, restart and RAM results are in C1 and C3 — [It's FOSS](https://itsfoss.com/news/fedora-beats-windows-11/); [XenoSpectrum](https://xenospectrum.com/en/linux-vs-windows-11-lab-benchmark/)
- macOS 27: faster app launches and Spotlight indexing — [Macworld](https://www.macworld.com/article/3139330/macos-27-golden-gate-has-arrived-heres-what-s-new-on-your-mac.html). Plasma 6.8 starts faster with less memory — [KDE](https://kde.org/announcements/plasma/6/6.8.0/)

#### C15 Accessibility and internationalization
- Windows:
  - The April 2026 update expanded Narrator's image descriptions — [Windows Latest](https://www.windowslatest.com/2026/04/14/windows-11-april-2026-update-tested-whats-new-improved-and-fixed/).
  - KB5083806 (build 28000.1896, 30 Apr 2026) brought rich Narrator image descriptions on Copilot+ PCs and better number handling in Voice Access — [devicebase](https://devicebase.net/en/microsoft-windows-11-26h1/updates/kb5083806-narrator-provides-rich-image-descriptions-on-copilot-pcs/9ap).
  - Live Captions run on-device and offline; they launched in US English and added more languages later (2022-era source) — [Windows Experience Blog](https://blogs.windows.com/windowsexperience/?p=177100)
- macOS: Tahoe added Magnifier for Mac, Accessibility Reader, Braille Access and Vehicle Motion Cues — [Apple Newsroom](https://www.apple.com/newsroom/2025/06/macos-tahoe-26-makes-the-mac-more-capable-productive-and-intelligent-than-ever/). macOS 27.2 lets FaceTime use a third-party sign-language interpreter or live captioner — [Macworld](https://www.macworld.com/article/3139330/macos-27-golden-gate-has-arrived-heres-what-s-new-on-your-mac.html)
- Linux:
  - GNOME 51 adds Reduced Motion and improved screen-reader behaviour — [OMG! Ubuntu](https://www.omgubuntu.co.uk/2026/09/gnome-51-released).
  - "Newton", the Wayland-native accessibility project, made Orca "basically usable" with some GTK 4 apps, but GNOME Shell still relied on AT-SPI, mouse review did not work and synthesising mouse events was unsupported. This source is 2024-era, and I found no 2026 update — [GNOME accessibility blog](https://blogs.gnome.org/a11y/); [OSNews](https://www.osnews.com/story/139996/update-on-newton-the-wayland-native-accessibility-project/). Orca's Feb 2026 man page still describes AT-SPI — [Arch man page](https://man.archlinux.org/man/orca.1).
  - Plasma 6.8 added only now a built-in dwell clicker, plus an Amharic on-screen keyboard and Bikram Sambat calendar support — [KDE](https://kde.org/announcements/plasma/6/6.8.0/); [OMG! Ubuntu](https://omgubuntu.co.uk/2026/10/kde-plasma-6-8-released).
  - Ubuntu 26.10 adds opt-in speech-to-text ("Myna") — [OMG! Ubuntu](https://www.omgubuntu.co.uk/2026/10/ubuntu-2610-new-features)

#### C16 Installation, updates, recovery and administration
- Windows:
  - Annual enablement-package feature updates, with automatic feature updates near end of support; 24H2 Home/Pro reaches end of support on 13 Oct 2026 — [Pureinfotech](https://pureinfotech.com/windows-11-26h2-released-force-install/).
  - 24/36-month support windows — [Thurrott](https://www.thurrott.com/?p=342194).
  - Hotpatching for Windows 11 Enterprise 24H2/25H2 applies security updates without a restart, on a quarterly cycle: a baseline month with a restart, then two hotpatch months — [Microsoft Support](https://support.microsoft.com/en-US/servicing/os/hotpatch/windows-11/2025/release-notes-for-hotpatch-on-windows-11-enterprise-version-24h2); [Microsoft Learn](https://learn.microsoft.com/windows/deployment/windows-autopatch/manage/windows-autopatch-hotpatch-updates).
  - A one-reboot-a-month option is planned — [The Register](https://theregister.com/2026/03/23/windows_quality_commitment/)
- Ubuntu: 5 years of LTS support, extended to 10 with Pro; interim releases get 9 months (26.10 until Jul 2027) — [Ubuntu release notes](https://documentation.ubuntu.com/release-notes/26.04/); [UbuntuHandbook](https://ubuntuhandbook.org/index.php/2026/10/ubuntu-26-10-released-with-kernel-7-3-gnome-51-amd64v3-image/)
- Atomic and image-based Linux:
  - KDE Linux (alpha Sep 2025) uses atomic A/B image updates with automatic Btrfs snapshots of the last 5 OS images. It is Wayland-only, with no package layering on the base — [OMG! Linux](https://www.omglinux.com/kde-linux-immutable-os/); [OMG! Ubuntu](https://omgubuntu.co.uk/2025/09/kde-linux-immutable-distro-alpha-released).
  - Fedora Atomic Desktops can be built with bootc container tooling — [Fedora Magazine](https://fedoramagazine.org/building-your-own-atomic-bootc-desktop/).
  - GNOME OS reportedly moved from OSTree to systemd-sysupdate (forum commentary) — [Techlore discussion](https://discuss.techlore.tech/t/both-kde-and-gnome-to-offer-official-distros/10955)
- Enterprise management of Linux: Microsoft Intune supports Ubuntu (26.04 LTS added in client 3.0.2, 27 Apr 2026) and RHEL 9/10 (RHEL 8 support ends Jul 2026), with an identity broker for Entra ID — [Microsoft Learn](https://learn.microsoft.com/en-ca/%20entra/identity/devices/whats-new-linux); [Microsoft Tech Community](https://techcommunity.microsoft.com/discussions/microsoft-intune/rhel10-or-ubuntu-26-04-lts-support/4496739)
- Windows Administrator protection is managed through Intune or Group Policy — [FoneArena](https://www.fonearena.com/blog/?p=493781)

#### C17 Community, governance, licensing and sustainability
- Linux 7.3: 18,765 non-merge commits from 3,021 developers, 807 of them first-time contributors, a record — [LWN](https://lwn.net/Articles/1101143/); [IT-Connect](https://www.it-connect.tech/linux-7-3-sets-a-new-kernel-commit-record-whats-new/). Linux 7.2: 16,418 commits from 2,652 developers, 613 first-timers — [braindetox](https://braindetox.kr/en/posts/linux_7_2_kernel_development_statistics.html)
- Governance examples: the Rust decision reached by consensus at the Maintainers Summit — [LWN](https://lwn.net/Articles/1047293); Torvalds removing bcachefs after a maintainer conflict — [Linux Journal](https://www.linuxjournal.com/content/bcachefs-ousted-mainline-kernel-move-dkms-and-what-it-means); KDE at 30 years — [KDE](https://kde.org/announcements/plasma/6/6.8.0/)
- Microsoft is redesigning the Insider Feedback Hub as part of its quality plan — [The Register](https://theregister.com/2026/03/23/windows_quality_commitment/)

### Inferences

#### Per-category baseline: table stakes, differentiators and the measurable bar
Each category below lists four things. **Table stakes** are what a newcomer needs to be taken seriously, roughly the 7-8 level on the shared scale. **Differentiators** are what puts the leaders at 9-10. **Terminal OS reference** compares the bar with the README features the coordinator supplied; this is not an audit. **Scores** give W = Windows 11, M = macOS 27, L = Linux desktop.

**C1 Boot, firmware and platform**
- Table stakes:
  - UEFI boot on real x86_64 hardware from NVMe or USB with GPT.
  - Boot with Secure Boot on. This needs a shim signed under the Microsoft UEFI CA 2023 now that the 2011 CA has expired, or a documented user key-enrolment path.
  - TPM 2.0 use for measured boot and disk-key sealing.
  - Full ACPI (battery, thermal, lid, buttons).
  - Dual-boot coexistence.
  - Cold boot to login in about 25 s and restart in about 30 s or less on an ordinary 8 GB laptop (PhoneBuff measured 23-24 s cold and 27-61 s restart).
- Differentiators:
  - ARM64 laptop support (Snapdragon X2, Apple silicon).
  - OS-delivered firmware updates (fwupd/LVFS, Windows Update) (general knowledge, not re-verified in this session).
  - Attestation.
  - x86-64-v3-optimised builds.
- Terminal OS reference: it has its own UEFI loader and uACPI on x86_64. The missing items are Secure Boot signing (or a key-enrolment story), TPM-sealed keys and ARM64.
- Scores:
  - W 10: TPM 2.0 and Secure Boot are mandatory, on x64 and ARM64.
  - M 9: the tightest chain of trust, but Apple hardware only.
  - L 9: the broadest architecture coverage, but Secure Boot depends on Microsoft-signed shims and the 2026 CA rotation, and ARM laptop support lags.

**C2 Kernel architecture, scheduling and SMP**
- Table stakes:
  - A preemptive kernel that scales across all cores with fine-grained locking (no global lock on hot paths).
  - Per-CPU run queues and priority/QoS classes.
  - Hybrid P/E-core awareness on Intel Alder Lake and later.
  - Tickless idle.
  - Low-latency scheduling for audio and the UI.
  - Linux retired its own Big Kernel Lock in 2.6.39 in 2011 (general knowledge, not re-verified in this session).
- Differentiators:
  - Pluggable BPF schedulers (sched_ext with cgroup sub-schedulers).
  - Cache-aware scheduling.
  - Real-time preemption (general knowledge).
  - User-visible CPU affinity.
- Terminal OS reference: "all cores under one big kernel lock" is the classic early-hobby-OS design. Removing the BKL from hot paths is the gating item for this category.
- Scores: W 9, M 9 (sophisticated QoS-driven schedulers; no 2026 specifics verified), L 10.

**C3 Memory management**
- Table stakes:
  - Demand paging with copy-on-write fork.
  - Unified page cache.
  - Swap plus compressed memory.
  - ASLR, NX and W^X.
  - Huge pages.
  - Per-process or per-cgroup limits.
  - Graceful OOM handling.
  - GPU memory management.
  - A base-desktop footprint well under about 3 GB: Fedora used about 31% of 8 GB after boot, and a clean Windows install idles at about 3.3 GB.
- Differentiators:
  - MGLRU, zswap, KSM and userfaultfd.
  - VRAM cgroups.
  - Hardware memory tagging (Apple MIE).
- Scores:
  - W 9: capable, but Microsoft admits the footprint is too heavy.
  - M 9.
  - L 10.

**C4 Processes, threads, IPC, signals and syscall/POSIX surface**
- Table stakes:
  - A mature libc with the near-complete POSIX surface: pthreads, signals, fork/exec/posix_spawn, mmap, poll/epoll-or-kqueue, pipes, UNIX-domain sockets, shared memory and futex-style sync.
  - Dynamic linking.
  - ptrace-class debugging.
  - Async I/O (io_uring, IOCP).
  - Isolation primitives (namespaces, jails or sandbox profiles).
  - A fast syscall entry path: x86_64 OSes use syscall/sysret rather than the legacy int 0x80 gate (general knowledge, not re-verified in this session).
- Differentiators:
  - io_uring, pidfd and seccomp/Landlock.
  - New sandbox syscalls (failfs/fchroot).
  - Mach/XPC.
  - Running another OS's binaries (WSL2, Apple Containerization).
- Terminal OS reference: fork/exec, threads, shared memory and int 0x80 syscalls, with an mlibc port in progress. Finishing mlibc is what unlocks porting software, since Ladybird needs a broad POSIX surface.
- Scores:
  - W 9: the native API is not POSIX, but WSL2 supplies a full Linux.
  - M 10: BSD/POSIX userland (general knowledge).
  - L 10.

**C5 Security and privacy**
- Table stakes:
  - Full-disk encryption on by default or one click away, with TPM or enclave key sealing (BitLocker, FileVault, LUKS2; general knowledge for the defaults).
  - Verified boot and signed OS updates.
  - Multi-user privilege separation with elevation prompts.
  - Exploit mitigations: ASLR, NX, stack protector and CFI.
  - An app sandbox and permission prompts for camera, microphone, screen and location.
  - Signed apps with a reputation check (Smart App Control; Gatekeeper and notarization are general knowledge).
  - A firewall.
  - TLS 1.3 with certificate validation.
  - Monthly or faster security patches.
  - Passkeys/FIDO2 (now even at the GNOME login screen).
  - Post-quantum hybrid key exchange becoming the default.
- Differentiators:
  - Virtualisation-based kernel protection (VBS/HVCI; general knowledge).
  - Hardware memory tagging (MIE).
  - Just-in-time admin (Administrator protection).
  - Memory-safe kernel code (Rust).
  - Security software moved out of the kernel.
  - Lockdown-style hardened modes (general knowledge).
- Terminal OS reference: it has ring-3 isolation and BearSSL TLS 1.2. The bar adds TLS 1.3, FDE, signed updates and binaries, an app sandbox and permission prompts.
- Scores:
  - W 9: a deep stack, but key protections ship off by default (Administrator protection) and 2026 updates broke Smart App Control.
  - M 10: the most integrated hardware-rooted model, although the M5 MIE bypass shows nothing is absolute.
  - L 8: strong primitives, but weaker defaults. FDE is not default in mainstream installers, Ubuntu's TPM-FDE is only reaching general availability, distro-packaged apps are unsandboxed (Flatpak sandboxing is opt-in by app source; general knowledge), and Secure Boot depends on Microsoft's CA.

**C6 Storage and filesystems**
- Table stakes:
  - A crash-consistent journaling or copy-on-write root filesystem (ext4, XFS, Btrfs, APFS, NTFS; general knowledge).
  - FAT32 and exFAT for removable media.
  - Access to the dominant foreign filesystem (NTFS, plus Time Machine targets over SMB).
  - TRIM.
  - POSIX permissions, ACLs and xattrs.
  - Change notification.
  - Snapshots or built-in backup.
  - SMB and NFS clients.
  - FDE integration.
  - Repair tools.
- Differentiators:
  - CoW snapshots and clones.
  - Self-healing metadata (XFS in 7.0).
  - Time Machine-class backup.
  - Cloud file providers (OneDrive, iCloud; general knowledge).
- Terminal OS reference: it has FAT32/VFAT and ext2/ext3 with journaling, roughly the 2001-era ext3 level. The bar is ext4 or exFAT, crash-safe writes, snapshots or backup, and encryption.
- Scores:
  - W 9: NTFS is mature, but the client is not snapshot-first (general knowledge).
  - M 10: APFS snapshots with Time Machine (general knowledge).
  - L 9: the best filesystem breadth and innovation, but snapshot and backup UX varies by distro (Kup only now integrated in Plasma), and bcachefs churn.

**C7 Networking and connectivity**
- Table stakes:
  - Dual-stack IPv4/IPv6 with SLAAC or DHCPv6.
  - Wi-Fi 6/6E with WPA3 across Intel, MediaTek, Qualcomm and Realtek chips (general knowledge).
  - Wi-Fi 7 on new laptops.
  - Bluetooth 5.x audio (A2DP/HFP), HID and LE.
  - TLS 1.3, HTTP/2 and HTTP/3 (QUIC) in the browser or network stack (general knowledge).
  - Encrypted DNS.
  - A built-in VPN (WireGuard, IPsec, OpenVPN).
  - A firewall, mDNS, captive-portal handling, an SMB client, and Ethernet, USB and phone tethering.
- Differentiators:
  - Wi-Fi 7 MLO, including enterprise 802.1X on Wi-Fi 7.
  - LE Audio and Auracast.
  - Post-quantum TLS and SSH.
  - Proximity ecosystems (AirDrop/Continuity; general knowledge).
  - Remote desktop servers.
- Terminal OS reference: it has TCP/IP, DHCP, DNS, HTTPS on TLS 1.2, CDC-ECM, and AX2xx Wi-Fi with WPA2 at legacy 802.11a/g rates. The bar is 802.11ax/be rates, WPA3, IPv6, TLS 1.3 and Bluetooth.
- Scores:
  - W 9: Wi-Fi 7 including enterprise; LE Audio limited.
  - M 9: no native LE Audio; strong ecosystem.
  - L 9: rough MLO edges but ahead on LE Audio; WireGuard in-kernel (general knowledge).

**C8 Hardware support breadth and drivers**
- Table stakes:
  - Accelerated graphics on Intel, AMD and NVIDIA GPUs.
  - Wi-Fi and Bluetooth for the major vendors.
  - HDA plus SOF-DSP audio on modern laptops (general knowledge).
  - I2C-HID precision touchpads with gestures.
  - UVC and MIPI webcams.
  - USB 3, USB4 and Thunderbolt with DisplayPort-alt-mode docks.
  - NVMe and SATA; card readers, fingerprint readers and sensors.
  - IPP Everywhere printing.
  - Gamepads.
  - Overall, "works on most laptops from the last ~5 years".
- Differentiators:
  - Day-one enablement of new CPUs (Nova Lake and Zen 6 landed in 7.0).
  - ARM laptops.
  - Certified-hardware programmes.
- Terminal OS reference: it covers NVMe, AHCI, ATA, xHCI with hubs, HID and CDC-ECM, I2C-HID, HDA, Intel Gen12 graphics and AX2xx Wi-Fi, mostly one vendor family (Intel). The bar is multi-vendor (AMD and NVIDIA GPUs, Realtek, MediaTek and Qualcomm Wi-Fi, Bluetooth, webcams).
- Scores:
  - W 10.
  - M 8: by design macOS runs only on Apple Macs, with no discrete or eGPU on Apple silicon (general knowledge), though peripheral class-driver support is excellent.
  - L 9: the largest in-tree driver base, but laptop gaps remain (Snapdragon X, legacy NVIDIA, Wi-Fi 7 quirks).

**C9 Power management and thermal**
- Table stakes:
  - Reliable suspend and resume (S0ix/Modern Standby or S3) with lid handling.
  - CPU P-states via HWP, CPPC or amd-pstate, plus C-states (general knowledge).
  - Runtime power management for GPU, PCIe and USB.
  - Panel self-refresh.
  - Accurate battery reporting, thermal throttling and power profiles.
  - Roughly 10 h or more of real-world use on x86 ultrabooks. This is inferred from reviews, which put Snapdragon X2 at about 10-12 h typical and the Apple M5 Air at 15-16 h.
- Differentiators:
  - Instant wake.
  - 16 h or more of web use (MacBook Air M5) or about 25 h of video (X2 Elite).
  - Low-power modes and charge limits.
- Terminal OS reference: the README lists HWP and MWAIT C6, which is a good start. The bar adds suspend/resume, device runtime power management and battery reporting.
- Scores:
  - W 8: long-standing sleep and wake complaints, and x86 battery life behind Apple; ARM is strong.
  - M 10.
  - L 7: results vary by laptop. A tuned SteamOS can double Windows battery life on a handheld, but Snapdragon X Linux lags and there is no rigorous evidence of parity on mainstream laptops.

**C10 Graphics, windowing and desktop UX**
- Table stakes:
  - A GPU-composited, tear-free display server (Wayland-class).
  - Multi-monitor hotplug with mixed DPI and fractional scaling.
  - HDR on capable panels, VRR and colour management.
  - A modern 3D API (Vulkan 1.4, DirectX 12 or Metal 4-class).
  - Hardware video decode (H.264, HEVC, AV1; general knowledge).
  - Clipboard and drag-and-drop, notifications and a search launcher.
  - Window snapping and tiling, virtual desktops and dark mode.
  - Screenshots and screen recording.
  - A remote desktop server.
  - One consistent toolkit.
  - Smooth 60-120 Hz animation.
- Differentiators:
  - Frame interpolation (MetalFX).
  - Game translation layers (GPTK 4; Proton, general knowledge).
  - 5K/120 Hz external displays.
  - A coherent design language.
  - Actions from search.
- Terminal OS reference: it has a compositing window server with GPU effects on Intel Gen12, a login window, and apps. The bar adds a Vulkan-class API, HDR, VRR, fractional scaling, multi-GPU and multi-monitor support, and video decode.
- Scores:
  - W 9: the DirectX gaming leader (general knowledge), but UX regressions (taskbar, Explorer latency, Copilot clutter).
  - M 10: Metal 4 and a coherent design, despite Mission Control glitches.
  - L 9: Wayland-only with HDR and ICC profiles, but legacy NVIDIA pain and GTK/Qt fragmentation.

**C11 Applications and software ecosystem**
- Table stakes:
  - A modern browser engine (Chromium, Gecko or WebKit-class) that can run Google Workspace, Microsoft 365 web, YouTube and streaming. Streaming needs DRM such as Widevine (general knowledge).
  - An office suite, email and calendar, a media player with codecs, a PDF viewer, a file manager, a terminal and an image editor.
  - An app store or package manager with signed packages and automatic updates.
  - Thousands of maintained apps.
  - At least one compatibility layer: Wine/Proton, a Linux personality (WSL), Android apps (Googlebook) or binary translation (Rosetta).
- Differentiators:
  - Exclusive professional apps.
  - Game catalogue depth (93.7% of Steam users are on Windows).
  - Compatibility layers.
- Terminal OS reference: the Ladybird port is the single highest-leverage ecosystem item, because a working modern browser substitutes for thousands of native apps.
- Scores:
  - W 10.
  - M 9: strong professional apps and weak gaming; the end of Rosetta will strand Intel-only apps.
  - L 8: about 3.2k Flathub apps, about 70k Debian packages and Proton gaming, but commonly cited gaps such as Adobe Creative Cloud, desktop Microsoft Office and anti-cheat multiplayer titles (general knowledge, not re-verified in this session).

**C12 Developer experience and SDK**
- Table stakes:
  - Self-hosting native toolchains (C/C++, Rust, Go, Python, JS).
  - Debugger, profiler, crash dumps and symbols.
  - A stable, documented ABI/API and SDK with a UI toolkit.
  - A packaging and distribution channel.
  - Containers and VMs.
  - A headless or CI mode.
  - VS Code-class editor support.
- Differentiators:
  - On-device model APIs (Foundation Models).
  - Agent APIs (MCP connectors).
  - Linux compatibility for developers (WSL, Containerization).
  - Game-porting kits.
- Scores:
  - W 9.
  - M 9.
  - L 10: the reference development environment. Evidence: both rivals now ship ways to run Linux for developers (WSL, Containerization). The weak spot is the fragmented desktop SDK.

**C13 Reliability, testing and quality engineering**
- Table stakes:
  - Automated unit, integration and fuzz testing.
  - Crash reporting with symbolication.
  - Staged rollouts through beta or insider rings (general knowledge).
  - Release-blocker criteria (Fedora 45 slipped twice on blockers).
  - Regression tracking.
  - A safe mode or recovery environment.
  - Driver fault containment or recovery.
- Differentiators:
  - Cloud-pushed remediation for unbootable machines (Quick Machine Recovery).
  - Hotpatching.
  - A/B rollback.
  - Fleet telemetry.
  - AI-assisted bug finding, which in 2026 is both a defensive tool and a source of large fix volumes.
- Scores:
  - W 8: by Microsoft's own admission quality slipped, and there were repeated 2026 update regressions (Smart App Control, hotpatch exceptions).
  - M 9: generally solid, but features were pulled and the UI has glitches.
  - L 9: transparent regression handling and strict release gates (syzbot and KernelCI fuzzing and CI are general knowledge), though quality varies by distro.

**C14 Performance and efficiency**
- Table stakes:
  - Cold boot to login in about 25 s and restart in about 30 s on an 8 GB laptop.
  - Base-desktop idle RAM well under 3 GB.
  - No dropped frames at 60 Hz or more.
  - Sub-second launch of core apps.
  - Fast file search.
  - Low idle power.
- Differentiators: Apple silicon performance per watt, and Linux's lower overhead on identical hardware.
- Scores:
  - W 8: 85% of 8 GB used after boot and 61 s restarts in the PhoneBuff test, with File Explorer latency admitted.
  - M 10.
  - L 9.

**C15 Accessibility and internationalization**
- Table stakes:
  - A screen reader that covers the login screen, shell and core apps, plus an accessibility API that third-party toolkits implement.
  - A magnifier; high contrast, large text and reduced motion.
  - Full keyboard navigation; sticky, slow and bounce keys; an on-screen keyboard.
  - Captions; voice control or dictation; braille display support.
  - Unicode text with complex-script shaping and bidi (HarfBuzz-class; general knowledge).
  - Input methods for CJK and Indic scripts.
  - Locale formats and calendars (Plasma added Bikram Sambat in 2026).
  - Translated UIs.
- Differentiators:
  - AI image descriptions (Narrator on Copilot+ PCs).
  - On-device live captions and translation.
  - Voice Access.
  - An Accessibility Reader.
  - Sign-language interpreter integration in calls.
- Terminal OS reference: no accessibility or IME features are listed in the README summary supplied. This is likely the category with the largest gap to table stakes.
- Scores:
  - W 9.
  - M 10: VoiceOver is widely regarded as the reference (general knowledge), and Tahoe added breadth.
  - L 7: Orca and AT-SPI work, but the Wayland-era plumbing is incomplete (Newton; GNOME Shell mouse review), and basics such as a built-in dwell click arrived only in 2026. Internationalization is strong, so the score is pulled down by accessibility.

**C16 Installation, updates, recovery and administration**
- Table stakes:
  - A graphical installer with partitioning, FDE and dual-boot.
  - Signed online updates installed automatically, with a published support lifecycle.
  - Rollback, a recovery mode and reinstall.
  - Firmware updates; backup and restore; user and admin accounts.
  - MDM enrolment and policy for organisations (Intune or Apple Business Manager-class).
- Differentiators:
  - Atomic A/B images with automatic rollback (KDE Linux, Fedora Atomic/bootc; SteamOS, general knowledge).
  - Hotpatching and cloud recovery.
  - Zero-touch provisioning (Autopilot/ABM; general knowledge).
  - 10-year support (Ubuntu Pro).
- Scores:
  - W 9: the deepest enterprise tooling and hotpatch, but forced reboots and update regressions.
  - M 9: signed, snapshot-based updates (general knowledge), with MDM mostly through third parties.
  - L 8: atomic designs exist but are not the default on Ubuntu or Fedora Workstation, Intune covers only Ubuntu and RHEL, and release upgrades remain package-based.

**C17 Community, governance, licensing and sustainability**
- Table stakes:
  - A clear licence.
  - A public repository with a contribution guide and code of conduct.
  - A security disclosure and CVE process.
  - A predictable release schedule and roadmap.
  - More than one maintainer (bus factor).
  - Funding.
- Differentiators:
  - Multi-vendor foundations (Linux Foundation, GNOME Foundation, KDE e.V.; general knowledge).
  - Thousands of contributors per release.
- Terminal OS reference: a single main developer is the defining risk for this category.
- Scores: W 9, M 9 (closed governance but well funded), L 10.

#### Scores (0-10, shared scale; author's judgement from the findings above)

| Category | Windows 11 (26H2) | macOS 27 | Linux desktop (Fedora 45 / Ubuntu 26.04, GNOME/Plasma) | Best-in-class reference |
|---|---|---|---|---|
| C1 Boot, firmware, platform | 10 | 9 | 9 | Windows (breadth + mandatory TPM/Secure Boot) |
| C2 Kernel, scheduling, SMP | 9 | 9 | 10 | Linux (sched_ext, cache-aware scheduling) |
| C3 Memory management | 9 | 9 | 10 | Linux (MGLRU, zswap, KSM, dmem) |
| C4 Processes, IPC, POSIX | 9 | 10 | 10 | Linux / macOS |
| C5 Security and privacy | 9 | 10 | 8 | macOS (hardware-rooted, MIE) |
| C6 Storage and filesystems | 9 | 10 | 9 | macOS (APFS + Time Machine) / Linux breadth |
| C7 Networking | 9 | 9 | 9 | Mixed (Windows Wi-Fi 7; Linux LE Audio) |
| C8 Hardware breadth | 10 | 8 | 9 | Windows |
| C9 Power and thermal | 8 | 10 | 7 | macOS (15-16 h web browsing on Air M5) |
| C10 Graphics and desktop UX | 9 | 10 | 9 | macOS (polish) / Windows (gaming) |
| C11 Apps and ecosystem | 10 | 9 | 8 | Windows |
| C12 Developer experience | 9 | 9 | 10 | Linux |
| C13 Reliability and QA | 8 | 9 | 9 | macOS / Linux |
| C14 Performance and efficiency | 8 | 10 | 9 | macOS (perf/W) / Linux (overhead) |
| C15 Accessibility and i18n | 9 | 10 | 7 | macOS |
| C16 Install, updates, admin | 9 | 9 | 8 | Windows (enterprise) / macOS |
| C17 Community and sustainability | 9 | 9 | 10 | Linux |
| Unweighted mean | 9.0 | 9.35 | 8.9 | |

Justification of every score below 9 (summarised from the category notes):
- Windows C9 (8): Microsoft itself lists slow wake from sleep and Bluetooth problems as unresolved, and x86 Windows battery life trails Apple silicon.
- Windows C13 (8): the March 2026 quality memo concedes the decline, and 2026 updates broke Smart App Control and repeatedly suspended reboot-free hotpatching.
- Windows C14 (8): 85% of 8 GB used after boot versus 31% on Fedora, and 61 s versus 27 s restarts on identical hardware; Microsoft is working to cut the baseline footprint (K2).
- macOS C8 (8): it runs only on Apple's own hardware, a deliberate narrowness.
- Linux C5 (8): weaker defaults (FDE and app sandboxing are not universal, and Secure Boot depends on Microsoft's CA).
- Linux C9 (7): battery life and suspend quality depend on the laptop model, and ARM laptops lag.
- Linux C11 (8): major proprietary apps and anti-cheat games are missing (general knowledge).
- Linux C15 (7): Wayland accessibility plumbing is still incomplete.
- Linux C16 (8): atomic updates and enterprise management are not mainstream defaults.

Reading the scale for Terminal OS: "taken seriously" means reaching about 7 in the categories users notice first (C1, C8, C10, C11, C16). "Toe to toe" means 9 or more, which no small team has reached across the board. SerenityOS, Haiku and Redox define the 5-6 band.

### Gaps
- Not re-verified this session; treat as general knowledge:
  - APFS, NTFS and ReFS specifics.
  - Whether VBS/HVCI and BitLocker or Device Encryption are on by default on new Windows 11 PCs.
  - macOS Lockdown Mode, SIP and SSV details.
  - DirectX 12 Ultimate features.
  - OS-level QUIC/HTTP/3 and built-in WireGuard status.
  - PREEMPT_RT mainline status.
  - KVM and Hyper-V details.
  - Windows and macOS scheduler internals.
  - Unicode and emoji versions supported.
  - Which Macs ship Wi-Fi 7.
- No authoritative 2026 status was found for:
  - Microsoft's "antivirus out of the kernel" platform (beyond its July 2025 private preview).
  - The Newton Wayland accessibility stack (latest found is 2024-era).
  - KDE Linux and GNOME OS (latest found is the Sep 2025 alpha).
- No rigorous same-hardware battery comparison of Linux and Windows in 2026 was found, and no macOS boot-time or idle-RAM measurement.
- I found no source for Apple Platform SSO or Entra integration on macOS, or for current Apple Business Manager and declarative device management features.

## 3. Quantitative metrics (market share, Steam survey, boot time and RAM, minimum hardware, repository sizes, Linux kernel scale, release cadence)

### Takeaway
Worldwide desktop share in Oct 2026 (StatCounter): Windows about 76.7%, macOS about 15.7% (OS X plus macOS rows), Linux about 5.6%, ChromeOS about 2.0%. On Steam (Oct 2026), Windows has 93.70% (Windows 11 72.51%), Linux 3.95% (a record of about 5.3% in Mar 2026) and macOS 2.35%; the modal gaming PC has 16 GB RAM, 8 CPU threads, 8 GB VRAM and a 1080p screen. On an 8 GB laptop, cold boot takes about 23-24 s and restart 27-61 s; Windows uses most of 8 GB after boot. Minimum RAM is 4 GB for Windows 11 and 6 GB for the Ubuntu 26.04 desktop. Flathub has about 3.2k apps and Debian about 70k packages. The Linux kernel is about 43.9M lines, takes about 16-19k commits from about 2,600-3,000 developers per roughly nine-week cycle, and ships stable releases about every nine weeks.

### Cited Findings

#### Desktop OS market share (StatCounter, page-view based)
- Worldwide desktop, Oct 2026: Windows 76.65%, OS X 9.86%, macOS 5.81% (so Apple totals about 15.67%), Linux 5.63%, Chrome OS 2.04%, FreeBSD 0% — [StatCounter](https://gs.statcounter.com/os-market-share/desktop/worldwide)
- Windows versions (desktop, Aug 2026, secondary report): Windows 11 68.6%, Windows 10 30.14%, Windows 7 0.96% — [ifeng](https://tech.ifeng.com/c/8w38YHYmFlc)
- StatCounter monthly readings are volatile. Search snippets for Jul and Aug 2026 showed Windows at 62-71% and Linux at 7.5-8.9% on other StatCounter views, and a Jun 2026 worldwide Windows reading of 56.55% was described as a reporting error. These were not verified by fetch — [StatCounter](https://gs.statcounter.com/os-market-share/desktop-operating-systems/worldwide)

#### Steam Hardware & Software Survey (gaming PCs)
- Oct 2026:
  - OS: Windows 11 64-bit 72.51% (+1.17), Windows 10 64-bit 21.11% (-2.50), Linux 3.95% (+0.90), macOS 2.35% (+0.43).
  - Most common hardware: 16 GB RAM (41.42%), 8 GB VRAM (26.92%), 1920x1080 (50.23%), 8 CPUs (28.68%).
  - Top GPUs: RTX 3060 3.62%, RTX 4060 Laptop 3.40%, RTX 4060 3.32%.
  — [Steam](https://store.steampowered.com/hwsurvey/Steam-Hardware-Software-Survey-Welcome-to-Steam)
- Windows total 93.70% (-1.33) — [GamingOnLinux](https://www.gamingonlinux.com/2026/11/steam-survey-october-2026-shows-an-uptick-for-the-linux-user-share/). Valve revised September's Linux share to 3.90%, so the real month-on-month change is about +0.05 points — [Digital Citizen](https://www.digitalcitizen.life/linux-gains-steam-market-share-as-october-2026-hardware-survey-shows-major-changes/)
- Linux distro mix, Oct 2026: CachyOS 16.39%, SteamOS Holo 13.89%, Arch 8.92%, Linux Mint 22.3 8.41%, SteamOS 8.09%, Bazzite 6.74%, Freedesktop SDK 26.08 runtime 4.00%, Ubuntu 26.04.1 3.63%, Ubuntu Core 24 2.97%, Fedora 44 KDE 2.82% — [GamingOnLinux](https://www.gamingonlinux.com/2026/11/steam-survey-october-2026-shows-an-uptick-for-the-linux-user-share/)
- Linux passed 5% on Steam in Mar 2026, more than double macOS — [Phoronix](https://phoronix.com/news/Steam-On-Linux-Tops-5p). A record of 5.33% was credited to SteamOS — [KitGuru](https://www.kitguru.net/page/40/!https:/www.kitguru.net/gaming/joao-silva/steamos-propels-linux-to-record-5-33-share-in-latest-steam-survey/)
- GPU vendor shares were not on the summary page I fetched (see Gaps).

#### PC shipments
- IDC's preliminary Q3 2026 figure: 62.7M traditional PCs, down 20.1% year on year and 9.1% on Q2. IDC blamed inventory pulled forward ahead of memory-driven price rises, supply constraints and high prices (secondary report of IDC's 8 Oct release) — [Neoteo](https://www.neoteo.com/en/idc-estimates-global-pc-shipments-fell-201-in-q3-2026)
- For comparison, Q3 2025 was 75.8M (+9.4%), driven by the Windows 10 end-of-support refresh — [IDC](https://www.idc.com/resource-center/press-releases/pc-shipments-accelerate-in-q3-signaling-steady-refresh-of-existing-installed-base/)

#### Boot time and idle RAM
- PhoneBuff (Sep 2026) on two identical Dell XPS 13s (Core 5 320, 8 GB, 512 GB):
  - Cold boot: 23 s on Windows 11 versus 24 s on Fedora.
  - Median restart: 61 s versus 27 s.
  - RAM after boot: 85.2% on Windows (with Dell utilities, Defender and WebView2) versus about 31% on Fedora.
  - Overall: Fedora won 6 of 11 tests, 4 tied and Windows won 1.
  — [It's FOSS](https://itsfoss.com/news/fedora-beats-windows-11/); [Comss.ru](https://www.comss.ru/page.php?id=21870); [XenoSpectrum](https://xenospectrum.com/en/linux-vs-windows-11-lab-benchmark/)
- A fresh Windows 11 25H2 idles at about 3.3 GB (secondary, citing Microsoft-related reporting). Previews of 26H2 reportedly showed 1.5-3 GB lower idle use, case by case — [unwire.hk](https://unwire.hk/2026/03/24/windows11-update-ram/fun-tech/); [Pureinfotech](https://pureinfotech.com/windows-11-8gb-memory-optimization/)

#### Minimum hardware requirements
- Windows 11: a 64-bit CPU with POPCNT, 4 GB RAM, 64 GB storage, UEFI with Secure Boot, and TPM 2.0 — [Pureinfotech](https://pureinfotech.com/windows-11-26h2-released-force-install/)
- Ubuntu 26.04 LTS desktop: a 2 GHz dual-core CPU, 6 GB RAM and 25 GB of storage. Server needs 1.5 GB RAM and 4 GB of storage — [Ubuntu release notes](https://documentation.ubuntu.com/release-notes/26.04/)
- macOS 27: Apple silicon Macs from 2020 on, plus the MacBook Neo (A18 Pro). Full Siri AI needs an M3 or later Mac with at least 12 GB unified memory — [Macworld](https://www.macworld.com/article/3139330/macos-27-golden-gate-has-arrived-heres-what-s-new-on-your-mac.html)
- Googlebook OS: an Arm64 or x64 SoC with UEFI and NVMe — [Wikipedia](https://en.wikipedia.org/wiki/Googlebook)
- Copilot+ PC AI features: an NPU of 40 TOPS or more. Many 2026 devices have 45-55 TOPS, and some ARM chips reach 80 TOPS — [Wccftech](https://wccftech.com/intel-outlines-40-tops-npu-performance-minimum-requirement-windows-copilot-ai-pc/); [PC-WELT](https://www.pcwelt.de/article/2819676/copilot-pc-lohnt-sich-der-kauf-eines-ki-rechners-das-muessen-sie-wissen.html)

#### App store and package repository sizes
- Flathub: about 3,249 apps at the end of 2025 (446 added that year) — [Der Standard](https://www.derstandard.at/story/3000000303619/flathub-der-linux-app-store-erfreut-sich-starken-wachstums). 438.2M downloads and 727.4M updates in 2025 — [Flathub](https://flathub.org/year-in-review/2025)
- Debian 13: 69,830 packages — [Debian](https://www.debian.org/news/2025/20250809)

#### Linux kernel scale
- Size: about 43.9M total lines (33,653,681 code, 5,033,878 comment, 5,211,184 blank, in 108,158 files) during the 7.2 merge window in Jun 2026 — [Phoronix](https://www.phoronix.com/news/Linux-7.2-43-Million-Lines). Linux 7.0 had 39,621,378 lines, and the AMDGPU driver exceeds 6M lines — [Phoronix](https://www.phoronix.com/news/Linux-Kernel-Nearly-40M)
- Contributors per release: 7.3 had 18,765 non-merge commits from 3,021 developers (807 first-timers) — [LWN](https://lwn.net/Articles/1101143/). 7.2 had 16,418 commits from 2,652 developers (613 first-timers) — [braindetox](https://braindetox.kr/en/posts/linux_7_2_kernel_development_statistics.html)

#### Release cadence
- Linux: 7.0 on 12 Apr, 7.2 on 16 Aug and 7.3 on 18 Oct 2026, with each next merge window opening immediately. 6.18 is the LTS, supported to Dec 2028 — [Slashdot](https://linux.slashdot.org/story/26/04/13/1857240/linux-70-released); [Phoronix](https://www.phoronix.com/news/Linux-7.2-Released); [Gigazine](https://www.gigazine.net/gsc_news/en/20261020-linux-kernel-7-3/)
- GNOME: two releases a year (50 in March, 51 on 16 Sep 2026, 52 scheduled for 17 Mar 2027) — [OMG! Ubuntu](https://www.omgubuntu.co.uk/2026/09/gnome-51-released); [UbuntuHandbook](https://ubuntuhandbook.org/index.php/2026/09/gnome-51-released-to-be-default-in-ubuntu-26-10-fedora-45/)
- KDE Plasma: 6.8 on 14 Oct 2026 — [KDE](https://kde.org/announcements/plasma/6/6.8.0/)
- Ubuntu: April and October releases. The LTS gets 5 years (10 with Pro) and interim releases 9 months — [Ubuntu release notes](https://documentation.ubuntu.com/release-notes/26.04/); [UbuntuHandbook](https://ubuntuhandbook.org/index.php/2026/10/ubuntu-26-10-released-with-kernel-7-3-gnome-51-amd64v3-image/)
- Fedora: roughly six-monthly releases (45 on 3 Nov 2026 after slips; 43 reaches end of life in Dec 2026) — [GamingOnLinux](https://www.gamingonlinux.com/2026/11/fedora-45-arrives-to-continue-advancing-the-linux-desktop/)
- Windows: an annual H2 feature update (26H2 on 29 Sep 2026) with 24 months of support for Home/Pro and 36 for Enterprise/Education, plus monthly security updates and a quarterly hotpatch baseline cycle for enterprise — [Pureinfotech](https://pureinfotech.com/windows-11-26h2-released-force-install/); [Thurrott](https://www.thurrott.com/?p=342194); [Microsoft Learn](https://learn.microsoft.com/windows/deployment/windows-autopatch/manage/windows-autopatch-hotpatch-updates)
- macOS: an annual September major release (27 on 14 Sep 2026) and roughly monthly point releases (27.2 on 26 Oct 2026) — [Macworld](https://www.macworld.com/article/3139330/macos-27-golden-gate-has-arrived-heres-what-s-new-on-your-mac.html)

### Inferences
- Scale contrast for Terminal OS (146 commits since 2 Oct 2025, essentially one developer): the 7.3 kernel cycle ran 63 days (16 Aug to 18 Oct) with 18,765 commits, about 300 commits a day. The kernel alone merges about twice Terminal OS's whole history every day, from about 3,000 people. Parity across all of C1-C17 is not attainable. The realistic goal is the 7-8 band in a few chosen categories, on a narrow hardware target.
- The hardware a newcomer must run well on today: an x86_64 laptop or desktop with 8-16 GB RAM, an NVMe SSD, UEFI with Secure Boot and TPM 2.0, a 1080p-to-4K display (often HiDPI), and an Intel, AMD or NVIDIA GPU. NVIDIA dominates gaming PCs: the top three Steam GPUs are all GeForce. Note that 8 GB machines are where Windows 11 struggles most.
- Market headroom: Linux holds about 5.6% of desktop page views and about 4% of Steam users after decades. Even a hypothetically excellent newcomer should expect hobbyist or niche adoption, not general-market share.
- Minimum RAM on mainstream desktops is rising (Ubuntu desktop is now 6 GB, and full Siri AI needs 12 GB) just as memory prices are pushing PC prices up. Lean resource use is therefore a credible differentiator rather than a nostalgia point.

### Gaps
- GPU vendor shares (NVIDIA, AMD, Intel) and CPU vendor shares for the Oct 2026 Steam survey were not available on the summary page.
- StatCounter's month-by-month 2026 desktop trend (the JavaScript chart) was not retrievable. The Windows 11/10 split for Oct 2026 was not found.
- No authoritative 2026 Gartner figure was found. The IDC Q3 2026 number comes from a secondary report.
- macOS boot time and idle RAM, GNOME and Plasma idle-RAM measurements on a clean install, and Microsoft Store, Mac App Store, Snap Store, Homebrew and winget catalogue sizes were not found.
- The Linux kernel's driver count was not found; only line counts and the AMDGPU size are cited.
- Linux 7.1's exact date, and hence a precise 2026 cycle-length average, was not verified.

## 4. What reviewers and analysts see as each OS's main strengths and weaknesses in 2026, and which weaknesses a newcomer could exploit as a niche

### Takeaway
- **Windows 11** is unmatched on apps, games, hardware breadth and enterprise management. In 2026 it is widely criticised, by Microsoft itself among others, for declining quality, Copilot clutter, heavy RAM use, slow restarts and sleep/wake, intrusive features, buggy updates and privacy worries about agentic AI.
- **macOS** leads on performance per watt, battery life, security integration, accessibility and polish. Its weaknesses are hardware lock-in, Apple's abandonment of Intel (Rosetta ends after macOS 27), a beta-quality AI rollout with features pulled at launch, UI glitches, a weak gaming share and EU feature restrictions.
- **The Linux desktop** wins on cost, efficiency, openness, developer workflows and fast-improving gaming. It still trails on market share, laptop-specific hardware (ARM, some Wi-Fi 7 and NVIDIA cases), battery consistency, Wayland-era accessibility, missing proprietary apps and fragmentation.
- **ChromeOS** is mid-transition to an Android-based OS, with app-compatibility rough edges.

The niches these leave open: lean OSes for 8 GB and older PCs, AI-free and privacy-first computing, an "Apple-like" coherent system on a narrow hardware target, understandable OSes for education, and appliance or kiosk OSes.

### Cited Findings

#### Windows 11
- Strengths:
  - Dominant share: 76.65% of worldwide desktop page views — [StatCounter](https://gs.statcounter.com/os-market-share/desktop/worldwide) — and 93.70% of Steam users — [GamingOnLinux](https://www.gamingonlinux.com/2026/11/steam-survey-october-2026-shows-an-uptick-for-the-linux-user-share/).
  - Wi-Fi 7 including enterprise — [Neowin](https://www.neowin.net/news/windows-11-unlocks-wi-fi-7-enterprise-support-boosting-speed-and-security/).
  - Reboot-free enterprise hotpatching — [Microsoft Support](https://support.microsoft.com/en-US/servicing/os/hotpatch/windows-11/2025/release-notes-for-hotpatch-on-windows-11-enterprise-version-24h2).
  - AI accessibility (Narrator image descriptions on Copilot+ PCs) — [devicebase](https://devicebase.net/en/microsoft-windows-11-26h1/updates/kb5083806-narrator-provides-rich-image-descriptions-on-copilot-pcs/9ap).
  - A new ARM64 platform (26H1/Bromine) — [Windows Central](https://www.windowscentral.com/microsoft/windows-11/windows-11-version-26h1-faq)
- Weaknesses acknowledged by Microsoft (Mar 2026): unnecessary Copilot entry points; File Explorer latency; a baseline memory footprint too high; the missing top/side taskbar; too many update reboots; inconsistent performance under load. The Register adds Bluetooth connection problems, slow wake from sleep and intrusive widgets as very old complaints, and calls the RAM work "a commercial necessity" given memory prices — [The Register](https://theregister.com/2026/03/23/windows_quality_commitment/)
- Critics say repeated pledges have not been matched by delivery — [Implicator](https://www.implicator.ai/microsoft-pledges-windows-11-quality-push-cuts-copilot-entry-points-after-backlash/)
- Measured overhead: on identical 8 GB hardware, RAM after boot was 85.2% on Windows versus 31% on Fedora, and median restart was 61 s versus 27 s — [It's FOSS](https://itsfoss.com/news/fedora-beats-windows-11/)
- Update regressions: the Oct 2026 KB5127648 broke the Smart App Control toggle — [BleepingComputer](https://bleepingcomputer.com/news/microsoft/kb5127648-windows-security-update-may-break-smart-app-control). Reboot-free hotpatching was suspended for Jun, Sep and Oct 2026 — [Windows Report](https://windowsreport.com/windows-11-hotpatch-pcs-need-restarts-in-september-and-october/)
- Agentic AI raises file-access privacy concerns — [PC Gamer](https://www.pcgamer.com/software/windows/apparently-windows-11-becoming-agentic-ai-means-letting-the-bots-rummage-through-some-of-your-files)
- Upgrade friction: 21.11% of Steam users are still on Windows 10 — [Steam](https://store.steampowered.com/hwsurvey/Steam-Hardware-Software-Survey-Welcome-to-Steam) — and about 30% of Windows desktops on StatCounter (Aug 2026) — [ifeng](https://tech.ifeng.com/c/8w38YHYmFlc). Microsoft extended free consumer ESU to Oct 2027 — [Help Net Security](https://www.helpnetsecurity.com/2026/06/26/microsoft-windows-10-free-security-updates-esu-program/)

#### macOS
- Strengths:
  - Battery life: the MacBook Air M5 ran about 16 h in Notebookcheck's Wi-Fi test — [Notebookcheck](https://www.notebookcheck.net/MacBook-Air-13-M5-Vs-M4-battery-life-New-MacBook-suffers-from-6-shorter-endurance-on-max-brightness.1245763.0.html).
  - Hardware memory safety (MIE) — [Computerworld](https://www.computerworld.com/article/4124435/apple-touts-unparalleled-protection-for-m5-macs.html).
  - Breadth of accessibility features — [Apple Newsroom](https://www.apple.com/newsroom/2025/06/macos-tahoe-26-makes-the-mac-more-capable-productive-and-intelligent-than-ever/).
  - In macOS 27: faster app launches, Game Porting Toolkit 4 frame-rate gains and 5K/120 Hz external displays — [Macworld](https://www.macworld.com/article/3139330/macos-27-golden-gate-has-arrived-heres-what-s-new-on-your-mac.html)
- Weaknesses (all reported by Macworld on macOS 27):
  - Mission Control and Spaces stutter, with black previews.
  - Siri AI shipped as a beta with "rough edges" and is unavailable on iPhone and iPad in the EU and mainland China.
  - Automatic password changes and personalised Smart Reply were removed before launch.
  - Parental-control delays and incorrect status.
  - DVD playback is on its way out.
  - Intel Macs were dropped after Tahoe, and Intel-only apps break in macOS 28.
  — [Macworld](https://www.macworld.com/article/3139330/macos-27-golden-gate-has-arrived-heres-what-s-new-on-your-mac.html)
- Even MIE was bypassed: a working M5 kernel privilege escalation was built in five days with AI assistance — [9to5Mac](https://9to5mac.com/2026/05/14/calif-team-details-how-anthropic-mythos-helped-build-a-working-macos-exploit-in-five-days/)
- Gaming share is small: macOS has 2.35% of Steam users — [Steam](https://store.steampowered.com/hwsurvey/Steam-Hardware-Software-Survey-Welcome-to-Steam)

#### Linux desktop
- Strengths:
  - On identical hardware (PhoneBuff), Fedora won or tied 10 of 11 timed tests against Windows 11, with much lower RAM use and faster restarts — [It's FOSS](https://itsfoss.com/news/fedora-beats-windows-11/).
  - A record kernel contributor base — [LWN](https://lwn.net/Articles/1101143/).
  - Leading LE Audio support — [Collabora](https://www.collabora.com/news-and-blog/blog/2025/11/24/implementing-bluetooth-le-audio-and-auracast-on-linux-systems).
  - Up to 10-year LTS support — [Ubuntu release notes](https://documentation.ubuntu.com/release-notes/26.04/).
  - Gaming growth, with Steam share reaching a 5%+ record in Mar 2026 — [Phoronix](https://phoronix.com/news/Steam-On-Linux-Tops-5p).
  - Modern HDR and colour management in Plasma 6.8 — [KDE](https://kde.org/announcements/plasma/6/6.8.0/)
- Weaknesses:
  - Small share: 5.63% of desktop page views — [StatCounter](https://gs.statcounter.com/os-market-share/desktop/worldwide).
  - ARM laptop support is weak (Snapdragon X Elite "disappointing") — [Phoronix](https://www.phoronix.com/review/snapdragon-x-elite-linux-eoy2025).
  - Wi-Fi 7 MLO has instability reports — [Fedora Discussion](https://discussion.fedoraproject.org/t/how-to-enable-wi-fi-7-multi-link-operation-mlo-on-linux-for-intel-be201/198428).
  - Legacy NVIDIA support was dropped in GNOME 51 — [OMG! Ubuntu](https://www.omgubuntu.co.uk/2026/09/gnome-51-released).
  - Wayland accessibility is incomplete — [GNOME accessibility blog](https://blogs.gnome.org/a11y/).
  - Secure Boot depends on Microsoft's CA through the 2026 rotation — [Red Hat](https://www.redhat.com/es/blog/expiration-secure-boot-signing-certificates-2026).
  - Battery results are inconsistent across machines — [yomotherboard](https://yomotherboard.com/question/does-linux-usually-get-better-or-worse-battery-life-than-windows/).
  - Fragmentation, for example Kubuntu 26.10 not shipping Plasma 6.8 — [OMG! Ubuntu](https://omgubuntu.co.uk/2026/10/kde-plasma-6-8-released) — and bcachefs being ejected from mainline — [Linux Journal](https://www.linuxjournal.com/content/bcachefs-ousted-mainline-kernel-move-dkms-and-what-it-means).
  - Enterprise management covers only Ubuntu and RHEL in Intune — [Microsoft Tech Community](https://techcommunity.microsoft.com/discussions/microsoft-intune/rhel10-or-ubuntu-26-04-lts-support/4496739)

#### ChromeOS and Googlebook OS
- ChromeOS has 2.04% of desktop page views — [StatCounter](https://gs.statcounter.com/os-market-share/desktop/worldwide). Googlebooks are positioned as premium devices, and some Android apps ran poorly on Intel models at launch — [Wikipedia](https://en.wikipedia.org/wiki/Googlebook). A multi-year coexistence with ChromeOS is expected, reportedly until 2034 (secondary) — [StarryHope](https://www.starryhope.com/chromebooks/chromeos-android-merger-aluminium-os/)

### Inferences
Weaknesses a newcomer could exploit as niches. These are judgements, ordered by fit for a small from-scratch OS such as Terminal OS.
1. **Coherent, opinionated system on one narrow hardware target (the "Apple model" at hobby scale).** macOS shows that vertical integration can beat breadth. A newcomer can certify one or two reference laptops (Terminal OS already targets Intel Gen12 graphics, AX2xx Wi-Fi, I2C-HID and HDA) and make them work flawlessly: suspend, Wi-Fi, audio, display, touchpad. This turns the C8 weakness into a focused strength.
2. **Lean OS for 8 GB and older PCs.** Windows 11 uses most of 8 GB after boot, needs TPM 2.0 and Secure Boot (stranding the 21-30% still on Windows 10), and loses free consumer ESU in Oct 2027. Memory prices make RAM efficiency commercially relevant. Caveat: this niche is contested by lightweight Linux distros, and it needs a browser that works (Ladybird).
3. **AI-free, privacy-first, no-telemetry desktop.** Microsoft is cutting back Copilot entry points after backlash, and agent workspaces raise file-access concerns. A newcomer can promise "no agents, no telemetry, no ads", but must still meet basic security table stakes (C5).
4. **Fast, responsive, low-latency UX.** Windows restarts take 61 s and Microsoft admits File Explorer is slow. A small system can visibly win on boot-to-desktop time, restart time and UI latency, and these are measurable claims.
5. **Comprehensibility and education.** A codebase of tens of thousands of lines, against Linux's 43.9M, can be read end to end. This is SerenityOS's and Haiku's appeal and a durable niche for a hobby OS.
6. **Appliance, kiosk and single-purpose devices.** Fixed hardware, image-based updates and fast boot. Googlebook OS's requirements show that UEFI and NVMe x64 or Arm64 is the modern baseline even here.
7. **Not realistic niches:** gaming (93.7% Windows, plus Proton on Linux), enterprise fleets (Intune, Entra, hotpatch) and creative professionals (proprietary app lock-in).

### Gaps
- Systematic 2026 reviewer verdicts (for example Ars Technica's macOS 27 review, or The Verge or LWN year-end assessments) were not fetched. The weaknesses above come from specific articles, not a consensus survey.
- No 2026 user-satisfaction or retention data (for example Windows 11 rollbacks to Windows 10) was verified. One Windows Central headline suggests Windows 11 adoption flatlined at some point, but its date and context were not checked.

## 5. Emerging trends that will define the bar by 2028-2030

### Takeaway
Ten trends are already visible in 2026 shipping software and will raise the bar:
1. OS-level AI agents and on-device models: MCP connectors and agent workspaces in Windows, Siri AI and the Foundation Models framework on macOS, Gemini in Googlebook OS. These come with a backlash and a pull-back on intrusive AI.
2. NPUs as standard PC hardware, from a 40 TOPS floor to 45-80 TOPS in 2026.
3. ARM64 as a first-class laptop platform: an all-ARM Mac line, Windows on Snapdragon X2, Arm64 Googlebooks.
4. Memory safety: Rust permanent in Linux, with DRM moving to require it for new drivers; Rust coreutils in Ubuntu; Rust at Microsoft; hardware memory tagging in Apple's M5.
5. AI-accelerated vulnerability discovery and exploitation, which raises patch volume and speed requirements.
6. Image-based, atomic, rollback-capable updates and hotpatching.
7. Wayland-only, HDR and colour-managed desktops.
8. Post-quantum cryptography and passkey-first authentication by default.
9. Regulation: the EU DMA already designates Windows as a gatekeeper OS, and Apple lost its court challenge for iOS.
10. Memory-price-driven pressure for RAM efficiency.

### Cited Findings

#### AI agents and on-device models in the OS
- Windows:
  - Agent connectors (MCP servers) with an On-Device Registry, and an isolated, policy-controlled, auditable agent workspace — [Neowin](https://www.neowin.net/amp/microsoft-is-making-ai-agents-a-first-class-component-of-windows/).
  - "Agent in Settings" in 26H2 — [Pureinfotech](https://pureinfotech.com/windows-11-26h2-released-force-install/).
  - MCP tools in Copilot Studio agent workflows, generally available on 15 Jul 2026 (secondary) — [HANDS ON tek](https://m365admin.handsontek.net/?p=23860).
  - Microsoft remains "unwavering" on an agentic OS — [ITPro](https://www.itpro.com/technology/artificial-intelligence/microsoft-is-unwavering-on-making-windows-an-agentic-os-)
- The backlash: Microsoft is cutting "unnecessary Copilot entry points" — [The Register](https://theregister.com/2026/03/23/windows_quality_commitment/); [Redmond Magazine](https://redmondmag.com/articles/2026/03/24/microsoft-dials-back-copilot.aspx)
- Apple: Siri AI ships in macOS 27 as a beta, with advanced features requiring an M3 or later Mac with at least 12 GB — [Macworld](https://www.macworld.com/article/3139330/macos-27-golden-gate-has-arrived-heres-what-s-new-on-your-mac.html). The on-device model is exposed to developers, and the 2026 models were reportedly built with Gemini technology (secondary) — [Firebase blog](https://firebase.blog/posts/2026/06/apple-foundation-models-gemini); [MacRumors](https://www.macrumors.com/2026/03/25/apple-google-gemini-distill-models/)
- Google: Googlebook OS is built around "Gemini Intelligence" — [Wikipedia](https://en.wikipedia.org/wiki/Googlebook)
- Linux: Ubuntu 26.10 adds opt-in on-device speech-to-text ("Myna") — [OMG! Ubuntu](https://www.omgubuntu.co.uk/2026/10/ubuntu-2610-new-features)

#### NPUs
- 40 TOPS is the Copilot+ threshold — [Wccftech](https://wccftech.com/intel-outlines-40-tops-npu-performance-minimum-requirement-windows-copilot-ai-pc/). In practice it has become the minimum: many 2026 devices have 45-55 TOPS and some ARM parts up to 80 — [PC-WELT](https://www.pcwelt.de/article/2819676/copilot-pc-lohnt-sich-der-kauf-eines-ki-rechners-das-muessen-sie-wissen.html). Task Manager shows NPU activity in 26H2 — [Pureinfotech](https://pureinfotech.com/windows-11-26h2-released-force-install/)
- Counter-signal: PCWorld reports that Microsoft shifted strategy away from NPU dependence after OEMs rushed to meet the 40 TOPS bar (headline and summary only) — [PCWorld](https://www.pcworld.com/article/3028591/laptop-makers-embraced-ai-then-microsoft-left-them-hanging.html)

#### ARM64 laptops
- Apple silicon only from macOS 27, with Rosetta ending — [Macworld](https://www.macworld.com/article/3139330/macos-27-golden-gate-has-arrived-heres-what-s-new-on-your-mac.html). Windows has a dedicated Bromine platform for Snapdragon X2 — [Windows Central](https://www.windowscentral.com/microsoft/windows-11/windows-11-version-26h1-will-launch-exclusively-on-snapdragon-x2-devices-this-spring). Googlebook runs on Arm64 or x64 — [Wikipedia](https://en.wikipedia.org/wiki/Googlebook). Linux ARM laptop support lags — [Phoronix](https://www.phoronix.com/review/snapdragon-x-elite-linux-eoy2025)

#### Memory-safe languages and hardware memory safety
- Rust is permanent in Linux, and DRM is about a year from requiring Rust for new drivers — [LWN](https://lwn.net/Articles/1047293); [Phoronix](https://www.phoronix.com/news/Rust-To-Stay-Linux-Kernel). Ubuntu 26.10 fully adopts Rust coreutils — [OMG! Ubuntu](https://www.omgubuntu.co.uk/2026/10/ubuntu-2610-new-features). Microsoft's Rust push is real, but a full Windows rewrite by 2030 was denied — [Windows Central](https://www.windowscentral.com/microsoft/windows-11/my-goal-is-to-eliminate-every-line-of-c-and-c-from-microsoft-by-2030-microsoft-bets-on-ai-to-finally-modernize-windows); [igor'sLAB](https://www.igorslab.de/en/microsoft-puts-the-brakes-on-speculation-about-a-full-rust-port-of-windows/). Apple's MIE puts memory tagging in shipping laptops — [Computerworld](https://www.computerworld.com/article/4124435/apple-touts-unparalleled-protection-for-m5-macs.html)

#### AI-accelerated security research and bug volume
- A working M5 macOS kernel exploit was built in five days with Anthropic's Mythos Preview — [9to5Mac](https://9to5mac.com/2026/05/14/calif-team-details-how-anthropic-mythos-helped-build-a-working-macos-exploit-in-five-days/). AI tools are driving record kernel fix volumes, which Torvalds calls the "new normal" — [Let's Data Science](https://letsdatascience.com/news/torvalds-releases-linux-72-rc7-amid-ai-fixes-1d6ee190); [Neowin](https://www.neowin.net/amp/linux-70-arrives-as-linus-torvalds-embraces-a-new-era-of-ai-driven-kernel-development/)

#### Immutable, atomic and live-patchable systems
- KDE Linux uses A/B images with snapshot rollback — [OMG! Linux](https://www.omglinux.com/kde-linux-immutable-os/). Fedora Atomic is built with bootc — [Fedora Magazine](https://fedoramagazine.org/building-your-own-atomic-bootc-desktop/). SteamOS and Bazzite, both image-based, together account for about 29% of Linux Steam users — [GamingOnLinux](https://www.gamingonlinux.com/2026/11/steam-survey-october-2026-shows-an-uptick-for-the-linux-user-share/). Windows hotpatches without reboots — [Microsoft Learn](https://learn.microsoft.com/windows/deployment/windows-autopatch/manage/windows-autopatch-hotpatch-updates). Linux 7.0's NULLFS targets systems that mount the real root later — [OSTechNix](https://ostechnix.com/linux-kernel-7-0-released/)

#### Wayland-only, HDR and colour-managed desktops
- GNOME 50 removed its X11 session — [Phoronix](https://www.phoronix.com/news/GNOME-50-Alpha) — and Plasma 6.8 followed — [KDE](https://kde.org/announcements/plasma/6/6.8.0/). HDR with ICC profiles and HLG arrived in KWin — [KDE](https://kde.org/announcements/plasma/6/6.8.0/)

#### Post-quantum crypto, passkeys and platform trust
- Ubuntu 26.04 makes post-quantum hybrid crypto the default, and 26.10 adds post-quantum-ready package signing — [OSTechNix](https://ostechnix.com/ubuntu-26-04-lts-resolute-raccoon-features-release-notes/); [OMG! Ubuntu](https://www.omgubuntu.co.uk/2026/10/ubuntu-2610-new-features). GNOME 51 supports FIDO2/passkey login — [OMG! Ubuntu](https://www.omgubuntu.co.uk/2026/09/gnome-51-released). Secure Boot's 2011 CAs expired in 2026, and the 2023 CAs run to 2038 — [LWN](https://lwn.net/Articles/1079808/)

#### Regulation (EU Digital Markets Act)
- The designated gatekeeper operating systems are Microsoft's Windows PC OS (6 Sep 2023), Apple's iOS (6 Sep 2023) and iPadOS (29 Apr 2024), and Google's Android Mobile. macOS is not designated — [European Commission DMA](https://digital-markets-act.ec.europa.eu/gatekeepers_en)
- On 8 Jul 2026 the EU General Court dismissed Apple's challenge to its gatekeeper designation for the App Store and iOS — [PYMNTS](https://www.pymnts.com/cpi-posts/eu-court-rejects-apple-challenge-to-digital-markets-act-gatekeeper-status/). Coverage cites DMA fines, delayed features and unclear benefits — [Euronews](https://www.euronews.com/2026/06/11/the-eus-dma-fines-delayed-features-and-unclear-benefits). Siri AI is unavailable on iPhone and iPad in the EU — [Macworld](https://www.macworld.com/article/3139330/macos-27-golden-gate-has-arrived-heres-what-s-new-on-your-mac.html)

#### Hardware economics
- PC shipments fell 20.1% year on year in Q3 2026 amid memory-related price rises (IDC preliminary figures, secondary report) — [Neoteo](https://www.neoteo.com/en/idc-estimates-global-pc-shipments-fell-201-in-q3-2026). Windows' RAM diet is "a commercial necessity" — [The Register](https://theregister.com/2026/03/23/windows_quality_commitment/); [Pureinfotech](https://pureinfotech.com/windows-11-8gb-memory-optimization/)

### Inferences
What will likely be table stakes by 2028-2030 (projections, not facts):
- **Memory-safety strategy.** New kernel drivers and core userland in a memory-safe language, or hardware tagging, or both. Linux DRM's move to require Rust, Ubuntu's Rust coreutils and Apple's MIE point there. A pure-C newcomer will be judged on its mitigations (CFI, sanitizers, fuzzing) and on whether new subsystems use Rust. For Terminal OS, the cheapest credible step is to write new drivers and services in Rust, or at least to fuzz the parsers (network stack, filesystems, USB descriptors).
- **Patch velocity.** AI-assisted bug finding (record kernel fix volumes; a five-day M5 exploit) means an OS needs signed automatic updates and the ability to ship fixes within days. Atomic A/B updates with rollback will be expected even on consumer desktops (KDE Linux, Fedora Atomic, SteamOS and Bazzite already cover a large share of Linux gamers).
- **AI as an optional, permissioned OS service.** Expect standard agent-connector APIs (MCP-style), sandboxed agent sessions with audit logs, on-device models using NPUs, and NPU drivers. Given the 2026 backlash, a credible "off switch" and local-only processing will be a selling point, not a gap. A newcomer can skip shipping AI but should not preclude an NPU driver and a model runtime later.
- **ARM64 parity.** By 2030 a general-purpose OS without ARM64 laptop support is likely to look legacy-only, as Mac, Windows on Snapdragon and Googlebook show.
- **Modern display and input stack.** Wayland-style compositing with HDR, colour management, VRR and fractional scaling as defaults rather than options.
- **Post-quantum TLS and SSH, passkeys and Secure Boot hygiene.** Hybrid ML-KEM key exchange in TLS 1.3 and SSH, passkey or FIDO2 sign-in, and handling of firmware certificate rotations (the 2011 to 2023 CA change). TLS 1.2-only stacks (as in BearSSL today) will be increasingly out of step.
- **Regulatory awareness.** Only gatekeeper-scale OSes face DMA obligations, but interoperability expectations (side-loading, default-app choice, data portability) are spreading. EU product-security rules may also impose vulnerability-handling and update obligations on anyone distributing software commercially in the EU (see Gaps).
- **Efficiency as a feature.** With RAM expensive and Windows trimming its footprint, idle-RAM and boot-time numbers will be marketed. This is a field where a small OS can compete on measured numbers.

### Gaps
- EU Cyber Resilience Act timelines and obligations (vulnerability reporting, SBOMs, support periods), and whether they would apply to a free hobby OS, were not researched this session.
- Whether Windows Recall, Click to Do, Windows ML or Foundry Local reached general availability in 2026, and the current Copilot+ feature list, were not verified.
- The status of Apple's "Background Security Improvements" or rapid security responses in macOS 26/27 was not verified.
- No authoritative forecast (IDC or Gartner) of ARM's share of PC shipments by 2028-2030 was found.
- The PCWorld claim that Microsoft is moving away from NPU dependence was seen only as a headline and summary.
- No Microsoft roadmap for a post-Windows-11 brand ("Windows 12") or for the 27H2 platform merge was found beyond press reporting.
