// settings [wifi|sound|battery|display|about] - System Settings: Wi-Fi
// networks, the volume, the battery, the screen and this computer.
//
// Built from views (view.h): a sidebar table of panes, and one box per pane
// of which only the chosen one is shown. Every two seconds the state is read
// again; only the views whose text or value changed are drawn.
#include <stdio.h>
#include <string.h>
#include <os.h>
#include <view.h>

enum { P_WIFI, P_SOUND, P_BATTERY, P_DISPLAY, P_ABOUT, PANES };
static const char *pane_names[PANES] = { "Wi-Fi", "Sound", "Battery", "Display", "About" };
static const char *pane_icons[PANES] = { "wifi", "sound", "battery", "computer", "info" };

static int S, pane;
static view_t *sidebar, *title, *panes[PANES];

/* Wi-Fi */
static struct os_wifi nets[24], now;
static int n_nets, connected, have_card = 1, picked = -1;
static view_t *no_card, *wifi_box, *status, *signal_lbl, *disconnect, *message, *networks, *join_row, *password;

/* Sound */
static int vol = -1, muted_at = -1;
static view_t *no_sound, *sound_box, *vol_label, *slider, *mute;

/* Battery */
static int bat = -1;
static view_t *bat_label, *bat_bar;

static const char *strength(int signal) {
    return signal >= -55 ? "Excellent" : signal >= -65 ? "Good" : signal >= -75 ? "Fair" : "Weak";
}

/* ---- Wi-Fi ----------------------------------------------------------------- */

static void show_wifi(void) {
    v_show(no_card, !have_card);
    v_show(wifi_box, have_card);
    char line[96];
    if (connected) snprintf(line, sizeof(line), "Connected to %s   %s", now.name, now.address);
    else snprintf(line, sizeof(line), "Not connected");
    v_set_text(status, line);
    v_set_text(signal_lbl, connected ? strength(now.signal) : "");
    v_show(disconnect, connected);
    v_table_rows(networks, n_nets);
    v_table_select(networks, picked);
    v_show(join_row, picked >= 0);
    v_show(password, picked >= 0 && nets[picked].secured);
}

static void refresh_wifi(void) {
    int r = wifi_status(&now);
    have_card = r != SYSERR_NOENT;
    connected = r == 1;
}

static void net_cell(view_t *t, int row, int col, char *out, int max) {
    (void)t;
    if (col == 0) snprintf(out, max, "%s", nets[row].name);
    else if (col == 1) snprintf(out, max, "%s", strength(nets[row].signal));
    else snprintf(out, max, "%s", nets[row].secured ? "Secured" : "Open");
}

static void scan(view_t *b) {
    (void)b;
    v_set_text(message, "Looking for networks...");
    v_update();                                        /* (the scan takes a few seconds) */
    n_nets = wifi_scan(nets, 24);
    if (n_nets < 0) {
        char line[80];
        snprintf(line, sizeof(line), "Scanning failed: %s", os_strerror(n_nets));
        v_set_text(message, line);
        n_nets = 0;
    } else {
        v_set_text(message, n_nets ? "" : "No networks heard.");
    }
    picked = -1;
    show_wifi();
}

static void disconnect_now(view_t *b) {
    (void)b;
    wifi_disconnect();
    refresh_wifi();
    show_wifi();
}

static void pick_net(view_t *t, int row) {
    (void)t;
    picked = row;
    v_set_text(password, "");
    show_wifi();
    if (nets[row].secured) v_focus(password);
}

static void join(view_t *b) {
    (void)b;
    if (picked < 0) return;
    char line[96];
    snprintf(line, sizeof(line), "Joining %s...", nets[picked].name);
    v_set_text(message, line);
    v_update();
    int r = wifi_connect(nets[picked].name, password->text);
    memset(password->text, 0, sizeof(password->text));
    refresh_wifi();
    v_set_text(message, r == 0 ? "Connected." : "Could not join: check the password.");
    picked = -1;
    show_wifi();
}

