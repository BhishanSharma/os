// loginwindow - DOOR OS starting up, and the graphical sign-in (like macOS's
// loginwindow).
//
// 1. The door. The kernel's boot screen (lib/print.c, splash_door) shows a door
//    standing slightly ajar with light behind it; the light grows with the boot
//    steps. Here the same door is drawn again and the camera moves in: the
//    light widens, fills the whole screen, and the Lobby (the desktop) appears
//    in it (windowserver --veil fades the light away).
// 2. The sign-in, only if the user turned a PIN or a password on (System
//    Settings > Sign-in). An account with an empty password has none: after the
//    door the Lobby opens straight away. With one account that has a PIN or
//    password the screen asks just for that (the name is known); with several
//    accounts it shows their profiles first, to choose from.
// 3. On the first start (no accounts yet) it is the Setup Assistant: just a
//    name. The account has no sign-in; Settings can add a PIN or password.
//
// Typing ">console" and Enter, in any of these screens, gives the text login
// instead (as on a Mac); so does a screen without graphics. Logging out of the
// desktop brings the profiles back (no door the second time).
#include <stdio.h>
#include <string.h>
#include <os.h>
#include <ui.h>

/* Colours: design/tokens.json (dark) and the door's light. */
#define C_BG       0x0B0E14
#define C_FRAME    0x232A36
#define C_EDGE     0x39424F
#define C_LEAF     0x2D3543
#define C_FLOOR    0x1A202A
#define C_LIGHT    0xFFE6BE           /* the light behind the door (and the veil's colour) */
#define C_TOP      0xFFF4DC
#define C_BOT      0xFFD9A0
#define C_TEXT     0xE8ECF2
#define C_TEXT2    0xAAB3C2
#define C_MUTED    0x8590A0
#define C_S1       0x1A202A
#define C_S2       0x232A36
#define C_S3       0x2D3543
#define C_ACCENT   0xA9B8D0
#define C_DANGER   0xE07A7A

#define K_UP     0x101
#define K_DOWN   0x102
#define K_LEFT   0x103
#define K_RIGHT  0x104

#define STEPS 22                       /* the kernel's boot steps: the door as it left it */
#define MAX_ACCOUNTS 16

static struct os_gfx_info g;
static ui_canvas scr;
static unsigned *px, *bgc;             /* the frame, and the sign-in's background */
static unsigned short *glowbuf;        /* the light's strength at a quarter of the resolution */
static int W, H, S, U;                 /* U: a twentieth of the screen's height (the door's unit) */

/* ---- Small helpers --------------------------------------------------------- */

static int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

static unsigned mix(unsigned a, unsigned b, int t) {                     /* t 0..255 */
    int r = (int)(a >> 16 & 255) + ((int)(b >> 16 & 255) - (int)(a >> 16 & 255)) * t / 255;
    int gr = (int)(a >> 8 & 255) + ((int)(b >> 8 & 255) - (int)(a >> 8 & 255)) * t / 255;
    int bl = (int)(a & 255) + ((int)(b & 255) - (int)(a & 255)) * t / 255;
    return (unsigned)(r << 16 | gr << 8 | bl);
}

static long isq(long v) {                                                /* integer square root */
    if (v <= 0) return 0;
    long x = v, y = (x + 1) / 2;
    while (y < x) {
        x = y;
        y = (x + v / x) / 2;
    }
    return x;
}

static int smooth(int u) {                                               /* 0..256 -> 0..256, eased both ends */
    u = clampi(u, 0, 256);
    return u * u * (768 - 2 * u) / (256 * 256);
}

static void text_login(void) {
    gfx_end();
    char *argv[] = { "login", 0 };
    exec("login", argv);
    printf("loginwindow: cannot start login\n");
    exit(1);
}

static void present(void) {
    gfx_blit(0, 0, W, H, px, W);
    if (gfx_present() <= 0) sleep_ms(8);
}

/* ---- The light --------------------------------------------------------------
 * A soft glow round a rectangle, computed at a quarter of the resolution and
 * enlarged with bilinear sampling: the light has no hard edge and costs little. */

