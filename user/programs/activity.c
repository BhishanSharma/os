// activity - Activity Monitor: the programs running, their CPU use and
// memory, and a button to quit one.
#include <stdio.h>
#include <string.h>
#include <os.h>
#include <ui.h>

#define MAX_ROWS 64

typedef struct {
    int pid, parent, state, program;
    unsigned cpu_ms, last_ms;
    int percent;
    char name[16], user[16];
} row_t;

static ui_window win;
static int S;
static row_t rows[MAX_ROWS], old[MAX_ROWS];
static int n_rows, n_old, selected_pid = -1, first;
static unsigned long last_sample;

static void sample(void) {
    unsigned long now = uptime_ms(), dt = now - last_sample;
    memcpy(old, rows, sizeof(rows));
    n_old = n_rows;
    n_rows = 0;
    struct os_task t;
    for (int i = 0, used; (used = taskinfo(i, &t)) >= 0 && n_rows < MAX_ROWS; i++) {
        if (!used || t.state == TASKSTATE_DONE || t.pid <= 0) continue;   /* 0: a free slot */
        row_t *r = &rows[n_rows++];
        memset(r, 0, sizeof(*r));
        r->pid = t.pid;
        r->parent = t.parent;
        r->state = t.state;
        r->program = t.is_program;
        r->cpu_ms = t.cpu_ms;
        snprintf(r->name, sizeof(r->name), "%s", t.name);
        snprintf(r->user, sizeof(r->user), "%s", t.user);
        for (int k = 0; k < n_old; k++)            /* CPU since the last look */
            if (old[k].pid == r->pid && old[k].cpu_ms <= r->cpu_ms && dt)
                r->percent = (int)((r->cpu_ms - old[k].cpu_ms) * 100 / dt);
    }
    last_sample = now;
}

static int col_x(int i) {
    static const int at[] = { 16, 220, 290, 380, 470 };
    return at[i] * S;
}

static void draw(void) {
    ui_canvas *c = &win.c;
    ui_fill(c, 0, 0, c->w, c->h, UI_WHITE);
    int top = 44 * S, rh = 24 * S;
    ui_fill(c, 0, 0, c->w, top, 0xF2F2F6);
    ui_fill(c, 0, top - 1, c->w, 1, 0xDADAE0);
    static const char *heads[] = { "Process", "PID", "User", "% CPU", "State" };
    for (int i = 0; i < 5; i++) ui_text_bold(c, col_x(i), 14 * S, heads[i], 0x505058, S);
    int list_h = c->h - top - 60 * S, visible = list_h / rh;
    if (first > n_rows - visible) first = n_rows - visible;
    if (first < 0) first = 0;
    for (int i = 0; i < visible && first + i < n_rows; i++) {
        row_t *r = &rows[first + i];
        int y = top + i * rh, sel = r->pid == selected_pid;
        if (sel) ui_fill(c, 0, y, c->w, rh, UI_ACCENT);
        else if (i % 2) ui_fill(c, 0, y, c->w, rh, 0xF6F6F9);
        unsigned fg = sel ? UI_WHITE : UI_TEXT, dim = sel ? UI_WHITE : UI_DIM;
        char text[24];
        int ty = y + (rh - 16 * S) / 2;
        ui_text(c, col_x(0), ty, r->name, r->program ? fg : dim, S);
        snprintf(text, sizeof(text), "%d", r->pid);
        ui_text(c, col_x(1), ty, text, fg, S);
        ui_text(c, col_x(2), ty, r->user, fg, S);
        snprintf(text, sizeof(text), "%d", r->percent > 100 ? 100 : r->percent);
        ui_text(c, col_x(3), ty, text, fg, S);
        static const char *states[] = { "running", "ready", "sleeping", "ended" };
        ui_text(c, col_x(4), ty, states[r->state & 3], dim, S);
    }
    int by = c->h - 46 * S;
    ui_fill(c, 0, by - 10 * S, c->w, 1, 0xDADAE0);
    struct os_meminfo m;
    meminfo(&m);
    char line[96];
    unsigned long used = m.pages_used * 4096 >> 20, total = m.pages_total * 4096 >> 20;
    snprintf(line, sizeof(line), "%d processes    Memory: %lu of %lu MiB used", n_rows, used, total);
    ui_text(c, 16 * S, by + 6 * S, line, UI_DIM, S);
    ui_button(c, c->w - 150 * S, by, 134 * S, 30 * S, "Quit Process", S, 0);
    ui_present(&win);
}

int main(void) {
    S = ui_scale();
    if (ui_open(&win, "Activity Monitor", 620 * S, 460 * S) < 0) {
        printf("activity: the desktop is not running\n");
        return 1;
    }
    last_sample = uptime_ms();
    sample();
    draw();
    for (;;) {
        struct ws_event e;
        if (!ui_wait(&win, &e, 1000)) {
            sample();
            draw();
            continue;
        }
        if (e.type == WS_EV_CLOSE) return 0;
        int top = 44 * S, rh = 24 * S, by = win.c.h - 46 * S;
        if (e.type == WS_EV_DOWN) {
            if (ui_inside(e.x, e.y, win.c.w - 150 * S, by, 134 * S, 30 * S)) {
                if (selected_pid > 0 && selected_pid != getpid()) {
                    sigsend(selected_pid, SIGTERM);
                    selected_pid = -1;
                    sleep_ms(100);
                    sample();
                }
            } else if (e.y >= top && e.y < by - 10 * S) {
                int i = first + (e.y - top) / rh;
                if (i < n_rows) selected_pid = rows[i].pid;
            }
        } else if (e.type == WS_EV_WHEEL) {
            first += e.key * 3;
        } else {
            continue;
        }
        draw();
    }
}
