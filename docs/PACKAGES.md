# Installing apps from other people (`.tapp` packages)

Apps that are not part of Terminal OS come as **packages**: one `.tapp` file
with the program, its name and its icon. Anyone can make one with the SDK
(this repository's toolchain), put it on a website or a USB stick, and anyone
can install it, with no administrator password and without rebuilding the OS.

## Installing

- **In Files:** open (double-click) the `.tapp` file. A window asks
  *Install "Hello"?* and shows the version and author. **Install**, then
  **Open** or **Done**. The app is in the Dock from then on.
- **In Terminal:**

  ```
  tapp install Downloads/hello.tapp    install (or update) it
  tapp list                            the installed apps
  tapp remove hello                    remove it (by its id)
  ```

Apps go to **your own** `~/Apps/ID/` (for example `/HOME/BOSS/Apps/hello/`):
each user has their own apps, and installing one cannot change the system or
another user's files. Installing a package whose id is already installed
replaces it (an update). On a PC with the NVMe `TERMINALOS` partition they are
kept across reboots; on a USB stick boot without it, they last until shutdown.

An installed app runs as you and can read and change your files, like any
program you start: install apps only from people you trust. Packages are not
signed yet.

## Making a package

An app is a folder, like the built-in ones in `apps/` (see
[APPS.md](APPS.md) for writing one: windows, views, drawing, files):

```
examples/hello/
  app.cfg        id, name, version, author, color
  main.c         the program (any number of .c and .h files)
  icon.png       its icon (optional; square, any size, transparency kept)
```

```ini
id = hello            # 1-8 lower-case letters or digits, a letter first: the folder's name
name = Hello          # in the Dock and the menu bar
version = 1.0
author = Your Name
color = FF9500        # the drawn icon's colour when there is no icon.png
icon = folder         # (optional) a built-in icon instead of icon.png
```

Build the package (in Docker, like the OS):

```powershell
.\build.ps1 -Package examples\hello      # -> build\packages\hello.tapp
```

(or `make tapp SRC=examples/hello` inside the build container). The `.c`
files are compiled and linked exactly like the built-in apps, against
`user/include` and `user/lib` (the SDK: `ui.h`, `view.h`, `os.h`, `stdio.h`,
`image.h`...). Any other files in the folder (pictures, data) go into the
package too and are installed next to the program; the program finds them in
its own folder (`~/Apps/ID/`).

## The format

A `.tapp` file is a plain **ustar tar archive** of flat files (no folders,
links or special files):

| File | |
| --- | --- |
| `app.cfg` | required: `id`, and `name`, `version`, `author`, `color`, `icon` |
| `ID` | required: the program (an ELF for Terminal OS), named like the id |
| `icon.png` | optional: the Dock icon |
| anything else | the app's own files |

So it can also be made by hand: `tar --format=ustar -cf hello.tapp app.cfg hello icon.png`.
`tapp` checks it before installing anything (a tar archive, an id, a program
that is an ELF file, nothing outside the app's folder) and installs at most
64 MB.

## How it fits together

- `user/programs/tapp.c`: the installer (window and commands).
- `apps/finder`: Files opens `.tapp` files with `tapp`.
- `user/programs/windowserver.c`: the Dock lists the built-in apps
  (`DOCK.CFG`) and then `~/Apps/*/app.cfg`; `tapp` sends it `WS_APPS` after
  installing or removing, and it reads them again.
- `user/lib/ui.c`: `ui_icon` draws a picture file when the icon name is a
  path (`icon.png`).
