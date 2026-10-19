// settings [wifi|sound|battery|appearance|wallpaper|dock|signin|display|about] -
// System Settings: Wi-Fi networks, the volume, the battery, how the desktop
// looks (light or dark, the accent colour, the wallpaper, the Dock, the menu
// bar's clock), the screen and this computer.
//
// The desktop's settings are shared with the window server (ws.h,
// struct ws_settings): a change here is on the screen at once, and the window
// server keeps it in DESKTOP.CFG in the home folder.
//
// Built from views (view.h): a sidebar table of panes, and one box per pane
// of which only the chosen one is shown. Every two seconds the state is read
// again; only the views whose text or value changed are drawn.
#include <stdio.h>
#include <string.h>
#include <os.h>
#include <view.h>

enum { P_WIFI, P_SOUND, P_BATTERY, P_APPEARANCE, P_WALLPAPER, P_DOCK, P_POINTER, P_SIGNIN, P_DISPLAY, P_ABOUT, PANES };
static const char *pane_names[PANES] = { "Wi-Fi", "Sound", "Battery", "Appearance", "Wallpaper",
                                          "Dock & Menu Bar", "Mouse & Trackpad", "Sign-in", "Display", "About" };
static const char *pane_keys[PANES] = { "wifi", "sound", "battery", "appearance", "wallpaper", "dock", "mouse",
                                        "signin", "display", "about" };
static const char *pane_icons[PANES] = { "wifi", "sound", "battery", "paint", "picture", "gear", "mouse",
                                         "user", "computer", "info" };

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

/* ---- The desktop: Appearance, Wallpaper, Dock & Menu Bar ------------------- */

static struct ws_settings *cfg, cfg_none;
static unsigned cfg_seen;
static view_t *looks[2], *size_lbl, *size_sl, *mag_lbl, *mag_sl, *pic_field, *pic_msg;
static view_t *speed_lbl[2], *speed_sl[2], *scroll_lbl, *scroll_sl;
static view_t *live[40];                          /* switches and swatches: redrawn when the settings change */
static int n_live;

/* Tell the window server (it applies it with its next frame). */
static void changed(void) {
    cfg->seq++;
    cfg_seen = cfg->seq;
}

static view_t *keep(view_t *v) {
    if (n_live < 40) live[n_live++] = v;
    return v;
}

/* A switch, as macOS's: a pill, the accent colour when on; data: the int it sets. */
static void draw_switch(view_t *v, ui_canvas *c) {
    int on = *(int *)v->data, w = 40 * S, h = 24 * S, x = v->x + v->w - w, y = v->y + (v->h - h) / 2;
    ui_fill(c, v->x, v->y, v->w, v->h, ui_color(UI_C_WINDOW));
    ui_round(c, x, y, w, h, h / 2, on ? UI_ACCENT : ui_color(UI_C_CONTROL_OFF));
    ui_circle(c, on ? x + w - h / 2 : x + h / 2, y + h / 2, h / 2 - 2 * S, 0xFFFFFF);
}

static int switch_event(view_t *v, const struct ws_event *e) {
    if (e->type != WS_EV_DOWN) return 0;
    int *field = v->data;
    *field = !*field;
    changed();
    v_dirty(v);
    return 1;
}

static void switch_row(view_t *p, const char *label, int *field) {
    view_t *row = v_box(p, V_ROW);
    row->pad = 0;
    row->align = V_CENTER;
    v_label(row, label)->flex = 1;
    view_t *sw = keep(v_custom(row, draw_switch, switch_event));
    sw->want_w = 44;
    sw->want_h = 28;
    sw->flex = 0;
    sw->data = field;
}

static void heading(view_t *p, const char *text) {
    view_t *h = v_label(p, text);
    h->style = UI_BOLD;
}

