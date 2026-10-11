// settings [wifi|sound|battery|display|about] - System Settings: Wi-Fi
// networks, the volume, the battery, the screen and this computer.
#include <stdio.h>
#include <string.h>
#include <os.h>
#include <ui.h>

enum { P_WIFI, P_SOUND, P_BATTERY, P_DISPLAY, P_ABOUT, PANES };
static const char *pane_names[PANES] = { "Wi-Fi", "Sound", "Battery", "Display", "About" };
static const char *pane_icons[PANES] = { "wifi", "sound", "battery", "computer", "info" };

static ui_window win;
static int S, pane, SIDE;

/* Wi-Fi */
static struct os_wifi nets[24], now;
static int n_nets, connected, have_card = 1, picked = -1, typing;
static char password[64], wifi_msg[80];

/* Sound */
static int vol = -1, muted_at = -1, dragging;

static void button(int x, int y, int w, const char *label, int on) {
    ui_button(&win.c, x, y, w, 28 * S, label, S, on);
}

static void bars(int x, int y, int signal, unsigned color) {
    int n = signal >= -55 ? 4 : signal >= -65 ? 3 : signal >= -75 ? 2 : 1;
    for (int i = 0; i < 4; i++)
        ui_round(&win.c, x + i * 5 * S, y + (12 - 3 * i) * S, 3 * S, (3 + 3 * i) * S, S, i < n ? color : 0xD0D0D6);
}

static void refresh_wifi(void) {
    int r = wifi_status(&now);
    have_card = r != SYSERR_NOENT;
    connected = r == 1;
}

static void draw_wifi(int x, int y, int w) {
    ui_canvas *c = &win.c;
    if (!have_card) {
        ui_text(c, x, y, "No Wi-Fi card was found.", UI_DIM, S);
        return;
    }
    char line[96];
    if (connected) snprintf(line, sizeof(line), "Connected to %s   %s", now.name, now.address);
    else snprintf(line, sizeof(line), "Not connected");
    ui_text_bold(c, x, y, line, UI_TEXT, S);
    if (connected) bars(x + w - 30 * S, y, now.signal, UI_ACCENT);
    y += 30 * S;
    button(x, y, 110 * S, "Scan", 0);
    if (connected) button(x + 120 * S, y, 120 * S, "Disconnect", 0);
    y += 40 * S;
    if (wifi_msg[0]) {
        ui_text(c, x, y, wifi_msg, UI_DIM, S);
        y += 24 * S;
    }
    for (int i = 0; i < n_nets && y < win.c.h - 80 * S; i++, y += 30 * S) {
        if (i == picked) ui_round(c, x - 6 * S, y - 4 * S, w + 12 * S, 28 * S, 6 * S, UI_ACCENT);
        unsigned fg = i == picked ? UI_WHITE : UI_TEXT;
        bars(x, y + S, nets[i].signal, i == picked ? UI_WHITE : 0x606068);
        ui_text(c, x + 30 * S, y, nets[i].name, fg, S);
        if (nets[i].secured) ui_text(c, x + w - 60 * S, y, "secured", i == picked ? UI_WHITE : UI_DIM, S);
    }
    if (picked >= 0) {
        int py = win.c.h - 50 * S;
        if (nets[picked].secured) {
            ui_text(c, x, py + 6 * S, "Password", UI_TEXT, S);
            char dots[64];
            int n = (int)strlen(password);
            for (int i = 0; i < n && i < 63; i++) dots[i] = '*';
            dots[n < 63 ? n : 63] = 0;
            ui_textfield(c, x + 80 * S, py, w - 180 * S, 28 * S, dots, S, typing);
        }
        button(x + w - 90 * S, py, 90 * S, "Join", 0);
    }
}

static void slider(int x, int y, int w, int value) {
    ui_round(&win.c, x, y + 10 * S, w, 6 * S, 3 * S, 0xD8D8DE);
    ui_round(&win.c, x, y + 10 * S, w * value / 100, 6 * S, 3 * S, UI_ACCENT);
    int kx = x + w * value / 100;
    ui_shadow(&win.c, kx - 11 * S, y + 2 * S, 22 * S, 22 * S, 11 * S, 3 * S, 60);
    ui_circle(&win.c, kx, y + 13 * S, 11 * S, UI_WHITE);
}

static void draw_sound(int x, int y, int w) {
    ui_canvas *c = &win.c;
    if (vol < 0) {
        ui_text(c, x, y, "No sound device was found.", UI_DIM, S);
        return;
    }
    char line[32];
    snprintf(line, sizeof(line), "Output volume: %d%%", vol);
    ui_text_bold(c, x, y, line, UI_TEXT, S);
    slider(x, y + 30 * S, w - 20 * S, vol);
    button(x, y + 80 * S, 110 * S, vol ? "Mute" : "Unmute", 0);
    button(x + 120 * S, y + 80 * S, 110 * S, "Test", 0);
}

