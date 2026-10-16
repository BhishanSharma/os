#!/usr/bin/env python3
"""Start a new desktop app: apps/NAME/ with a main.c that opens a window,
and an app.cfg that puts it in the Dock.

    python scripts/new-app.py clock "Clock"        (name, then the title)

Then build (.\\build.ps1, or make in Docker) and click its Dock icon. Read
docs/APPS.md for the drawing and event calls.
"""
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

MAIN_C = r'''// __NAME__ - __TITLE__: (say what it does here)
//
// A window that the user can maximise (the green button), with a button
// that counts clicks. Replace the drawing and the event handling with yours.
#include <stdio.h>
#include <string.h>
#include <os.h>
#include <ui.h>

static ui_window win;
static int S;                                   /* 1, or 2 on a large screen: multiply sizes by it */
static int clicks;

/* The button's place, from the window's size (it can change: WS_EV_RESIZE). */
static void button_rect(int *x, int *y, int *w, int *h) {
    *w = 140 * S;
    *h = 32 * S;
    *x = (win.c.w - *w) / 2;
    *y = win.c.h / 2 + 10 * S;
}

static void draw(void) {
    ui_canvas *c = &win.c;
    ui_fill(c, 0, 0, c->w, c->h, UI_WHITE);
    const char *title = "__TITLE__";
    ui_text_bold(c, (c->w - ui_text_style_width(title, S, UI_BOLD)) / 2, c->h / 2 - 50 * S, title, UI_TEXT, S);
    char line[48];
    snprintf(line, sizeof(line), "Clicked %d time%s", clicks, clicks == 1 ? "" : "s");
    ui_text(c, (c->w - ui_text_width(line, S)) / 2, c->h / 2 - 22 * S, line, UI_DIM, S);
    int x, y, w, h;
    button_rect(&x, &y, &w, &h);
    ui_button(c, x, y, w, h, "Click me", S, 0);
    ui_present(&win);                           /* show what was drawn */
}

int main(void) {
    S = ui_scale();
    if (ui_open(&win, "__TITLE__", 420 * S, 280 * S) < 0) {
        printf("__NAME__: the desktop is not running (type windowserver)\n");
        return 1;
    }
    ui_set_resizable(&win);                     /* draw() follows win.c.w and win.c.h */
    draw();
    for (;;) {
        struct ws_event e;
        ui_wait(&win, &e, 0);                   /* sleeps until there is an event */
        if (e.type == WS_EV_CLOSE) return 0;    /* the red button, or Ctrl+W */
        if (e.type == WS_EV_DOWN) {             /* a click: e.x, e.y in the window */
            int x, y, w, h;
            button_rect(&x, &y, &w, &h);
            if (!ui_inside(e.x, e.y, x, y, w, h)) continue;
            clicks++;
        } else if (e.type == WS_EV_KEY) {       /* a key: e.key ('a', '\n', KEYCODE_UP, ...) */
            if (e.key != ' ') continue;
            clicks++;
        } else if (e.type != WS_EV_RESIZE) {    /* the new size is in win.c already */
            continue;
        }
        draw();
    }
}
'''

APP_CFG = """# __TITLE__ (see docs/APPS.md)
name = __TITLE__
# An icon in ICONS.BIN: put an icon.png in this folder and use "__NAME__"
# (then run python scripts/gen-icons.py), or use one of third_party/icons.
icon = __ICON__
# Its place in the Dock (lower is further left); delete the line to leave it out.
dock = __DOCK__
# The drawn icon's colour (RRGGBB) when there is no picture.
color = 5E5CE6
"""


def main():
    if len(sys.argv) not in (2, 3):
        sys.exit(__doc__)
    name = sys.argv[1]
    title = sys.argv[2] if len(sys.argv) == 3 else name.capitalize()
    if not re.fullmatch(r"[a-z][a-z0-9]{0,7}", name):
        sys.exit("The name is the program's file name: 1-8 lower-case letters or digits, starting with a letter.")
    if os.path.exists(os.path.join(ROOT, "user", "programs", name + ".c")):
        sys.exit("There is already a program called %s (user/programs/%s.c)." % (name, name))
    folder = os.path.join(ROOT, "apps", name)
    if os.path.exists(folder):
        sys.exit("apps/%s exists already." % name)
    if '"' in title or "\\" in title or "|" in title:
        sys.exit('The title cannot contain " \\ or |.')
    docks = []
    for cfg in (os.path.join(ROOT, "apps", a, "app.cfg") for a in os.listdir(os.path.join(ROOT, "apps"))):
        if os.path.isfile(cfg):
            docks += [int(m) for m in re.findall(r"^\s*dock\s*=\s*(\d+)", open(cfg).read(), re.M)]
    os.makedirs(folder)
    fill = lambda t: (t.replace("__NAME__", name).replace("__TITLE__", title)
                      .replace("__ICON__", "file").replace("__DOCK__", str(max(docks, default=0) + 1)))
    with open(os.path.join(folder, "main.c"), "w", newline="\n") as f:
        f.write(fill(MAIN_C))
    with open(os.path.join(folder, "app.cfg"), "w", newline="\n") as f:
        f.write(fill(APP_CFG))
    print("Made apps/%s/ (main.c, app.cfg). Build and it is in the Dock; the guide is docs/APPS.md." % name)


if __name__ == "__main__":
    main()