/* Swatches: value is what they set (an accent, a gradient, a colour). */
enum { SW_ACCENT, SW_GRADIENT, SW_COLOR };
static const unsigned colors[] = { 0x1C1C1E, 0x3A3A3C, 0x1F3A68, 0x2A9D8F, 0x3B7D4F, 0xD4B483, 0xE76F51, 0x6D3B6E };
#define N_COLORS 8

static int swatch_on(view_t *v) {
    int kind = (int)(long)v->data;
    if (kind == SW_ACCENT) return cfg->accent == v->value;
    if (kind == SW_GRADIENT) return cfg->wall_kind == WS_WALL_GRADIENT && cfg->wall_gradient == v->value;
    return cfg->wall_kind == WS_WALL_COLOR && cfg->wall_color == colors[v->value];
}

static void draw_swatch(view_t *v, ui_canvas *c) {
    int kind = (int)(long)v->data, on = swatch_on(v);
    ui_fill(c, v->x, v->y, v->w, v->h, ui_color(UI_C_WINDOW));
    if (kind == SW_ACCENT || kind == SW_COLOR) {      /* a circle; the chosen one has a ring */
        int r = (v->w < v->h ? v->w : v->h) / 2 - 3 * S, cx = v->x + v->w / 2, cy = v->y + v->h / 2;
        unsigned col = kind == SW_ACCENT ? ui_accent_color(v->value, ui_pref(UI_PREF_DARK), 0) : colors[v->value];
        if (on) ui_circle(c, cx, cy, r + 3 * S, ui_color(UI_C_LABEL));
        if (on) ui_circle(c, cx, cy, r + 1 * S, ui_color(UI_C_WINDOW));
        ui_circle(c, cx, cy, r - (on ? 1 : 0) * S, col);
        return;
    }
    unsigned top, bottom;                             /* a gradient: a small picture of it */
    ui_wall_gradient(v->value, ui_pref(UI_PREF_DARK), &top, &bottom);
    int m = 3 * S, x = v->x + m, y = v->y + m, w = v->w - 2 * m, h = v->h - 2 * m;
    if (on) ui_round(c, v->x, v->y, v->w, v->h, 8 * S, UI_ACCENT);
    for (int k = 0; k < h; k++) {
        unsigned t = (unsigned)(k * 256 / (h ? h : 1));
        unsigned col = 0;
        for (int sh = 0; sh <= 16; sh += 8) {
            int a = (int)(top >> sh & 255), b = (int)(bottom >> sh & 255);
            col |= (unsigned)(a + (b - a) * (int)t / 256) << sh;
        }
        ui_fill(c, x, y + k, w, 1, col);
    }
}

static int swatch_event(view_t *v, const struct ws_event *e) {
    if (e->type != WS_EV_DOWN) return 0;
    int kind = (int)(long)v->data;
    if (kind == SW_ACCENT) cfg->accent = v->value;
    else if (kind == SW_GRADIENT) {
        cfg->wall_kind = WS_WALL_GRADIENT;
        cfg->wall_gradient = v->value;
    } else {
        cfg->wall_kind = WS_WALL_COLOR;
        cfg->wall_color = colors[v->value];
    }
    changed();
    for (int i = 0; i < n_live; i++) v_dirty(live[i]);   /* the old choice loses its ring */
    return 1;
}

static view_t *swatch(view_t *row, int kind, int value, int w, int h) {
    view_t *v = keep(v_custom(row, draw_swatch, swatch_event));
    v->data = (void *)(long)kind;
    v->value = value;
    v->want_w = w;
    v->want_h = h;
    v->flex = 0;
    return v;
}