static void draw_battery(int x, int y, int w) {
    ui_canvas *c = &win.c;
    int b = battery();
    if (b < 0) {
        ui_text(c, x, y, "This computer has no battery.", UI_DIM, S);
        return;
    }
    int p = b & 0xFF;
    char line[48];
    snprintf(line, sizeof(line), "%d%%  %s", p, (b & 0x100) ? "charging" : "on battery");
    ui_text_style(c, x, y, line, UI_TEXT, 2 * S, UI_BOLD);
    int bw = w - 40 * S, bh = 40 * S, by = y + 60 * S;
    ui_round(c, x, by, bw, bh, 10 * S, 0xC8C8D0);
    ui_round(c, x + 3 * S, by + 3 * S, bw - 6 * S, bh - 6 * S, 8 * S, UI_WHITE);
    ui_round(c, x + 5 * S, by + 5 * S, (bw - 10 * S) * p / 100, bh - 10 * S, 6 * S,
             p <= 20 ? 0xFF5F57 : (b & 0x100) ? 0x28C840 : UI_ACCENT);
}

static void draw_display(int x, int y, int w) {
    struct os_gfx_info g;
    char line[64];
    gfx_info(&g);
    snprintf(line, sizeof(line), "%d x %d pixels", g.width, g.height);
    ui_text_style(&win.c, x, y, line, UI_TEXT, 2 * S, UI_BOLD);
    snprintf(line, sizeof(line), "Drawn at %dx for sharp text (Retina-style)", S);
    ui_text(&win.c, x, y + 50 * S, line, UI_DIM, S);
    ui_text(&win.c, x, y + 74 * S, "The native mode comes from the screen's EDID.", UI_DIM, S);
}

static void draw_about(int x, int y, int w) {
    struct os_uname u;
    struct os_meminfo m;
    struct os_user me;
    uname(&u);
    meminfo(&m);
    getuser(&me);
    char line[96];
    ui_icon(&win.c, x, y, 80 * S, "computer", 255);
    ui_text_style(&win.c, x + 100 * S, y + 6 * S, u.sysname, UI_TEXT, 2 * S, UI_BOLD);
    snprintf(line, sizeof(line), "Version %s (%s)", u.release, u.machine);
    ui_text(&win.c, x + 100 * S, y + 46 * S, line, UI_DIM, S);
    snprintf(line, sizeof(line), "Memory: %lu MiB", m.ram >> 20);
    ui_text(&win.c, x, y + 110 * S, line, UI_TEXT, S);
    snprintf(line, sizeof(line), "Computer name: %s", u.hostname);
    ui_text(&win.c, x, y + 134 * S, line, UI_TEXT, S);
    snprintf(line, sizeof(line), "You are %s%s", me.name, me.uid == 0 ? " (administrator)" : "");
    ui_text(&win.c, x, y + 158 * S, line, UI_TEXT, S);
}

static void draw(void) {
    ui_canvas *c = &win.c;
    ui_fill(c, 0, 0, c->w, c->h, UI_WHITE);
    ui_fill(c, 0, 0, SIDE, c->h, 0xF0F0F4);
    ui_fill(c, SIDE, 0, 1, c->h, 0xDADAE0);
    for (int i = 0; i < PANES; i++) {
        int y = 14 * S + i * 36 * S;
        if (i == pane) ui_round(c, 8 * S, y, SIDE - 16 * S, 32 * S, 7 * S, UI_ACCENT);
        ui_icon(c, 16 * S, y + 5 * S, 22 * S, pane_icons[i], 255);
        ui_text(c, 46 * S, y + 8 * S, pane_names[i], i == pane ? UI_WHITE : UI_TEXT, S);
    }
    int x = SIDE + 24 * S, y = 20 * S, w = c->w - x - 24 * S;
    ui_text_style(c, x, y, pane_names[pane], UI_TEXT, 2 * S, UI_BOLD);
    y += 50 * S;
    if (pane == P_WIFI) draw_wifi(x, y, w);
    else if (pane == P_SOUND) draw_sound(x, y, w);
    else if (pane == P_BATTERY) draw_battery(x, y, w);
    else if (pane == P_DISPLAY) draw_display(x, y, w);
    else draw_about(x, y, w);
    ui_present(&win);
}

