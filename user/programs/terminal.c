// terminal [command] - a shell in a window on the desktop
//
// The shell (`shell -t`) reads the lines typed here from a pipe and writes
// into another pipe, which a second thread reads and draws as text. With a
// command, it is typed first (Files opens text files with `terminal cat x`).
#include <stdio.h>
#include <string.h>
#include <os.h>
#include <ui.h>

#define COLS 80
#define ROWS 25
#define BG 0x1E1E24
#define FG 0xE6E6E6
#define PROMPT_FG 0x7FD1FF

static ui_window win;
static char grid[ROWS][COLS];
static int row, col;
static mutex_t lock;
static volatile int dirty = 1;
static int to_shell, from_shell, shell_pid;
static char line[256];
static int line_len;
static int secret;                     /* typing after "Password: ": not shown */

static void scroll(void) {
    memmove(grid[0], grid[1], sizeof(grid) - COLS);
    memset(grid[ROWS - 1], ' ', COLS);
    row = ROWS - 1;
}

static void put(char c) {
    if (c == '\r') {
        col = 0;
    } else if (c == '\n') {
        col = 0;
        if (++row == ROWS) scroll();
    } else if (c == '\b') {
        if (col > 0) grid[row][--col] = ' ';
    } else if (c == '\t') {
        do put(' '); while (col % 4);
    } else if ((unsigned char)c >= ' ') {
        if (col == COLS) put('\n');
        grid[row][col++] = c;
    }
}

/* What the shell and its programs print. */
static void reader(void *arg) {
    char buf[256];
    for (;;) {
        long n = read(from_shell, buf, sizeof(buf));
        if (n <= 0) exit(0);                            /* the shell has ended */
        mutex_lock(&lock);
        for (long i = 0; i < n; i++) put(buf[i]);
        dirty = 1;
        mutex_unlock(&lock);
    }
}

static void draw(void) {
    int S = win.scale, cw = 8 * S, ch = 16 * S, m = 4 * S;
    ui_fill(&win.c, 0, 0, win.c.w, win.c.h, BG);
    for (int r = 0; r < ROWS; r++)
        for (int c = 0; c < COLS; c++)
            if (grid[r][c] != ' ') ui_char(&win.c, m + c * cw, m + r * ch, (unsigned char)grid[r][c], FG, S);
    ui_fill(&win.c, m + col * cw, m + row * ch, cw, ch, PROMPT_FG);      /* the cursor */
    ui_present(&win);
}

static void send_line(const char *text) {
    write(to_shell, text, strlen(text));
    write(to_shell, "\n", 1);
}

/* Ctrl+C: interrupt the programs the shell is running -> how many. */
static int interrupt(void) {
    struct os_task t;
    int n = 0;
    for (int i = 0, used; (used = taskinfo(i, &t)) >= 0; i++)
        if (used && t.pid > 0 && t.parent == shell_pid && t.pid != shell_pid && sigsend(t.pid, SIGINT) == 0) n++;
    return n;
}

static int start_shell(void) {
    int in[2], out[2];
    if (pipe(in) < 0 || pipe(out) < 0) return -1;
    int saved[3] = { dup(0), dup(1), dup(2) };
    dup2(in[0], 0);
    dup2(out[1], 1);
    dup2(out[1], 2);
    char *argv[] = { "shell", "-t", 0 };
    int pid = shell_pid = spawn("shell", argv, 0);
    for (int i = 0; i < 3; i++) {
        dup2(saved[i], i);
        close(saved[i]);
    }
    close(in[0]);
    close(out[1]);
    to_shell = in[1];
    from_shell = out[0];
    return pid;
}

int main(int argc, char **argv) {
    int S = ui_scale();
    if (ui_open(&win, "Terminal", COLS * 8 * S + 8 * S, ROWS * 16 * S + 8 * S) < 0) {
        printf("terminal: the desktop is not running (type windowserver)\n");
        return 1;
    }
    memset(grid, ' ', sizeof(grid));
    if (start_shell() < 0) {
        printf("terminal: cannot start the shell\n");
        return 1;
    }
    thread_create(reader, 0);
    if (argc > 1) {
        char cmd[256] = "";
        for (int i = 1; i < argc; i++) {
            if (i > 1) strcat(cmd, " ");
            if (strlen(cmd) + strlen(argv[i]) + 2 < sizeof(cmd)) strcat(cmd, argv[i]);
        }
        send_line(cmd);
    }
    for (;;) {
        struct ws_event e;
        while (ui_poll(&win, &e)) {
            if (e.type == WS_EV_CLOSE) exit(0);
            if (e.type != WS_EV_KEY) continue;
            int k = e.key;
            mutex_lock(&lock);
            if (k == 3) {                                /* Ctrl+C */
                put('^');
                put('C');
                put('\n');
                line_len = 0;
                if (interrupt() == 0) send_line("\x03");   /* a kernel command's question, or a new prompt */
            } else if (k == '\n' || k == '\r') {
                line[line_len] = 0;
                put('\n');
                send_line(line);
                line_len = 0;
            } else if (k == '\b') {
                if (line_len > 0) {
                    line_len--;
                    if (!secret) put('\b');
                }
            } else if (k >= ' ' && k < 127 && line_len < (int)sizeof(line) - 1) {
                if (!line_len) secret = col >= 9 && !memcmp(&grid[row][col - 9], "assword: ", 9);
                line[line_len++] = (char)k;
                if (!secret) put((char)k);
            }
            dirty = 1;
            mutex_unlock(&lock);
        }
        if (dirty) {
            mutex_lock(&lock);
            dirty = 0;
            draw();
            mutex_unlock(&lock);
        }
        sleep_ms(15);
    }
}