static void show_desktop(void) {                       /* the controls, from the settings */
    v_set_flags(looks[0], cfg->dark ? 0 : V_TOGGLED);
    v_set_flags(looks[1], cfg->dark ? V_TOGGLED : 0);
    char t[48];
    snprintf(t, sizeof(t), "Size: %d pt", cfg->dock_size);
    v_set_text(size_lbl, t);
    v_set_value(size_sl, (cfg->dock_size - 32) * 100 / 64);
    if (cfg->dock_magnify) snprintf(t, sizeof(t), "Magnification: %d%%", cfg->dock_magnify);
    else snprintf(t, sizeof(t), "Magnification: off");
    v_set_text(mag_lbl, t);
    v_set_value(mag_sl, cfg->dock_magnify);
    struct os_pointer *pt = &cfg->pointer;
    int sp[2] = { pt->pad_speed, pt->mouse_speed };
    for (int i = 0; i < 2; i++) {
        snprintf(t, sizeof(t), "%s speed: %s", i ? "Mouse" : "Tracking",
                 sp[i] < 25 ? "slow" : sp[i] < 45 ? "slower" : sp[i] <= 55 ? "normal" : sp[i] <= 75 ? "faster" : "fast");
        v_set_text(speed_lbl[i], t);
        v_set_value(speed_sl[i], sp[i]);
    }
    snprintf(t, sizeof(t), "Scrolling speed: %dx", pt->scroll_speed);
    v_set_text(scroll_lbl, t);
    v_set_value(scroll_sl, (pt->scroll_speed - 1) * 100 / 3);
    for (int i = 0; i < n_live; i++) v_dirty(live[i]);
}

static void pick_light(view_t *b) { (void)b; cfg->dark = 0; changed(); show_desktop(); }
static void pick_dark(view_t *b) { (void)b; cfg->dark = 1; changed(); show_desktop(); }

static void build_appearance(view_t *p) {
    heading(p, "Appearance");
    view_t *row = v_box(p, V_ROW);
    row->pad = 0;
    looks[0] = v_button(row, "Light", pick_light);
    looks[1] = v_button(row, "Dark", pick_dark);
    heading(p, "Accent colour");
    row = v_box(p, V_ROW);
    row->pad = 0;
    row->gap = 4;
    for (int i = 0; i < WS_ACCENTS; i++) swatch(row, SW_ACCENT, i, 34, 34);
    heading(p, "Accessibility");
    switch_row(p, "Increase contrast", &cfg->contrast);
    switch_row(p, "Reduce motion", &cfg->reduce_motion);
    switch_row(p, "Reduce transparency", &cfg->reduce_transp);
}

static void set_picture(view_t *v) {
    (void)v;
    char path[160];
    const char *name = pic_field->text;
    struct os_user me;
    if (!name[0]) return;
    if (name[0] != '/' && getuser(&me) == 0) snprintf(path, sizeof(path), "%s/%s", me.home, name);
    else snprintf(path, sizeof(path), "%s", name);
    int fd = open(path, OPEN_READ);
    if (fd < 0) {
        v_set_text(pic_msg, "There is no such file.");
        return;
    }
    close(fd);
    snprintf(cfg->wall_path, sizeof(cfg->wall_path), "%s", path);
    cfg->wall_kind = WS_WALL_PICTURE;
    changed();
    v_set_text(pic_msg, "The picture is on the desktop (if it is one this OS can read).");
    show_desktop();
}

static void build_wallpaper(view_t *p) {
    heading(p, "Gradients");
    for (int r = 0; r < 2; r++) {
        view_t *row = v_box(p, V_ROW);
        row->pad = 0;
        row->gap = 8;
        for (int i = 0; i < WS_WALL_GRADIENTS / 2; i++) swatch(row, SW_GRADIENT, r * WS_WALL_GRADIENTS / 2 + i, 104, 64);
    }
    heading(p, "Colours");
    view_t *row = v_box(p, V_ROW);
    row->pad = 0;
    row->gap = 4;
    for (int i = 0; i < N_COLORS; i++) swatch(row, SW_COLOR, i, 34, 34);
    heading(p, "Picture");
    row = v_box(p, V_ROW);
    row->pad = 0;
    pic_field = v_field(row, cfg->wall_kind == WS_WALL_PICTURE ? cfg->wall_path : "", set_picture);
    pic_field->flex = 1;
    snprintf(pic_field->hint, sizeof(pic_field->hint), "PHOTO.JPG (in your home folder) or a path");
    v_button(row, "Set", set_picture);
    pic_msg = v_label(p, "PNG, JPEG, GIF, BMP or TGA; it is scaled to fill the screen.");
    pic_msg->color = UI_C_SECONDARY;
}