static void scan(void) {
    snprintf(wifi_msg, sizeof(wifi_msg), "Looking for networks...");
    draw();
    n_nets = wifi_scan(nets, 24);
    if (n_nets < 0) {
        snprintf(wifi_msg, sizeof(wifi_msg), "Scanning failed: %s", os_strerror(n_nets));
        n_nets = 0;
    } else {
        snprintf(wifi_msg, sizeof(wifi_msg), n_nets ? "" : "No networks heard.");
    }
    picked = -1;
}

static void join(void) {
    snprintf(wifi_msg, sizeof(wifi_msg), "Joining %s...", nets[picked].name);
    draw();
    int r = wifi_connect(nets[picked].name, password);
    memset(password, 0, sizeof(password));
    refresh_wifi();
    snprintf(wifi_msg, sizeof(wifi_msg), r == 0 ? "Connected." : "Could not join: check the password.");
    picked = -1;
    typing = 0;
}

static void set_volume_at(int ex) {
    int x = SIDE + 24 * S, w = win.c.w - x - 24 * S - 20 * S;
    int v = (ex - x) * 100 / w;
    vol = volume(v < 0 ? 0 : v > 100 ? 100 : v);
}

static void click(int ex, int ey) {
    if (ex < SIDE) {
        int i = (ey - 14 * S) / (36 * S);
        if (i >= 0 && i < PANES) pane = i;
        return;
    }
    int x = SIDE + 24 * S, y = 70 * S, w = win.c.w - x - 24 * S;
    if (pane == P_WIFI && have_card) {
        int by = y + 30 * S;
        if (ui_inside(ex, ey, x, by, 110 * S, 28 * S)) {
            scan();
            return;
        }
        if (connected && ui_inside(ex, ey, x + 120 * S, by, 120 * S, 28 * S)) {
            wifi_disconnect();
            refresh_wifi();
            return;
        }
        int ly = by + 40 * S + (wifi_msg[0] ? 24 * S : 0);
        if (ey >= ly - 4 * S && ey < win.c.h - 80 * S) {
            int i = (ey - ly + 4 * S) / (30 * S);
            if (i < n_nets) {
                picked = i;
                typing = nets[i].secured;
                password[0] = 0;
            }
            return;
        }
        if (picked >= 0 && ui_inside(ex, ey, x + w - 90 * S, win.c.h - 50 * S, 90 * S, 28 * S)) join();
        else if (picked >= 0) typing = nets[picked].secured;
    } else if (pane == P_SOUND && vol >= 0) {
        if (ey >= y + 26 * S && ey < y + 60 * S) {
            dragging = 1;
            set_volume_at(ex);
        } else if (ui_inside(ex, ey, x, y + 80 * S, 110 * S, 28 * S)) {
            if (vol) {
                muted_at = vol;
                vol = volume(0);
            } else {
                vol = volume(muted_at > 0 ? muted_at : 50);
            }
        } else if (ui_inside(ex, ey, x + 120 * S, y + 80 * S, 110 * S, 28 * S)) {
            kcommand("beep");
        }
    }
}

int main(int argc, char **argv) {
    S = ui_scale();
    SIDE = 170 * S;
    for (int i = 0; argc > 1 && i < PANES; i++) {
        char lower[16];
        int k = 0;
        for (; pane_names[i][k] && k < 15; k++) {
            char ch = pane_names[i][k];
            lower[k] = ch >= 'A' && ch <= 'Z' ? (char)(ch + 32) : ch;
        }
        lower[k] = 0;
        if (!strcmp(argv[1], lower) || (!strcmp(argv[1], "wifi") && i == P_WIFI)) pane = i;
    }
    if (ui_open(&win, "System Settings", 660 * S, 440 * S) < 0) {
        printf("settings: the desktop is not running\n");
        return 1;
    }
    refresh_wifi();
    vol = volume(-1);
    if (vol < 0) vol = -1;
    draw();
    for (;;) {
        struct ws_event e;
        if (!ui_wait(&win, &e, 2000)) {                /* now and then: the battery, the network */
            refresh_wifi();
            draw();
            continue;
        }
        if (e.type == WS_EV_CLOSE) return 0;
        if (e.type == WS_EV_DOWN) click(e.x, e.y);
        else if (e.type == WS_EV_MOVE && dragging) set_volume_at(e.x);
        else if (e.type == WS_EV_UP) dragging = 0;
        else if (e.type == WS_EV_KEY && typing) {
            int n = (int)strlen(password);
            if (e.key == '\n' || e.key == '\r') join();
            else if (e.key == '\b' && n) password[n - 1] = 0;
            else if (e.key >= ' ' && e.key < 127 && n < (int)sizeof(password) - 1) {
                password[n] = (char)e.key;
                password[n + 1] = 0;
            }
        } else {
            continue;
        }
        draw();
    }
}
