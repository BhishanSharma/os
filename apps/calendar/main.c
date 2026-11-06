// calendar - Calendar: a month at a glance, today marked, and the events of
// the day you pick. Events live in ~/Calendar/events.txt, one per line:
//     2026-11-06 14:30 Dentist
// (a time is optional: "2026-12-25 Christmas"). Type an event in the field
// ("14:30 Dentist" or just "Dentist") and press Add; select one and press
// Delete to remove it. While Calendar is open it reminds you of an event at
// its time with a notification. Arrows move the day, < and > the month.
//
// Built from views (view.h): a toolbar, the month (a custom view), the day's
// events (a table) and a field to add one.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <os.h>
#include <view.h>

#define MAX_EVENTS 512

typedef struct { int y, m, d, hh, mm; char text[96]; } event_t;   /* hh < 0: all day */

static event_t events[MAX_EVENTS];
static int n_events;
static int view_y, view_m, sel_d, today_y, today_m, today_d;
static int day_rows[64], n_day_rows;          /* the events of the selected day (indexes) */
static view_t *title, *month, *list, *field;
static char path[96];
static int S;

static const char *month_names[] = { "January", "February", "March", "April", "May", "June", "July",
                                     "August", "September", "October", "November", "December" };

static int leap(int y) { return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0; }
static int days_in(int y, int m) {
    static const int d[] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    return m == 2 && leap(y) ? 29 : d[m - 1];
}
static int weekday(int y, int m, int d) {      /* 0 = Monday */
    static const int t[] = { 0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4 };
    if (m < 3) y--;
    int w = (y + y / 4 - y / 100 + y / 400 + t[m - 1] + d) % 7;   /* 0 = Sunday */
    return (w + 6) % 7;
}

static int has_events(int y, int m, int d) {
    for (int i = 0; i < n_events; i++)
        if (events[i].y == y && events[i].m == m && events[i].d == d) return 1;
    return 0;
}

static int cmp_events(const void *a, const void *b) {
    const event_t *x = a, *y = b;
    long kx = ((long)x->y * 400 + x->m * 32 + x->d) * 2000 + (x->hh < 0 ? -1 : x->hh * 60 + x->mm);
    long ky = ((long)y->y * 400 + y->m * 32 + y->d) * 2000 + (y->hh < 0 ? -1 : y->hh * 60 + y->mm);
    return kx < ky ? -1 : kx > ky;
}

static void load(void) {
    n_events = 0;
    int fd = open(path, OPEN_READ);
    if (fd < 0) return;
    static char buf[MAX_EVENTS * 120];
    long n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    buf[n > 0 ? n : 0] = 0;
    for (char *line = buf, *next; line && *line && n_events < MAX_EVENTS; line = next) {
        next = strchr(line, '\n');
        if (next) *next++ = 0;
        event_t e;
        int used = 0;
        e.hh = -1;
        e.mm = 0;
        if (sscanf(line, "%d-%d-%d %n", &e.y, &e.m, &e.d, &used) < 3 || !used) continue;
        char *rest = line + used;
        int hh, mm, k = 0;
        if (sscanf(rest, "%d:%d %n", &hh, &mm, &k) >= 2 && k && hh >= 0 && hh < 24 && mm >= 0 && mm < 60) {
            e.hh = hh;
            e.mm = mm;
            rest += k;
        }
        snprintf(e.text, sizeof(e.text), "%s", rest);
        events[n_events++] = e;
    }
    qsort(events, (size_t)n_events, sizeof(event_t), cmp_events);
}

static void save(void) {
    struct os_user u;
    char dir[96];
    if (getuser(&u) == 0) {
        snprintf(dir, sizeof(dir), "%s/Calendar", u.home);
        mkdir(dir);
    }
    int fd = open(path, OPEN_WRITE);
    if (fd < 0) return;
    for (int i = 0; i < n_events; i++) {
        char line[160];
        const event_t *e = &events[i];
        int n = e->hh < 0 ? snprintf(line, sizeof(line), "%04d-%02d-%02d %s\n", e->y, e->m, e->d, e->text)
                          : snprintf(line, sizeof(line), "%04d-%02d-%02d %02d:%02d %s\n", e->y, e->m, e->d,
                                     e->hh, e->mm, e->text);
        write(fd, line, (size_t)n);
    }
    close(fd);
}