static void dock_size(view_t *sl) {
    cfg->dock_size = 32 + sl->value * 64 / 100;
    changed();
    show_desktop();
}

static void dock_magnify(view_t *sl) {
    cfg->dock_magnify = sl->value;
    changed();
    show_desktop();
}

static void build_dock(view_t *p) {
    heading(p, "Dock");
    view_t *row = v_box(p, V_ROW);
    row->pad = 0;
    row->align = V_CENTER;
    size_lbl = v_label(row, "");
    size_lbl->want_w = 170;
    size_sl = v_slider(row, 0, dock_size);
    size_sl->flex = 1;
    row = v_box(p, V_ROW);
    row->pad = 0;
    row->align = V_CENTER;
    mag_lbl = v_label(row, "");
    mag_lbl->want_w = 170;
    mag_sl = v_slider(row, 0, dock_magnify);
    mag_sl->flex = 1;
    switch_row(p, "Automatically hide and show the Dock", &cfg->dock_autohide);
    switch_row(p, "Show dots under open apps", &cfg->dock_dots);
    switch_row(p, "Animate opening apps", &cfg->dock_bounce);
    heading(p, "Menu bar clock");
    switch_row(p, "Use a 24-hour clock", &cfg->clock_24h);
    switch_row(p, "Show the date", &cfg->clock_date);
    switch_row(p, "Show the seconds", &cfg->clock_seconds);
}

/* ---- Mouse & Trackpad ------------------------------------------------------- */

static void pad_speed(view_t *sl) { cfg->pointer.pad_speed = sl->value; changed(); show_desktop(); }
static void mouse_speed(view_t *sl) { cfg->pointer.mouse_speed = sl->value; changed(); show_desktop(); }

static void scroll_speed(view_t *sl) {
    cfg->pointer.scroll_speed = 1 + (sl->value + 16) / 33;
    changed();
    show_desktop();
}

static view_t *slider_row(view_t *p, view_t **lbl, void (*moved)(view_t *)) {
    view_t *row = v_box(p, V_ROW);
    row->pad = 0;
    row->align = V_CENTER;
    *lbl = v_label(row, "");
    (*lbl)->want_w = 190;
    view_t *sl = v_slider(row, 0, moved);
    sl->flex = 1;
    return sl;
}

static void build_pointer(view_t *p) {
    struct os_pointer found;
    pointer_settings(&found, 0);
    heading(p, "Trackpad");
    speed_sl[0] = slider_row(p, &speed_lbl[0], pad_speed);
    switch_row(p, "Tap to click", &cfg->pointer.tap_click);
    switch_row(p, "Tap with two fingers for a secondary click", &cfg->pointer.tap_right);
    switch_row(p, "Tap twice and drag to move things", &cfg->pointer.tap_drag);
    switch_row(p, "Natural scrolling (content follows your fingers)", &cfg->pointer.natural);
    heading(p, "Mouse");
    speed_sl[1] = slider_row(p, &speed_lbl[1], mouse_speed);
    switch_row(p, "Use the right button as the primary button", &cfg->pointer.swap_buttons);
    switch_row(p, "Reverse the scroll wheel", &cfg->pointer.wheel_reverse);
    heading(p, "Both");
    scroll_sl = slider_row(p, &scroll_lbl, scroll_speed);
    switch_row(p, "Pointer acceleration (fast moves go further)", &cfg->pointer.accel);
    char t[80];
    snprintf(t, sizeof(t), "Found: %s%s%s", found.has_pad ? "a trackpad" : "",
             found.has_pad && found.has_mouse ? " and " : "", found.has_mouse ? "a mouse" : "");
    if (!found.has_pad && !found.has_mouse) snprintf(t, sizeof(t), "Touch the trackpad or move the mouse to have it found.");
    v_label(p, t)->color = UI_C_SECONDARY;
}

