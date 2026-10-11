// loginwindow - the graphical login (like macOS's loginwindow): a picture,
// the user's name and password, then the desktop as that user. Logging out
// of the desktop ends it, and launchd starts a fresh login screen.
//
// User name ">console" (as on a Mac) gives the text login instead; so does a
// screen without graphics.
#include <stdio.h>
#include <string.h>
#include <os.h>
#include <ui.h>

static struct os_gfx_info g;
static ui_canvas scr;
static int W, H, S;
static char name[16], password[64], message[64];
static int field;                                   /* 0 name, 1 password */

static void text_login(void) {
    gfx_end();
    char *argv[] = { "login", 0 };
    exec("login", argv);
    printf("loginwindow: cannot start login\n");
    exit(1);
}

static void draw(void) {
    ui_noclip(&scr);
    for (int y = 0; y < H; y++) {                      /* the desktop's evening sky, darker */
        int t = (y * 256) / H;
        int r = 0x1A + ((0x6A - 0x1A) * t >> 8), gr = 0x36 + ((0x40 - 0x36) * t >> 8), b = 0x9A + ((0xA0 - 0x9A) * t >> 8);
        ui_fill(&scr, 0, y, W, 1, (unsigned)(r << 16 | gr << 8 | b));
    }
    struct os_time t;
    gettime(&t);
    char clock[8];
    snprintf(clock, sizeof(clock), "%d:%02d", t.hour, t.minute);
    int cw = ui_text_style_width(clock, 4, UI_BOLD);
    ui_text_bold(&scr, (W - cw) / 2, H / 7, clock, 0xFFFFFF, 4);   /* a big clock, like a lock screen */

    int cx = W / 2, top = H / 2 - 120 * S;
    int av = 110 * S;                                  /* the avatar */
    ui_circle(&scr, cx, top + av / 2, av / 2 + 3 * S, 0xFFFFFF);
    ui_circle(&scr, cx, top + av / 2, av / 2, 0xD8DCE8);
    ui_icon(&scr, cx - av * 35 / 100, top + av * 15 / 100, av * 70 / 100, "user", 255);

    int fw = 240 * S, fh = 34 * S, fx = cx - fw / 2, fy = top + av + 26 * S;
    const char *title = name[0] ? name : "Terminal OS";
    int tw = ui_text_style_width(title, S, UI_BOLD);
    ui_text_bold(&scr, cx - tw / 2, fy - 8 * S - 16 * S + 4 * S, title, 0xFFFFFF, S);
    fy += 8 * S;
    ui_round_ex(&scr, fx, fy, fw, fh, fh / 2, 0xFFFFFF, field == 0 ? 120 : 70, UI_ALL);
    ui_text(&scr, fx + 16 * S, fy + (fh - 16 * S) / 2, name[0] ? name : "Name", name[0] ? 0xFFFFFF : 0xD0D4E8, S);
    if (field == 0) ui_fill(&scr, fx + 16 * S + ui_text_width(name, S) + S, fy + 9 * S, S, 16 * S, 0xFFFFFF);

    int py = fy + fh + 12 * S;
    ui_round_ex(&scr, fx, py, fw, fh, fh / 2, 0xFFFFFF, field == 1 ? 120 : 70, UI_ALL);
    int n = (int)strlen(password), dx = fx + 18 * S;
    if (!n) ui_text(&scr, fx + 16 * S, py + (fh - 16 * S) / 2, "Password", 0xD0D4E8, S);
    for (int i = 0; i < n && i < 24; i++, dx += 12 * S) ui_circle(&scr, dx, py + fh / 2, 4 * S, 0xFFFFFF);
    if (field == 1) ui_fill(&scr, dx - 4 * S + S, py + 9 * S, S, 16 * S, 0xFFFFFF);

    const char *hint = message[0] ? message : "Enter: next   Tab: switch fields";
    int hw = ui_text_width(hint, S);
    ui_text(&scr, cx - hw / 2, py + fh + 18 * S, hint, message[0] ? 0xFFB4B4 : 0xC8CCE0, S);
    const char *tip = "Name >console for a text login";
    ui_text(&scr, 20 * S, H - 30 * S, tip, 0xA8ACC8, S);
    gfx_blit(0, 0, W, H, scr.px, W);
    gfx_present();
}

static void log_in(void) {
    struct os_user u;
    if (!strcmp(name, ">console")) text_login();
    if (auth(name, password, &u) != 0) {
        snprintf(message, sizeof(message), "Wrong name or password");
        memset(password, 0, sizeof(password));
        field = 1;
        return;
    }
    memset(password, 0, sizeof(password));
    printf("loginwindow: %s logged in\n", u.name);
    if (chdir(u.home) != 0) chdir("/");
    gfx_end();                                         /* the desktop takes the screen */
    char *argv[] = { "windowserver", "finder", 0 };
    int pid = spawn("windowserver", argv, u.name), status;
    if (pid > 0) wait(pid, &status, WAIT_FOREGROUND);
    exit(0);                                           /* launchd shows a new login */
}

int main(void) {
    if (gfx_info(&g) < 0 || !ui_has_icon("user")) text_login();
    W = g.width;
    H = g.height;
    S = g.cell_w >= 16 ? 2 : 1;
    unsigned *px = mmap((size_t)W * H * 4, PROT_READ | PROT_WRITE, -1);
    if (!px || gfx_begin_full() < 0) text_login();
    ui_canvas_init(&scr, px, W, H, W);
    ctrlc(CTRLC_KEY);
    printf("loginwindow: ready\n");
    unsigned long last = 0;
    int dirty = 1;
    for (;;) {
        int k;
        while ((k = getkey()) != 0) {
            char *buf = field ? password : name;
            int size = field ? (int)sizeof(password) : (int)sizeof(name), len = (int)strlen(buf);
            message[0] = 0;
            if (k == '\t') field = !field;
            else if (k == '\n' || k == '\r') {
                if (!strcmp(name, ">console")) log_in();
                else if (field == 0 && name[0]) field = 1;
                else if (field == 1) log_in();
            } else if (k == '\b') {
                if (len) buf[len - 1] = 0;
            } else if (k >= ' ' && k < 127 && len < size - 1) {
                buf[len] = (char)k;
                buf[len + 1] = 0;
            }
            dirty = 1;
        }
        if (uptime_ms() - last > 10000) dirty = 1;    /* the clock */
        if (dirty) {
            draw();
            last = uptime_ms();
            dirty = 0;
        }
        sleep_ms(10);
    }
}