static void refresh(void) {
    char t[48];
    snprintf(t, sizeof(t), "%s %d", month_names[view_m - 1], view_y);
    v_set_text(title, t);
    n_day_rows = 0;
    for (int i = 0; i < n_events && n_day_rows < 64; i++)
        if (events[i].y == view_y && events[i].m == view_m && events[i].d == sel_d) day_rows[n_day_rows++] = i;
    v_table_rows(list, n_day_rows);
    v_dirty(list);
    v_dirty(month);
}

static void cell(view_t *t, int row, int col, char *out, int max) {
    (void)t;
    const event_t *e = &events[day_rows[row]];
    if (col == 0) {
        if (e->hh < 0) snprintf(out, (size_t)max, "All day");
        else snprintf(out, (size_t)max, "%02d:%02d", e->hh, e->mm);
    } else {
        snprintf(out, (size_t)max, "%s", e->text);
    }
}

static void go_month(int step) {
    view_m += step;
    if (view_m < 1) { view_m = 12; view_y--; }
    if (view_m > 12) { view_m = 1; view_y++; }
    if (sel_d > days_in(view_y, view_m)) sel_d = days_in(view_y, view_m);
    refresh();
}

static void prev_clicked(view_t *b) { (void)b; go_month(-1); }
static void next_clicked(view_t *b) { (void)b; go_month(1); }
static void today_clicked(view_t *b) {
    (void)b;
    view_y = today_y, view_m = today_m, sel_d = today_d;
    refresh();
}

static void add_clicked(view_t *b) {
    (void)b;
    const char *s = field->text;
    while (*s == ' ') s++;
    if (!*s || n_events >= MAX_EVENTS) return;
    event_t e = { view_y, view_m, sel_d, -1, 0, "" };
    int hh, mm, k = 0;
    if (sscanf(s, "%d:%d %n", &hh, &mm, &k) >= 2 && k && hh >= 0 && hh < 24 && mm >= 0 && mm < 60) {
        e.hh = hh;
        e.mm = mm;
        s += k;
    }
    snprintf(e.text, sizeof(e.text), "%s", *s ? s : "Event");
    events[n_events++] = e;
    qsort(events, (size_t)n_events, sizeof(event_t), cmp_events);
    save();
    v_set_text(field, "");
    refresh();
}

static void field_entered(view_t *f) { add_clicked(f); }

static void delete_clicked(view_t *b) {
    (void)b;
    int r = list->selected;
    if (r < 0 || r >= n_day_rows) return;
    int i = day_rows[r];
    memmove(events + i, events + i + 1, (size_t)(n_events - i - 1) * sizeof(event_t));
    n_events--;
    save();
    v_table_select(list, -1);
    refresh();
}

/* ---- The month ---- */

static void draw_month(view_t *v, ui_canvas *c) {
    ui_fill(c, v->x, v->y, v->w, v->h, ui_color(UI_C_CONTENT));
    static const char *wd[] = { "Mon", "Tue", "Wed", "Thu", "Fri", "Sat", "Sun" };
    int cw = v->w / 7, head = 24 * S, ch = (v->h - head) / 6;
    for (int i = 0; i < 7; i++) {
        int tw = ui_text_width(wd[i], S);
        ui_text(c, v->x + i * cw + (cw - tw) / 2, v->y + 4 * S, wd[i], ui_color(i >= 5 ? UI_C_TERTIARY : UI_C_SECONDARY), S);
    }
    int first = weekday(view_y, view_m, 1), n = days_in(view_y, view_m);
    for (int d = 1; d <= n; d++) {
        int k = first + d - 1, x = v->x + (k % 7) * cw, y = v->y + head + (k / 7) * ch;
        int is_today = view_y == today_y && view_m == today_m && d == today_d, is_sel = d == sel_d;
        if (is_sel) ui_round(c, x + 3 * S, y + 3 * S, cw - 6 * S, ch - 6 * S, 8 * S, ui_color(UI_C_SURFACE3));
        char num[4];
        snprintf(num, sizeof(num), "%d", d);
        int tw = ui_text_width(num, S), tx = x + (cw - tw) / 2, ty = y + 8 * S;
        if (is_today) {
            ui_circle(c, x + cw / 2, ty + 8 * S, 13 * S, ui_color(UI_C_DANGER));
            ui_text_bold(c, tx, ty, num, UI_WHITE, S);
        } else {
            ui_text(c, tx, ty, num, ui_color(k % 7 >= 5 ? UI_C_SECONDARY : UI_C_LABEL), S);
        }
        if (has_events(view_y, view_m, d)) ui_circle(c, x + cw / 2, ty + 26 * S, 3 * S, ui_color(UI_C_ACCENT));
    }
}

