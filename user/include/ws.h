// ws.h - talking to the window server (and the UI library on top of it)
//
// A program asks the "windowserver" service (a port) for a window. The
// answer names a shared memory surface: a header (size, damage counter,
// events) followed by the window's pixels, 0x00RRGGBB. The program draws
// into the pixels and bumps `damage`; the server copies what changed to the
// screen. Mouse and key events come back in the header's ring.
#ifndef WS_H
#define WS_H

#define WS_SERVICE "windowserver"

enum { WS_CREATE = 1, WS_TITLE = 2 };

struct ws_request {
    int op;
    int w, h;              /* WS_CREATE: the content size in pixels */
    int pid;               /* the program the window belongs to */
    int id;                /* WS_TITLE */
    char title[48];
};

struct ws_reply {
    int id;                /* < 0: an error */
    char shm[32];          /* the surface's shared memory name */
    int x, y;              /* where the content starts on the screen */
};

enum { WS_EV_KEY = 1, WS_EV_DOWN, WS_EV_UP, WS_EV_MOVE, WS_EV_WHEEL, WS_EV_CLOSE };

struct ws_event {
    int type;
    int x, y;              /* in the window's content */
    int key;               /* WS_EV_KEY: the getkey() code; WS_EV_WHEEL: steps */
    int buttons;
};

#define WS_EVENTS 64
#define WS_HEADER 4096     /* the pixels start here */

struct ws_surface {
    int w, h;
    volatile unsigned damage;          /* the program bumps it after drawing */
    volatile unsigned ev_head;         /* the server writes events here */
    volatile unsigned ev_tail;         /* the program reads from here */
    volatile int focused;
    struct ws_event ev[WS_EVENTS];
};

static inline unsigned long ws_surface_size(int w, int h) {
    return WS_HEADER + (unsigned long)w * (unsigned long)h * 4;
}

#endif