static unsigned ramp[256];                                               /* 0 = the room, 255 = the light */

static void build_ramp(unsigned from, unsigned to) {
    for (int i = 0; i < 256; i++) ramp[i] = mix(from, to, i);
}

/* Fills the whole frame: C_BG plus the light round (x0,y0)-(x1,y1), `amp` (0..255) at
 * its edge, falling off over about `w` pixels. */
static void glow_frame(unsigned *dst, int x0, int y0, int x1, int y1, int amp, int w) {
    int gw = W / 4 + 2, gh = H / 4 + 2, any = 0;
    long ww = (long)w * w;
    for (int gy = 0; gy < gh; gy++)
        for (int gx = 0; gx < gw; gx++) {
            int x = gx * 4, y = gy * 4;
            long dx = x < x0 ? x0 - x : x > x1 ? x - x1 : 0, dy = y < y0 ? y0 - y : y > y1 ? y - y1 : 0;
            long d2 = dx * dx + dy * dy;
            int v = (int)((long)amp * ww / (ww + d2));
            glowbuf[gy * gw + gx] = (unsigned short)(v > 255 ? 255 : v);
            if (v) any = 1;
        }
    if (!any) {
        for (long i = 0; i < (long)W * H; i++) dst[i] = C_BG;
        return;
    }
    for (int gy = 0; gy + 1 < gh; gy++) {
        int ya = gy * 4, yb = ya + 4 > H ? H : ya + 4;
        for (int gx = 0; gx + 1 < gw; gx++) {
            int xa = gx * 4, xb = xa + 4 > W ? W : xa + 4;
            int a = glowbuf[gy * gw + gx], b = glowbuf[gy * gw + gx + 1];
            int c = glowbuf[(gy + 1) * gw + gx], d = glowbuf[(gy + 1) * gw + gx + 1];
            for (int y = ya; y < yb; y++) {
                int fy = y - ya, l4 = (4 - fy) * a + fy * c, r4 = (4 - fy) * b + fy * d;
                unsigned *row = dst + (long)y * W;
                for (int x = xa; x < xb; x++) {
                    int fx = x - xa;
                    row[x] = ramp[((4 - fx) * l4 + fx * r4) >> 4];
                }
            }
        }
    }
}

/* ---- The door ---------------------------------------------------------------
 * The same door as the kernel's boot screen (lib/print.c splash_door): the frame
 * 6U x 10U, its top 6U above the middle, the leaf hinged on the left and the
 * light in a slit at the right. A pose is how far it is opened (ajar: the slit as
 * a part of the doorway, in 1/256ths), the zoom (256 = as the kernel drew it),
 * the pan (0: round the slit, 256: round the doorway's middle, which is then the
 * screen's), the glow and a flat veil of light over it all. */

static long qx, qy, rx, ry, zoom;                                        /* screen = q + (world - r) * zoom */

static int tx(long x) { return (int)(qx + (x - rx) * zoom / 256); }
static int ty(long y) { return (int)(qy + (y - ry) * zoom / 256); }

static void wrect(long x, long y, long w, long h, unsigned color) {
    int a = tx(x), b = ty(y), c = tx(x + w), d = ty(y + h);
    if (c <= a) c = a + 1;
    if (d <= b) d = b + 1;
    a = clampi(a, 0, W);
    c = clampi(c, 0, W);
    b = clampi(b, 0, H);
    d = clampi(d, 0, H);
    if (c > a && d > b) ui_fill(&scr, a, b, c - a, d - b, color);
}

