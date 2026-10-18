// loginwindow - the graphical login (like macOS's loginwindow): a picture,
// the user's name and password, then the desktop as that user. Logging out
// of the desktop ends it, and launchd starts a fresh login screen.
//
// On the first start (no accounts yet) it is the Setup Assistant instead:
// a name and a password, typed twice, make the everyday account and root
// (the administrator) with that password, and the desktop opens.
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
static char fields[3][64], message[64];         /* name, password, password again */
static int field, nfields = 2, setup;

static void text_login(void) {
    gfx_end();
    char *argv[] = { "login", 0 };
    exec("login", argv);
    printf("loginwindow: cannot start login\n");
    exit(1);
}

static void draw_field(int x, int y, int w, int h, int i, const char *placeholder) {
    ui_round_ex(&scr, x, y, w, h, h / 2, 0xFFFFFF, field == i ? 120 : 70, UI_ALL);
    const char *text = fields[i];
    int n = (int)strlen(text), ty = y + (h - 16 * S) / 2;
    if (!n) ui_text(&scr, x + 16 * S, ty, placeholder, 0xD0D4E8, S);
    if (i == 0) {
        if (n) ui_text(&scr, x + 16 * S, ty, text, 0xFFFFFF, S);
        if (field == i) ui_fill(&scr, x + 16 * S + ui_text_width(text, S) + S, y + 9 * S, S, 16 * S, 0xFFFFFF);
        return;
    }
    int dx = x + 18 * S;                               /* a password: dots */
    for (int k = 0; k < n && k < 24; k++, dx += 12 * S) ui_circle(&scr, dx, y + h / 2, 4 * S, 0xFFFFFF);
    if (field == i) ui_fill(&scr, dx - 4 * S + S, y + 9 * S, S, 16 * S, 0xFFFFFF);
}

static void draw(void) {
    ui_noclip(&scr);
    for (int y = 0; y < H; y++) {                      /* the desktop's evening sky, darker */
        int t = (y * 256) / H;
        int r = 0x1A + ((0x6A - 0x1A) * t >> 8), gr = 0x36 + ((0x40 - 0x36) * t >> 8), b = 0x9A + ((0xA0 - 0x9A) * t >> 8);
        ui_fill(&scr, 0, y, W, 1, (unsigned)(r << 16 | gr << 8 | b));
    }
    int cx = W / 2, fw = 260 * S, fh = 34 * S, fx = cx - fw / 2;
    int top = H / 2 - (setup ? 170 : 120) * S;
    if (setup) {                                       /* Setup Assistant */
        const char *hello = "Welcome to Terminal OS";
        int hw = ui_text_style_width(hello, 2 * S, UI_BOLD);
        ui_text_bold(&scr, cx - hw / 2, H / 7, hello, 0xFFFFFF, 2 * S);
        const char *sub = "Create your account to get started";
        ui_text(&scr, cx - ui_text_width(sub, S) / 2, H / 7 + 46 * S, sub, 0xD8DCF0, S);
    } else {
        struct os_time t;
        gettime(&t);
        char clock[8];
        snprintf(clock, sizeof(clock), "%d:%02d", t.hour, t.minute);
        int cw = ui_text_style_width(clock, 4, UI_BOLD);
        ui_text_bold(&scr, (W - cw) / 2, H / 7, clock, 0xFFFFFF, 4);   /* a big clock, like a lock screen */
    }

    int av = 110 * S;                                  /* the avatar */
    ui_circle(&scr, cx, top + av / 2, av / 2 + 3 * S, 0xFFFFFF);
    ui_circle(&scr, cx, top + av / 2, av / 2, 0xD8DCE8);
    ui_icon(&scr, cx - av * 35 / 100, top + av * 15 / 100, av * 70 / 100, "user", 255);

    int fy = top + av + 26 * S;
    const char *title = fields[0][0] ? fields[0] : setup ? "New account" : "Terminal OS";
    int tw = ui_text_style_width(title, S, UI_BOLD);
    ui_text_bold(&scr, cx - tw / 2, fy - 20 * S, title, 0xFFFFFF, S);
    fy += 8 * S;
    static const char *holders[2][3] = { { "Name", "Password", "" },
                                         { "Account name (a-z, 0-9)", "Password", "Verify password" } };
    for (int i = 0; i < nfields; i++, fy += fh + 12 * S) draw_field(fx, fy, fw, fh, i, holders[setup][i]);

    const char *hint = message[0] ? message : "Enter: next   Tab: switch fields";
    int hw = ui_text_width(hint, S);
    ui_text(&scr, cx - hw / 2, fy + 6 * S, hint, message[0] ? 0xFFB4B4 : 0xC8CCE0, S);
    const char *tip = setup ? "This account is also the administrator (root) password"
                            : "Name >console for a text login";
    ui_text(&scr, 20 * S, H - 30 * S, tip, 0xA8ACC8, S);
    gfx_blit(0, 0, W, H, scr.px, W);
    gfx_present();
}

static void desktop(const struct os_user *u) {
    printf("loginwindow: %s logged in\n", u->name);
    if (chdir(u->home) != 0) chdir("/");
    gfx_end();                                         /* the desktop takes the screen */
    char *argv[] = { "windowserver", 0 };             /* the desktop, no app opened */
    int pid = spawn("windowserver", argv, u->name), status;
    if (pid > 0) wait(pid, &status, WAIT_FOREGROUND);
    exit(0);                                           /* launchd shows a new login */
}

static void clear_passwords(void) {
    memset(fields[1], 0, sizeof(fields[1]));
    memset(fields[2], 0, sizeof(fields[2]));
}

static void log_in(void) {
    struct os_user u;
    if (auth(fields[0], fields[1], &u) != 0) {
        snprintf(message, sizeof(message), "Wrong name or password");
        clear_passwords();
        field = 1;
        return;
    }
    clear_passwords();
    desktop(&u);
}

static void create_account(void) {
    if (strcmp(fields[1], fields[2])) {
        snprintf(message, sizeof(message), "The passwords do not match");
        clear_passwords();
        field = 1;
        return;
    }
    int r = setup_accounts(fields[0], fields[1]);
    if (r == -2) {
        snprintf(message, sizeof(message), "Name: 1-8 lower-case letters or digits");
        field = 0;
        return;
    }
    if (r == -3) {
        snprintf(message, sizeof(message), "Choose a password");
        field = 1;
        return;
    }
    printf("loginwindow: created %s\n", fields[0]);
    setup = 0;
    nfields = 2;
    log_in();
}

static void enter(void) {
    if (!strcmp(fields[0], ">console")) text_login();
    if (field == 0 && !fields[0][0]) return;
    if (field < nfields - 1) field++;
    else if (setup) create_account();
    else log_in();
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
    if (setup_needed() == 1) {
        setup = 1;
        nfields = 3;
        printf("loginwindow: setup\n");
    }
    printf("loginwindow: ready\n");
    unsigned long last = 0;
    int dirty = 1;
    for (;;) {
        int k;
        while ((k = getkey()) != 0) {
            char *buf = fields[field];
            int size = field ? (int)sizeof(fields[1]) : 16, len = (int)strlen(buf);
            message[0] = 0;
            if (k == '\t') field = (field + 1) % nfields;
            else if (k == '\n' || k == '\r') enter();
            else if (k == '\b') {
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