/* ---- Sign-in ----------------------------------------------------------------
 * Whether the desktop asks for a PIN or a password before it opens. With none
 * (the account's password is empty) loginwindow opens the Lobby straight away.
 * The secret is the account's password (hashed by the kernel); SIGNIN.CFG in
 * the home folder says only whether the login screen shows a PIN or a password. */

enum { SI_OFF, SI_PIN, SI_PASSWORD };
static int si_on, si_mode;                             /* a sign-in is on now; the kind chosen below */
static view_t *si_status, *si_off_btn, *si_pin_btn, *si_pw_btn, *si_cur, *si_new, *si_again, *si_apply, *si_msg;

static void signin_refresh(void) {
    si_on = signin_on() == 1;
    char line[96];
    snprintf(line, sizeof(line), si_on ? "Sign-in is on: the desktop asks for it." : "Sign-in is off: the desktop opens straight away.");
    v_set_text(si_status, line);
    v_show(si_cur, si_on);
    v_show(si_new, si_mode != SI_OFF);
    v_show(si_again, si_mode != SI_OFF);
    snprintf(si_cur->hint, sizeof(si_cur->hint), "Current PIN or password");
    snprintf(si_new->hint, sizeof(si_new->hint), si_mode == SI_PIN ? "New PIN (4-8 digits)" : "New password");
    snprintf(si_again->hint, sizeof(si_again->hint), si_mode == SI_PIN ? "New PIN again" : "New password again");
    v_set_text(si_apply, si_mode == SI_OFF ? "Turn sign-in off" : si_on ? "Change" : "Turn on");
    v_set_flags(si_off_btn, si_mode == SI_OFF ? UI_BTN_DEFAULT : 0);
    v_set_flags(si_pin_btn, si_mode == SI_PIN ? UI_BTN_DEFAULT : 0);
    v_set_flags(si_pw_btn, si_mode == SI_PASSWORD ? UI_BTN_DEFAULT : 0);
}

static void si_pick(int mode) {
    si_mode = mode;
    v_set_text(si_msg, "");
    signin_refresh();
}
static void si_pick_off(view_t *b) { (void)b; si_pick(SI_OFF); }
static void si_pick_pin(view_t *b) { (void)b; si_pick(SI_PIN); }
static void si_pick_pw(view_t *b) { (void)b; si_pick(SI_PASSWORD); }

static void si_write_kind(const char *kind) {          /* SIGNIN.CFG: "pin" or "password" (nothing when off) */
    struct os_user me;
    char path[80];
    getuser(&me);
    snprintf(path, sizeof(path), "%s%sSIGNIN.CFG", me.home, strcmp(me.home, "/") ? "/" : "");
    if (!kind) {
        unlink(path);
        return;
    }
    int fd = open(path, OPEN_WRITE);
    if (fd < 0) return;
    write(fd, kind, strlen(kind));
    close(fd);
}

static void si_apply_now(view_t *b) {
    (void)b;
    const char *cur = si_on ? si_cur->text : "", *now = si_mode == SI_OFF ? "" : si_new->text;
    if (si_mode != SI_OFF) {
        int n = (int)strlen(now);
        if (si_mode == SI_PIN) {
            for (int i = 0; i < n; i++)
                if (now[i] < '0' || now[i] > '9') {
                    v_set_text(si_msg, "A PIN is digits only.");
                    return;
                }
            if (n < 4 || n > 8) {
                v_set_text(si_msg, "A PIN is 4 to 8 digits.");
                return;
            }
        } else if (n < 4) {
            v_set_text(si_msg, "Use at least 4 characters.");
            return;
        }
        if (strcmp(now, si_again->text)) {
            v_set_text(si_msg, "The two do not match.");
            return;
        }
    }
    if (password_change(cur, now) != 0) {
        v_set_text(si_msg, si_on ? "The current PIN or password is wrong." : "Could not change the sign-in.");
        return;
    }
    si_write_kind(si_mode == SI_PIN ? "pin" : si_mode == SI_PASSWORD ? "password" : 0);
    memset(si_cur->text, 0, sizeof(si_cur->text));
    memset(si_new->text, 0, sizeof(si_new->text));
    memset(si_again->text, 0, sizeof(si_again->text));
    v_set_text(si_cur, "");
    v_set_text(si_new, "");
    v_set_text(si_again, "");
    v_set_text(si_msg, si_mode == SI_OFF ? "Sign-in is off." : si_mode == SI_PIN ? "The PIN is set." : "The password is set.");
    signin_refresh();
}