static void door_scene(int ajar, int z, int pan, int halo, int veil) {
    long u = U, t = u / 4 + 1, cx = W / 2;
    long fx = cx - 3 * u, fy = H / 2 - 6 * u, fw = 6 * u, fh = 10 * u;
    long ix = fx + t, iy = fy + t, iw = fw - 2 * t, ih = fh - t;         /* the doorway */
    long sw = iw * ajar / 256;
    if (sw < 2) sw = 2;
    long lw = iw - sw;                                                   /* the leaf */
    long slit_cx = ix + lw + sw / 2, slit_cy = iy + ih / 2;
    long door_cx = ix + iw / 2, door_cy = iy + ih / 2;
    zoom = z;
    rx = slit_cx + (door_cx - slit_cx) * pan / 256;
    ry = slit_cy + (door_cy - slit_cy) * pan / 256;
    qx = slit_cx + (W / 2 - slit_cx) * pan / 256;
    qy = slit_cy + (H / 2 - slit_cy) * pan / 256;

    int sx0 = tx(ix + lw), sx1 = tx(ix + iw), sy0 = ty(iy), sy1 = ty(iy + ih);
    glow_frame(px, sx0, sy0, sx1, sy1, halo * 190 / 256, (int)(u * 3 / 2 * z / 256 + (long)(sx1 - sx0)));

    wrect(0, fy + fh, W * 4L, 1, C_FLOOR);                               /* the floor line */
    wrect(fx - t, fy - t, fw + 2 * t, fh + t, C_EDGE);       /* the frame, an edge round it */
    wrect(fx, fy, fw, fh, C_FRAME);
    wrect(ix, iy, lw, ih, C_LEAF);                                       /* the leaf, its two panels */
    long pm = u / 3;
    if (lw > 3 * pm) {
        wrect(ix + pm, iy + u / 2, lw - 2 * pm, 3 * u, C_FRAME);
        wrect(ix + pm, iy + 4 * u, lw - 2 * pm, 5 * u - u / 2, C_FRAME);
        wrect(ix + pm + 1, iy + u / 2 + 1, lw - 2 * pm - 2, 3 * u - 2, C_LEAF - 0x040404);
        wrect(ix + pm + 1, iy + 4 * u + 1, lw - 2 * pm - 2, 5 * u - u / 2 - 2, C_LEAF - 0x040404);
        wrect(ix + lw - u / 2 - t, iy + 5 * u, t + 1, t + 1, C_ACCENT);   /* the knob */
    }
    int y0 = ty(iy), y1 = ty(iy + ih), x0 = tx(ix + lw), x1 = tx(ix + iw);   /* the light in the slit */
    if (x1 <= x0) x1 = x0 + 1;
    if (y1 > y0)
        for (int y = clampi(y0, 0, H); y < clampi(y1, 0, H); y++)
            ui_fill(&scr, clampi(x0, 0, W), y, clampi(x1, 0, W) - clampi(x0, 0, W), 1,
                    mix(C_TOP, C_BOT, (y - y0) * 255 / (y1 - y0)));
    wrect(ix + lw - 1, iy, 1, ih, mix(C_LEAF, C_LIGHT, 127));            /* the leaf's lit edge */
    long sh = 3 * u, sc = slit_cx, fy2 = fy + fh + 1;                    /* the light on the floor */
    for (long k = 0; k < sh; k++) {
        int q = (int)(255 - k * 255 / sh), a = q * q / 255 * 200 / 255;
        long half = sw / 2 + k * 3 / 2;
        int ax = clampi(tx(sc - half), 0, W), bx = clampi(tx(sc + half), 0, W), yy = ty(fy2 + k), hh = ty(fy2 + k + 1) - yy;
        if (hh < 1) hh = 1;
        if (bx > ax && yy < H && yy + hh > 0) ui_fill_a(&scr, ax, clampi(yy, 0, H), bx - ax, clampi(yy + hh, 0, H) - clampi(yy, 0, H), C_LIGHT, a);   /* (over the glow) */
    }
    if (veil > 0) ui_fill_a(&scr, 0, 0, W, H, C_LIGHT, veil > 255 ? 255 : veil);
}

#define T_GLOW 700                      /* the glow comes up (ms) */
#define T_ZOOM 2400                     /* then the camera moves in */
#define T_HOLD 150                      /* all light */

static int pend[64], npend;             /* keys typed during the door, for the screen after it */

