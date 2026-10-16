# Writing a desktop app

An app is a folder in `apps/`. The build turns every folder with an
`app.cfg` into a program on the system disk and puts it in the Dock: you
don't need to edit the Makefile, the window server or a list anywhere.

```
apps/
  paint/
    app.cfg        what the Dock shows: name, icon, place
    main.c         the program (any number of .c and .h files)
    icon.png       its own icon (optional)
```

## Quick start

```powershell
python scripts/new-app.py clock "Clock"     # makes apps/clock/ from a template
.\build.ps1                                 # build, then boot in QEMU
```

Log in, and Clock is at the end of the Dock. Click it to open the window.
`.\build.ps1 -Usb` writes the same build to a USB stick for a real PC (see
`scripts/write-usb.ps1`).

The name (`clock`) is the program's file name: 1-8 lower-case letters or
digits, starting with a letter. Typing `clock` in Terminal also starts it.

## app.cfg

```ini
# Clock: the time, big
name = Clock          # in the Dock and the menu bar
icon = clock          # an icon in ICONS.BIN (see "Icons")
dock = 7              # its place in the Dock, lower is further left; leave out: not in the Dock
color = 5E5CE6        # the drawn icon's colour (RRGGBB) when there is no picture
```

`scripts/mkdock.sh` reads these at build time and writes `DOCK.CFG`, which the
window server reads when it starts.

## The program

Every app has the same shape: open a window, draw, wait for an event, draw
again. This is the template that `new-app.py` writes, shortened:

```c
#include <stdio.h>
#include <os.h>      // files, time, processes, network: the system calls
#include <ui.h>      // windows, drawing, text, widgets

static ui_window win;
static int S;        // 1, or 2 on a large screen: multiply every size by it

static void draw(void) {
    ui_canvas *c = &win.c;                      // c->w, c->h: the window's size now
    ui_fill(c, 0, 0, c->w, c->h, UI_WHITE);
    ui_text_bold(c, 20 * S, 20 * S, "Hello", UI_TEXT, S);
    ui_button(c, 20 * S, 50 * S, 120 * S, 32 * S, "Click me", S, 0);
    ui_present(&win);                           // show it
}

int main(void) {
    S = ui_scale();
    if (ui_open(&win, "Clock", 420 * S, 280 * S) < 0) return 1;   // no desktop
    ui_set_resizable(&win);                     // the green button may resize it
    draw();
    for (;;) {
        struct ws_event e;
        ui_wait(&win, &e, 0);                   // sleep until something happens
        if (e.type == WS_EV_CLOSE) return 0;
        if (e.type == WS_EV_DOWN && ui_inside(e.x, e.y, 20 * S, 50 * S, 120 * S, 32 * S))
            ;                                   // the button was clicked
        draw();
    }
}
```

### Events (`struct ws_event`, from `ui_wait` or `ui_poll`)

| type           | meaning                                   | fields                         |
| -------------- | ----------------------------------------- | ------------------------------ |
| `WS_EV_DOWN`   | mouse button pressed                      | `x`, `y` in the window         |
| `WS_EV_UP`     | released                                  | `x`, `y`                       |
| `WS_EV_MOVE`   | the pointer moved (while over the window) | `x`, `y`, `buttons`            |
| `WS_EV_WHEEL`  | scrolled                                  | `key`: steps, + down / - up    |
| `WS_EV_KEY`    | a key                                     | `key`: `'a'`, `'\n'`, `'\b'`, `27` (Esc), `KEYCODE_UP`/`DOWN`/`LEFT`/`RIGHT`, Ctrl+letter = 1-26 |
| `WS_EV_RESIZE` | the window has a new size                 | `x`, `y` = width, height (already in `win.c`) |
| `WS_EV_CLOSE`  | the red button or Ctrl+W                  | -                              |

`ui_wait(&win, &e, ms)` waits up to `ms` (0: no limit) and returns 0 on a
timeout: a clock redraws on the timeout. `ui_poll` never waits (games, video).

### Drawing (`ui.h`)

All coordinates are pixels in the window, colours `0xRRGGBB`.