static void build_wifi(view_t *p) {
    no_card = v_label(p, "No Wi-Fi card was found.");
    no_card->color = UI_C_SECONDARY;
    wifi_box = v_box(p, V_COLUMN);
    wifi_box->flex = 1;
    wifi_box->gap = 10;
    view_t *top = v_box(wifi_box, V_ROW);
    status = v_label(top, "");
    status->style = UI_BOLD;
    status->flex = 1;
    signal_lbl = v_label(top, "");
    signal_lbl->color = UI_C_SECONDARY;
    view_t *buttons = v_box(wifi_box, V_ROW);
    v_button(buttons, "Scan", scan);
    disconnect = v_button(buttons, "Disconnect", disconnect_now);
    v_spacer(buttons);
    message = v_label(wifi_box, "");
    message->color = UI_C_SECONDARY;
    static const char *heads[] = { "Network", "Signal", "Security" };
    static const int widths[] = { 220, 100, 90 };
    networks = v_table(wifi_box, 3, heads, widths, net_cell);
    networks->on_select = pick_net;
    join_row = v_box(wifi_box, V_ROW);
    join_row->align = V_CENTER;
    password = v_field(join_row, "", join);
    password->flags = V_SECRET;
    password->flex = 1;
    snprintf(password->hint, sizeof(password->hint), "Password");
    v_button(join_row, "Join", join)->flags = UI_BTN_DEFAULT;
}

/* ---- Sound ----------------------------------------------------------------- */

static void show_sound(void) {
    v_show(no_sound, vol < 0);
    v_show(sound_box, vol >= 0);
    char line[32];
    snprintf(line, sizeof(line), "Output volume: %d%%", vol < 0 ? 0 : vol);
    v_set_text(vol_label, line);
    v_set_value(slider, vol < 0 ? 0 : vol);
    v_set_text(mute, vol ? "Mute" : "Unmute");
}

static void slid(view_t *s) {
    vol = volume(s->value);
    show_sound();
}

static void mute_now(view_t *b) {
    (void)b;
    if (vol) {
        muted_at = vol;
        vol = volume(0);
    } else {
        vol = volume(muted_at > 0 ? muted_at : 50);
    }
    show_sound();
}

static void test_sound(view_t *b) {
    (void)b;
    kcommand("beep");
}

static void build_sound(view_t *p) {
    no_sound = v_label(p, "No sound device was found.");
    no_sound->color = UI_C_SECONDARY;
    sound_box = v_box(p, V_COLUMN);
    sound_box->gap = 14;
    vol_label = v_label(sound_box, "");
    vol_label->style = UI_BOLD;
    slider = v_slider(sound_box, 0, slid);
    view_t *buttons = v_box(sound_box, V_ROW);
    mute = v_button(buttons, "Mute", mute_now);
    v_button(buttons, "Test", test_sound);
    v_spacer(buttons);
}

/* ---- Battery --------------------------------------------------------------- */

static void draw_bar(view_t *v, ui_canvas *c) {
    ui_fill(c, v->x, v->y, v->w, v->h, ui_color(UI_C_WINDOW));
    if (bat < 0) return;
    int p = bat & 0xFF, x = v->x, y = v->y, w = v->w, h = v->h;
    ui_round(c, x, y, w, h, 10 * S, ui_color(UI_C_CONTROL_EDGE));
    ui_round(c, x + 3 * S, y + 3 * S, w - 6 * S, h - 6 * S, 8 * S, ui_color(UI_C_CONTENT));
    ui_round(c, x + 5 * S, y + 5 * S, (w - 10 * S) * p / 100, h - 10 * S, 6 * S,
             p <= 20 ? 0xFF5F57 : (bat & 0x100) ? 0x28C840 : ui_color(UI_C_ACCENT));
}

static void show_battery(void) {
    bat = battery();
    char line[48];
    if (bat < 0) snprintf(line, sizeof(line), "This computer has no battery.");
    else snprintf(line, sizeof(line), "%d%%  %s", bat & 0xFF, (bat & 0x100) ? "charging" : "on battery");
    v_set_text(bat_label, line);
    v_show(bat_bar, bat >= 0);
    v_set_value(bat_bar, bat);                         /* (redraws the bar when it changes) */
}

static void build_battery(view_t *p) {
    bat_label = v_label(p, "");
    bat_label->style = UI_BOLD;
    bat_label->scale = 2;
    bat_bar = v_custom(p, draw_bar, 0);
    bat_bar->flex = 0;
    bat_bar->want_h = 40;
}