/* The door opens and its light fills the screen. Any key skips ahead. */
static void play_door(void) {
    build_ramp(C_BG, C_LIGHT);
    unsigned long t0 = uptime_ms();
    for (;;) {
        int k;
        while ((k = getkey()) != 0)
            if (npend < 64) pend[npend++] = k;
        long t = (long)(uptime_ms() - t0);
        if (npend && t < T_GLOW + T_ZOOM) t = T_GLOW + T_ZOOM;           /* a key: straight to the light */
        int halo = smooth((int)(t * 256 / T_GLOW));
        int u = (int)clampi((int)((t - T_GLOW) * 256 / T_ZOOM), 0, 256);
        int e = u * u / 256;                                             /* slow at first, then faster */
        int z = 256 + 2816 * e / 256;
        int ajar = 26 + (250 - 26) * smooth(u) / 256;
        int pan = smooth(u);
        int veil = u < 190 ? 0 : (u - 190) * 255 / 66;
        door_scene(ajar, z, pan, halo, veil);
        present();
        if (t >= T_GLOW + T_ZOOM + T_HOLD) break;
    }
    ui_fill(&scr, 0, 0, W, H, C_LIGHT);                                  /* exactly the veil's colour */
    present();
}

/* ---- The sign-in's picture ------------------------------------------------- */

static void make_background(void) {
    build_ramp(C_BG, C_LIGHT);                                           /* light from under the door, low */
    glow_frame(bgc, W / 2 - 4 * U, H + U, W / 2 + 4 * U, H + 2 * U, 52, H / 2);
}

/* ---- Accounts --------------------------------------------------------------- */

static struct os_account acct[MAX_ACCOUNTS];
static int nacct, pin_mode[MAX_ACCOUNTS];

/* A PIN rather than a password: SIGNIN.CFG in the home folder says "pin" (System Settings). */
static int uses_pin(const char *name, unsigned uid) {
    if (!uid) return 0;
    char path[64], buf[16] = { 0 };
    int n = snprintf(path, sizeof(path), "/HOME/");
    for (const char *p = name; *p && n < 40; p++) path[n++] = (*p >= 'a' && *p <= 'z') ? (char)(*p - 32) : *p;
    snprintf(path + n, sizeof(path) - (size_t)n, "/SIGNIN.CFG");
    int fd = open(path, OPEN_READ);
    if (fd < 0) return 0;
    long r = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    return r >= 3 && !strncmp(buf, "pin", 3);
}

static void load_accounts(void) {
    nacct = accounts(acct, MAX_ACCOUNTS);
    if (nacct < 0) nacct = 0;
    for (int i = 0; i < nacct; i++) pin_mode[i] = acct[i].has_password && uses_pin(acct[i].name, acct[i].uid);
}

/* ---- The screens ------------------------------------------------------------ */

enum { ST_SETUP, ST_PICK, ST_PASS };
static int stage, sel, chosen;
static char name_buf[16], pw[64], cmd[16], message[80];
static int light, fading;               /* the veil over the screen, 0..255 (coming out of the door's light) */
static void route(void);

static int weekday(int y, int m, int d) {                                /* 0 = Sunday */
    static const int tt[] = { 0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4 };
    if (m < 3) y--;
    return (y + y / 4 - y / 100 + y / 400 + tt[m - 1] + d) % 7;
}

static void centered(int y, const char *s, unsigned color, int scale, int bold) {
    int w = bold ? ui_text_style_width(s, scale, UI_BOLD) : ui_text_width(s, scale);
    if (bold) ui_text_bold(&scr, (W - w) / 2, y, s, color, scale);
    else ui_text(&scr, (W - w) / 2, y, s, color, scale);
}

