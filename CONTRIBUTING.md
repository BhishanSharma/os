# Contributing to Terminal OS

Thanks for helping. The easiest place to start is a desktop app: it lives in
its own folder and touches nothing else.

## Set up (once)

- Windows: Docker Desktop, QEMU (on `PATH`), Python 3 with Pillow for icons
  (`pip install pillow`). Linux and macOS: see [docs/SETUP.md](docs/SETUP.md).
- `.\build.ps1` builds in Docker and boots the result in QEMU.

## Add an app

```powershell
python scripts/new-app.py clock "Clock"     # apps/clock/: main.c and app.cfg
.\build.ps1                                 # it is in the Dock
```

[docs/APPS.md](docs/APPS.md) is the guide: the window and event calls, the
drawing calls, icons, and the rules (no floating point, sizes times the
scale, wait instead of spinning).

## Other changes

| You want to change | Look at | Guide |
| ------------------ | ------- | ----- |
| an app | `apps/NAME/` | [docs/APPS.md](docs/APPS.md) |
| a command-line program | `user/programs/` | [docs/USERSPACE.md](docs/USERSPACE.md) |
| the desktop (Dock, menus, windows) | `user/programs/windowserver.c`, `user/lib/ui.c` | [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) |
| the kernel or a driver | `src/` | [docs/DEVELOPING.md](docs/DEVELOPING.md) |

## Before you send it

1. It builds without new warnings (`.\build.ps1 -NoRun`).
2. The tests pass: `python tests/qemu/run_tests.py` (or at least `apps` for
   an app). They boot the OS in QEMU and drive it; screenshots and logs land
   in `build/test/qemu/`.
3. If you can, try it on a real PC: `.\build.ps1 -Usb` writes a USB stick.
4. One topic per pull request, with a message that says what changed and why.

## Style

- Match the code around you: 4 spaces, braces on the same line, short
  comments that say *why*, in plain English.
- No new dependencies without asking. Code you didn't write must be under a
  licence that allows it here (MIT, BSD, zlib, public domain): put it in
  `third_party/` with its licence file. No GPL code.
- Pictures and fonts too: only ones you may share, with their licence.

Terminal OS is under the MIT licence ([LICENSE](LICENSE)); by sending a change
you agree that it is shared under it too.
