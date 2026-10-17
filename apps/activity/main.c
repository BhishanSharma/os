// activity - Activity Monitor: the programs running, their CPU use and
// memory, and a button to quit one.
//
// Built from views (view.h): a table that asks for its cells, a status line
// and a button. Every second the data is sampled again; the table compares
// each row with what it shows and redraws only the rows whose numbers changed.
#include <stdio.h>
#include <string.h>
#include <os.h>
#include <view.h>

#define MAX_ROWS 64

typedef struct {
    int pid, parent, state, program;
    unsigned cpu_ms;
    int percent;
    char name[16], user[16];
} row_t;

static row_t rows[MAX_ROWS], old[MAX_ROWS];
static int n_rows, n_old, selected_pid = -1;
static unsigned long last_sample;
static view_t *table, *status;

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

/* What the views show, from the data. */
static void update(void) {
    v_table_rows(table, n_rows);
    int sel = -1;                                  /* the selection follows its process */
    for (int i = 0; i < n_rows; i++)
        if (rows[i].pid == selected_pid) sel = i;
    v_table_select(table, sel);
    struct os_meminfo m;
    meminfo(&m);
    char line[96];
    unsigned long used = m.pages_used * 4096 >> 20, total = m.pages_total * 4096 >> 20;
    snprintf(line, sizeof(line), "%d processes    Memory: %lu of %lu MiB used", n_rows, used, total);
    v_set_text(status, line);
}

static void cell(view_t *t, int row, int col, char *out, int max) {
    (void)t;
    static const char *states[] = { "running", "ready", "sleeping", "ended" };
    const row_t *r = &rows[row];
    switch (col) {
    case 0: snprintf(out, max, "%s", r->name); break;
    case 1: snprintf(out, max, "%d", r->pid); break;
    case 2: snprintf(out, max, "%s", r->user); break;
    case 3: snprintf(out, max, "%d", r->percent > 100 ? 100 : r->percent); break;
    default: snprintf(out, max, "%s", states[r->state & 3]); break;
    }
}

static void picked(view_t *t, int row) {
    (void)t;
    selected_pid = rows[row].pid;
}

static void quit_process(view_t *b) {
    (void)b;
    if (selected_pid <= 0 || selected_pid == getpid()) return;
    sigsend(selected_pid, SIGTERM);
    selected_pid = -1;
    sleep_ms(100);
    sample();
    update();
}

static void tick(void) {
    sample();
    update();
}

int main(void) {
    int S = ui_scale();
    ui_window win;
    if (ui_open(&win, "Activity Monitor", 620 * S, 460 * S) < 0) {
        printf("activity: the desktop is not running\n");
        return 1;
    }
    ui_set_resizable(&win);

    view_t *root = v_box(0, V_COLUMN);
    root->pad = root->gap = 0;
    static const char *heads[] = { "Process", "PID", "User", "% CPU", "State" };
    static const int widths[] = { 204, 70, 90, 90, 90 };
    table = v_table(root, 5, heads, widths, cell);
    table->on_select = picked;
    view_t *bar = v_box(root, V_ROW);
    bar->pad = 10;
    bar->align = V_CENTER;
    status = v_label(bar, "");
    status->color = UI_C_SECONDARY;
    status->flex = 1;
    v_button(bar, "Quit Process", quit_process);

    last_sample = uptime_ms();
    sample();
    update();
    return v_run(&win, root, 1000, tick);
}