static void avatar(int cx, int cy, int d, const char *name, int selected) {
    if (selected) ui_circle(&scr, cx, cy, d / 2 + 4 * S, C_ACCENT);
    ui_circle(&scr, cx, cy, d / 2 + (selected ? 1 * S : 0), C_S1);
    ui_circle(&scr, cx, cy, d / 2 - (selected ? 1 * S : 0), selected ? C_S3 : C_S2);
    char ini[2] = { name[0] >= 'a' && name[0] <= 'z' ? (char)(name[0] - 32) : name[0], 0 };
    int sc = d >= 100 * S ? 4 : 3;
    int w = ui_text_style_width(ini, sc, UI_BOLD);
    ui_text_bold(&scr, cx - w / 2, cy - 8 * sc, ini, selected ? C_TEXT : C_TEXT2, sc);
}

static void field(int y, int w, int dots, const char *placeholder, int show_text, const char *text) {
    int h = 40 * S, x = (W - w) / 2;
    ui_round_ex(&scr, x - S, y - S, w + 2 * S, h + 2 * S, h / 2 + S, C_S3, 255, UI_ALL);
    ui_round_ex(&scr, x, y, w, h, h / 2, C_S2, 255, UI_ALL);
    int ty = y + (h - 16 * S) / 2, cx = x + 20 * S;
    if (show_text) {
        if (text[0]) cx += ui_text(&scr, cx, ty, text, C_TEXT, S);
        else ui_text(&scr, cx, ty, placeholder, C_MUTED, S);
        ui_fill(&scr, cx + S, y + 10 * S, S, 20 * S, C_TEXT);
        return;
    }
    if (!dots) ui_text(&scr, cx, ty, placeholder, C_MUTED, S);
    for (int k = 0; k < dots && k < 18; k++, cx += 14 * S) ui_circle(&scr, cx + 4 * S, y + h / 2, 4 * S, C_TEXT);
    ui_fill(&scr, cx - 2 * S + S, y + 10 * S, S, 20 * S, C_TEXT);
}

static void draw(void) {
    ui_noclip(&scr);
    for (long i = 0; i < (long)W * H; i++) px[i] = bgc[i];

    struct os_time tm;                                                   /* a clock, like a lock screen */
    gettime(&tm);
    char clock[8], date[32];
    static const char *days[] = { "Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday" };
    static const char *months[] = { "January", "February", "March", "April", "May", "June", "July", "August",
                                    "September", "October", "November", "December" };
    snprintf(clock, sizeof(clock), "%d:%02d", tm.hour, tm.minute);
    snprintf(date, sizeof(date), "%s, %d %s", days[weekday(tm.year, tm.month, tm.day)], tm.day,
             months[clampi(tm.month, 1, 12) - 1]);
    centered(H / 12, clock, C_TEXT, 4 * S, 1);
    centered(H / 12 + 76 * S, date, C_TEXT2, S, 0);

    int cy = H / 2 - 20 * S;
    const char *hint = "";
    if (stage == ST_SETUP) {
        centered(cy - 150 * S, "Welcome to DOOR OS", C_TEXT, 2 * S, 1);
        centered(cy - 100 * S, "What should we call you?", C_TEXT2, S, 0);
        avatar(W / 2, cy - 10 * S, 96 * S, name_buf[0] ? name_buf : "?", 1);
        field(cy + 60 * S, 280 * S, 0, "Account name (a-z, 0-9)", 1, name_buf);
        hint = "Enter to continue. A PIN or password can be turned on later in Settings > Sign-in.";
    } else if (stage == ST_PICK) {
        int dia = 96 * S, gap = 56 * S, total = nacct * dia + (nacct - 1) * gap, x = (W - total) / 2 + dia / 2;
        centered(cy - 110 * S, "Who is coming in?", C_TEXT2, S, 0);
        for (int i = 0; i < nacct; i++, x += dia + gap) {
            avatar(x, cy, dia, acct[i].name, i == sel);
            int w = ui_text_style_width(acct[i].name, S, i == sel ? UI_BOLD : UI_REGULAR);
            if (i == sel) ui_text_bold(&scr, x - w / 2, cy + dia / 2 + 20 * S, acct[i].name, C_TEXT, S);
            else ui_text(&scr, x - w / 2, cy + dia / 2 + 20 * S, acct[i].name, C_TEXT2, S);
            if (acct[i].has_password) {
                static const char *lock = "PIN or password";
                if (i == sel) ui_text(&scr, x - ui_text_width(lock, S) / 2, cy + dia / 2 + 44 * S, lock, C_MUTED, S);
            }
        }
        hint = nacct > 1 ? "Left / Right to choose, Enter to continue" : "Enter to continue";
    } else {
        const char *who = acct[chosen].name;
        avatar(W / 2, cy - 20 * S, 110 * S, who, 1);
        centered(cy + 56 * S, who, C_TEXT, S, 1);
        int dots = (int)strlen(pw);
        field(cy + 90 * S, 280 * S, dots, pin_mode[chosen] ? "PIN" : "Password", 0, "");
        hint = nacct > 1 ? "Enter to sign in. Esc to choose another profile." : "Enter to sign in";
    }
    int ms = message[0];
    centered(cy + 150 * S, ms ? message : hint, ms ? C_DANGER : C_MUTED, S, 0);
    centered(H - 40 * S, "DOOR OS", C_MUTED, S, 1);
    if (light > 0) ui_fill_a(&scr, 0, 0, W, H, C_LIGHT, light > 255 ? 255 : light);
    present();
}