static int month_event(view_t *v, const struct ws_event *e) {
    if (e->type == WS_EV_DOWN) {
        int cw = v->w / 7, head = 24 * S, ch = (v->h - head) / 6;
        if (e->y < head || cw <= 0 || ch <= 0) return 1;
        int k = (e->y - head) / ch * 7 + e->x / cw, d = k - weekday(view_y, view_m, 1) + 1;
        if (d >= 1 && d <= days_in(view_y, view_m)) {
            sel_d = d;
            refresh();
        }
        return 1;
    }
    if (e->type == WS_EV_KEY) {
        int k = e->key, n = days_in(view_y, view_m), step = 0;
        if (k == KEYCODE_LEFT) step = -1;
        else if (k == KEYCODE_RIGHT) step = 1;
        else if (k == KEYCODE_UP) step = -7;
        else if (k == KEYCODE_DOWN) step = 7;
        else if (k == '<' || k == ',') { go_month(-1); return 1; }
        else if (k == '>' || k == '.') { go_month(1); return 1; }
        else if (k == 't') { today_clicked(0); return 1; }
        else return 0;
        sel_d += step;
        if (sel_d < 1) { go_month(-1); sel_d += days_in(view_y, view_m); }
        else if (sel_d > n) { sel_d -= n; go_month(1); }
        refresh();
        return 1;
    }
    return 0;
}

/* Reminders: an event at this minute (while Calendar is open). */
static void tick(void) {
    static int last_min = -1;
    struct os_time t;
    if (gettime(&t) != 0 || t.minute == last_min) return;
    last_min = t.minute;
    if (t.day != today_d || t.month != today_m || t.year != today_y) {   /* past midnight */
        today_y = t.year, today_m = t.month, today_d = t.day;
        v_dirty(month);
    }
    for (int i = 0; i < n_events; i++) {
        const event_t *e = &events[i];
        if (e->y == t.year && e->m == t.month && e->d == t.day && e->hh == t.hour && e->mm == t.minute) {
            char when[16];
            snprintf(when, sizeof(when), "%02d:%02d", e->hh, e->mm);
            ui_notify(e->text, when);
        }
    }
}

int main(void) {
    S = ui_scale();
    struct os_user u;
    if (getuser(&u) == 0) snprintf(path, sizeof(path), "%s/Calendar/events.txt", u.home);
    else snprintf(path, sizeof(path), "/TMP/events.txt");
    struct os_time t;
    if (gettime(&t) != 0) t = (struct os_time){ 2026, 1, 1, 0, 0, 0, "" };
    today_y = view_y = t.year;
    today_m = view_m = t.month;
    today_d = sel_d = t.day;
    load();

    ui_window win;
    if (ui_open(&win, "Calendar", 560 * S, 560 * S) < 0) {
        printf("calendar: the desktop is not running\n");
        return 1;
    }
    ui_set_resizable(&win);
    view_t *root = v_box(0, V_COLUMN);
    root->pad = root->gap = 0;
    view_t *bar = v_box(root, V_ROW);
    bar->pad = 8;
    bar->bg = UI_C_TOOLBAR;
    bar->align = V_CENTER;
    title = v_label(bar, "");
    title->style = UI_BOLD;
    title->flex = 1;
    v_button(bar, "<", prev_clicked);
    v_button(bar, "Today", today_clicked);
    v_button(bar, ">", next_clicked);
    month = v_custom(root, draw_month, month_event);
    month->focusable = 1;
    month->flex = 3;
    static const char *heads[] = { "Time", "Event" };
    static const int widths[] = { 80, 0 };
    list = v_table(root, 2, heads, widths, cell);
    list->flex = 2;
    view_t *add = v_box(root, V_ROW);
    add->pad = 8;
    add->align = V_CENTER;
    add->bg = UI_C_ROW_ALT;
    field = v_field(add, "", field_entered);
    field->flex = 1;
    snprintf(field->hint, sizeof(field->hint), "14:30 Dentist (the selected day)");
    v_button(add, "Add", add_clicked);
    v_button(add, "Delete", delete_clicked);
    refresh();
    v_focus(month);
    return v_run(&win, root, 1000, tick);
}