/* ---- Display, About ------------------------------------------------------- */

static void build_display(view_t *p) {
    struct os_gfx_info g;
    char line[64];
    gfx_info(&g);
    snprintf(line, sizeof(line), "%d x %d pixels", g.width, g.height);
    view_t *big = v_label(p, line);
    big->style = UI_BOLD;
    big->scale = 2;
    snprintf(line, sizeof(line), "Drawn at %dx for sharp text (Retina-style)", S);
    v_label(p, line)->color = UI_C_SECONDARY;
    v_label(p, "The native mode comes from the screen's EDID.")->color = UI_C_SECONDARY;
}

static void build_about(view_t *p) {
    struct os_uname u;
    struct os_meminfo m;
    struct os_user me;
    uname(&u);
    meminfo(&m);
    getuser(&me);
    char line[96];
    view_t *top = v_box(p, V_ROW);
    top->gap = 20;
    v_icon(top, "computer", 80);
    view_t *names = v_box(top, V_COLUMN);
    names->flex = 1;
    view_t *big = v_label(names, u.sysname);
    big->style = UI_BOLD;
    big->scale = 2;
    snprintf(line, sizeof(line), "Version %s (x86_64)", u.release);
    v_label(names, line)->color = UI_C_SECONDARY;
    snprintf(line, sizeof(line), "Memory: %lu MiB", m.ram >> 20);
    v_label(p, line);
    snprintf(line, sizeof(line), "Computer name: %s", u.hostname);
    v_label(p, line);
    snprintf(line, sizeof(line), "You are %s%s", me.name, me.uid == 0 ? " (administrator)" : "");
    v_label(p, line);
}

/* ---- The window ------------------------------------------------------------ */

static void choose(int i) {
    pane = i;
    for (int k = 0; k < PANES; k++) v_show(panes[k], k == i);
    v_set_text(title, pane_names[i]);
    v_table_select(sidebar, i);
}

static void pane_cell(view_t *t, int row, int col, char *out, int max) {
    (void)t;
    (void)col;
    snprintf(out, max, "%s", pane_names[row]);
}

static const char *pane_icon(view_t *t, int row) {
    (void)t;
    return pane_icons[row];
}

static void picked_pane(view_t *t, int row) {
    (void)t;
    choose(row);
}

static void tick(void) {                               /* now and then: the network, the battery */
    refresh_wifi();
    show_wifi();
    show_battery();
}

int main(int argc, char **argv) {
    S = ui_scale();
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
    ui_window win;
    if (ui_open(&win, "System Settings", 660 * S, 440 * S) < 0) {
        printf("settings: the desktop is not running\n");
        return 1;
    }
    ui_set_resizable(&win);

    view_t *root = v_box(0, V_ROW);
    root->pad = root->gap = 0;
    sidebar = v_table(root, 1, 0, 0, pane_cell);
    sidebar->flags = V_INSET;
    sidebar->flex = 0;
    sidebar->want_w = 170;
    sidebar->row_h = 36;
    sidebar->bg = UI_C_SIDEBAR;
    sidebar->row_icon = pane_icon;
    sidebar->on_select = picked_pane;
    v_table_rows(sidebar, PANES);
    view_t *content = v_box(root, V_COLUMN);
    content->flex = 1;
    content->pad = 24;
    content->gap = 14;
    title = v_label(content, "");
    title->style = UI_BOLD;
    title->scale = 2;
    for (int i = 0; i < PANES; i++) {
        panes[i] = v_box(content, V_COLUMN);
        panes[i]->flex = 1;
        panes[i]->gap = 10;
    }
    build_wifi(panes[P_WIFI]);
    build_sound(panes[P_SOUND]);
    build_battery(panes[P_BATTERY]);
    build_display(panes[P_DISPLAY]);
    build_about(panes[P_ABOUT]);

    refresh_wifi();
    vol = volume(-1);
    if (vol < 0) vol = -1;
    show_wifi();
    show_sound();
    show_battery();
    choose(pane);
    return v_run(&win, root, 2000, tick);
}