/* ---- Into the Lobby ----------------------------------------------------------- */

/* The screen fills with the light of the door (when it is not full of it already). */
static void flood(void) {
    unsigned long t0 = uptime_ms();
    for (;;) {
        int p = (int)((uptime_ms() - t0) * 256 / 380);
        if (p > 256) p = 256;
        light = p * 255 / 256;
        draw();
        if (p >= 256) break;
    }
    ui_fill(&scr, 0, 0, W, H, C_LIGHT);
    present();
}

static void lobby(const struct os_user *u, int full_light) {
    printf("loginwindow: %s logged in\n", u->name);
    if (chdir(u->home) != 0) chdir("/");
    if (!full_light) flood();
    gfx_release();                                     /* the desktop takes the screen as it is: light */
    char *argv[] = { "windowserver", "--veil", 0 };
    int pid = spawn("windowserver", argv, u->name), status;
    if (pid > 0) wait(pid, &status, WAIT_FOREGROUND);
    /* Logged out (or the desktop ended): the profiles again. */
    chdir("/");
    if (gfx_begin_full() < 0) exit(0);                 /* (launchd starts a fresh login) */
    memset(pw, 0, sizeof(pw));
    message[0] = 0;
    light = 0;
    fading = 0;
    load_accounts();
    sel = 0;
    route();
}

static void sign_in(int i, const char *password) {
    struct os_user u;
    if (auth(acct[i].name, password, &u) != 0) {
        snprintf(message, sizeof(message), pin_mode[i] ? "Wrong PIN" : "Wrong password");
        memset(pw, 0, sizeof(pw));
        return;
    }
    memset(pw, 0, sizeof(pw));
    lobby(&u, 0);
}

static void pick(int i) {
    chosen = i;
    memset(pw, 0, sizeof(pw));
    message[0] = 0;
    if (!acct[i].has_password) sign_in(i, "");         /* no PIN or password turned on: straight in */
    else stage = ST_PASS;
}

static void route(void) {                              /* which screen the accounts call for */
    if (nacct == 1 && acct[0].has_password) {
        chosen = 0;
        stage = ST_PASS;
    } else {
        stage = ST_PICK;
    }
}

static void create_account(void) {
    int r = setup_accounts(name_buf, "");
    if (r == -2) {
        snprintf(message, sizeof(message), "1-8 lower-case letters or digits, starting with a letter");
        return;
    }
    if (r != 0) {
        snprintf(message, sizeof(message), "Could not create the account");
        return;
    }
    printf("loginwindow: created %s\n", name_buf);
    load_accounts();
    for (int i = 0; i < nacct; i++)
        if (!strcmp(acct[i].name, name_buf)) {
            chosen = i;
            sign_in(i, "");
            return;
        }
    stage = ST_PICK;
}