| Call | |
| ---- | - |
| `ui_fill`, `ui_fill_a` | rectangle, solid or with alpha 0-255 |
| `ui_round`, `ui_round_ex` | rounded rectangle (anti-aliased) |
| `ui_circle`, `ui_frame`, `ui_shadow` | circle, outline, soft shadow |
| `ui_text`, `ui_text_bold`, `ui_text_style(..., UI_MONO)` | text; a line is `16 * S` high, `y` is its top; returns the width |
| `ui_text_width`, `ui_text_block` | measure; wrap a paragraph into a width |
| `ui_icon(c, x, y, size, "folder", 255)` | an icon from ICONS.BIN |
| `ui_copy` | your own pixels (an image) |
| `ui_button`, `ui_textfield`, `ui_list`, `ui_list_icons` | widgets: drawn by you, hit-tested with `ui_inside` |
| `ui_clip`, `ui_noclip` | keep drawing inside a rectangle |
| `ui_set_title` | change the title bar |

Colours that match the rest of the desktop: `UI_WINDOW`, `UI_TEXT`, `UI_DIM`,
`UI_ACCENT`, `UI_BORDER`, `UI_WHITE`. `ui_pref(UI_PREF_DARK)` tells you if
the user chose the dark appearance.

### The system (`os.h`)

Files: `open`, `read`, `write`, `close` (saved at close), `stat`, `listdir`,
`mkdir`, `unlink`. Time: `uptime_ms`, `gettime`, `sleep_ms`. Programs:
`spawn("textedit", argv, 0)` opens another app. Threads: `thread_create`,
`mutex_lock`. Network: `resolve`, `net_connect`, `net_send`, `net_recv`.
Sound: `sound_open`, `sound_write` (`video.c` shows how). The comments in
`user/include/os.h` explain each one; `docs/USERSPACE.md` covers more.

`printf` writes to the text console (and the serial port): use it for
debug output, which shows up in the test logs.

## Rules of thumb

- **No floating point.** Programs are built with `-mgeneral-regs-only`: use
  integers (fixed point, `* 1000`). The C library is small: `string.h`,
  `stdio.h` (`snprintf`, `printf`), `stdlib.h`; look in `user/include` for
  what exists.
- **Multiply sizes by `S`** (`ui_scale()`), so the app looks right on a
  1080p laptop and on a 4K screen.
- **Lay out from `win.c.w` and `win.c.h`**, not from the size you asked for:
  a small screen gives a smaller window, and the green button resizes it.
- **Wait, don't spin.** Use `ui_wait`; a loop with `ui_poll` needs a
  `sleep_ms` in it, or it takes a whole CPU core.
- Programs run as the user who logged in: they may write only in their home
  folder (`/HOME/NAME`) and `/TMP`.

## Icons

The icons live in `targets/x86_64/ramdisk/ICONS.BIN`, made by
`python scripts/gen-icons.py` (needs Pillow: `pip install pillow`). It takes
the icons in `third_party/icons/` and every `apps/NAME/icon.png` (a square
PNG with transparency, 512 x 512 or bigger), which becomes the icon `NAME`.
Run it after adding or changing an icon and commit `ICONS.BIN` with it.

Only use pictures you are allowed to share (your own, or under a licence
such as MIT or CC-BY), and say where they come from in the commit.

## Testing

```text
python tests/qemu/run_tests.py apps     every app in apps/ starts and quits cleanly
python tests/qemu/run_tests.py          everything (about 5 minutes)
```

The `apps` test opens every app at once on the desktop and fails if one
crashes; its screenshot is `build/test/qemu/apps.png`. Run it before sending
your change.

## Where the parts are

| | |
| - | - |
| `apps/*/` | the desktop apps (Files is `finder`) |
| `user/programs/` | command-line programs and the system's own: `windowserver`, `loginwindow`, `shell`, `launchd` |
| `user/lib/ui.c`, `user/include/ui.h` | drawing and windows |
| `user/include/ws.h` | how a window talks to the window server |
| `src/` | the kernel |