static void build_signin(view_t *p) {
    si_status = v_label(p, "");
    si_status->style = UI_BOLD;
    v_text(p, "With a PIN or a password the login screen asks for it before the Lobby opens. "
              "With none, DOOR OS goes straight from the door into the Lobby. "
              "With several accounts the login screen shows their profiles to choose from.")->color = UI_C_SECONDARY;
    view_t *kinds = v_box(p, V_ROW);
    kinds->gap = 8;
    si_off_btn = v_button(kinds, "No sign-in", si_pick_off);
    si_pin_btn = v_button(kinds, "PIN", si_pick_pin);
    si_pw_btn = v_button(kinds, "Password", si_pick_pw);
    v_spacer(kinds);
    si_cur = v_field(p, "", si_apply_now);
    si_cur->flags = V_SECRET;
    si_new = v_field(p, "", si_apply_now);
    si_new->flags = V_SECRET;
    si_again = v_field(p, "", si_apply_now);
    si_again->flags = V_SECRET;
    view_t *row = v_box(p, V_ROW);
    si_apply = v_button(row, "Turn on", si_apply_now);
    si_apply->flags = UI_BTN_DEFAULT;
    v_spacer(row);
    si_msg = v_label(p, "");
    si_msg->color = UI_C_SECONDARY;
    si_mode = signin_on() == 1 ? SI_PASSWORD : SI_OFF;
    signin_refresh();
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
    if (cfg->seq != cfg_seen) {                        /* changed from the menu bar */
        cfg_seen = cfg->seq;
        show_desktop();
    }
    refresh_wifi();
    show_wifi();
    show_battery();
}

int main(int argc, char **argv) {
    S = ui_scale();
    for (int i = 0; argc > 1 && i < PANES; i++)
        if (!strcmp(argv[1], pane_keys[i])) pane = i;
    cfg = shm_map(WS_SETTINGS, sizeof(struct ws_settings));
    if (!cfg || cfg->magic != WS_SETTINGS_MAGIC) cfg = &cfg_none;   /* (no window server: nothing to change) */
    cfg_seen = cfg->seq;
    ui_window win;
    if (ui_open(&win, "System Settings", 780 * S, 600 * S) < 0) {
        printf("settings: the desktop is not running\n");
        return 1;
    }
    ui_set_resizable(&win);

    view_t *root = v_box(0, V_ROW);
    root->pad = root->gap = 0;
    sidebar = v_table(root, 1, 0, 0, pane_cell);
    sidebar->flags = V_INSET;
    sidebar->flex = 0;
    sidebar->want_w = 190;
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
    build_appearance(panes[P_APPEARANCE]);
    build_wallpaper(panes[P_WALLPAPER]);
    build_dock(panes[P_DOCK]);
    build_pointer(panes[P_POINTER]);
    build_signin(panes[P_SIGNIN]);
    build_display(panes[P_DISPLAY]);
    build_about(panes[P_ABOUT]);

    refresh_wifi();
    vol = volume(-1);
    if (vol < 0) vol = -1;
    show_wifi();
    show_sound();
    show_battery();
    show_desktop();
    choose(pane);
    return v_run(&win, root, 2000, tick);
}