static void key_typed(int k) {
    char *buf = stage == ST_SETUP ? name_buf : stage == ST_PASS ? pw : cmd;
    int size = stage == ST_SETUP ? (int)sizeof(name_buf) : stage == ST_PASS ? (int)sizeof(pw) : (int)sizeof(cmd);
    int len = (int)strlen(buf);
    message[0] = 0;
    if (k == '\n' || k == '\r') {
        if (!strcmp(buf, ">console")) text_login();
        if (stage == ST_SETUP) {
            if (len) create_account();
        } else if (stage == ST_PASS) {
            sign_in(chosen, pw);
        } else {
            cmd[0] = 0;
            pick(sel);
        }
    } else if (k == K_LEFT && stage == ST_PICK) {
        if (sel > 0) sel--;
    } else if (k == K_RIGHT && stage == ST_PICK) {
        if (sel < nacct - 1) sel++;
    } else if (k == '\t' && stage == ST_PICK) {
        sel = (sel + 1) % nacct;
    } else if (k == 27 && stage == ST_PASS && nacct > 1) {
        memset(pw, 0, sizeof(pw));
        stage = ST_PICK;
    } else if (k == '\b') {
        if (len) buf[len - 1] = 0;
    } else if (k >= ' ' && k < 127 && len < size - 1) {
        if (stage == ST_PASS && pin_mode[chosen] && (k < '0' || k > '9') && pw[0] != '>' && !(len == 0 && k == '>'))
            return;                                    /* a PIN is digits (a first ">" starts ">console") */
        buf[len] = (char)k;
        buf[len + 1] = 0;
    }
}

int main(void) {
    if (gfx_info(&g) < 0) text_login();
    W = g.width;
    H = g.height;
    S = (H >= 1600 && W >= 2560) ? 2 : 1;              /* the same rule as the desktop */
    U = H / 20;
    px = mmap((size_t)W * H * 4, PROT_READ | PROT_WRITE, -1);
    bgc = mmap((size_t)W * H * 4, PROT_READ | PROT_WRITE, -1);
    glowbuf = mmap((size_t)(W / 4 + 2) * (H / 4 + 2) * 2, PROT_READ | PROT_WRITE, -1);
    if (!px || !bgc || !glowbuf || gfx_begin_keep() < 0) text_login();   /* (the boot screen's door stays) */
    ui_canvas_init(&scr, px, W, H, W);
    ctrlc(CTRLC_KEY);
    int setup = setup_needed() == 1;
    if (setup) printf("loginwindow: setup\n");
    load_accounts();
    printf("loginwindow: ready\n");

    play_door();                                       /* the door opens, the light fills the screen */
    make_background();
    if (setup) {
        stage = ST_SETUP;
    } else {
        route();
    }
    light = 255;                                       /* the sign-in comes out of the light */
    fading = 1;
    if (!setup && nacct == 1 && !acct[0].has_password && !(npend && pend[0] == '>')) {
        struct os_user u;                              /* no PIN or password turned on: the Lobby, in the light */
        if (auth(acct[0].name, "", &u) == 0) {
            npend = 0;
            lobby(&u, 1);                              /* (returns after logging out: the profiles) */
        }
    }
    unsigned long fade0 = uptime_ms(), last = 0;
    int dirty = 1;
    for (int i = 0; i < npend; i++) key_typed(pend[i]);
    npend = 0;
    for (;;) {
        int k;
        while ((k = getkey()) != 0) {
            key_typed(k);
            dirty = 1;
        }
        if (fading) {                                  /* the light fading away */
            int p = (int)((uptime_ms() - fade0) * 256 / 450);
            light = p >= 256 ? 0 : 255 * (256 - p) * (256 - p) / 65536;
            if (!light) fading = 0;
            dirty = 1;
        }
        if (uptime_ms() - last > 10000) dirty = 1;     /* the clock */
        if (dirty) {
            draw();
            last = uptime_ms();
            dirty = 0;
        } else {
            sleep_ms(10);
        }
    }
}