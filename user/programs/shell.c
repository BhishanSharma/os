// shell - the command line, as an ordinary user-mode program.
//
// The kernel logs you in and starts this program as you (like login and
// bash on Linux). It reads a line, then:
//   1. runs it itself if it is a shell command (cd, jobs, fg, su, exit, ...),
//   2. else asks the kernel to run it if it is a command built into the kernel
//      (ls, cat, ping, edit, ...: kcommand()),
//   3. else starts the program of that name with spawn() and wait()s for it,
//      or lets it run in the background with a trailing `&`.
// Several commands go on one line with `;`, and `a && b` runs b only if a
// succeeded. $? is the last exit code; $USER, $HOME and $PWD work too.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <os.h>

#define LINE_MAX    200
#define MAX_ARGV    16
#define HISTORY     32
#define MAX_JOBS    16

static struct os_user me;
static struct os_uname sys;
static int last_status;

/* ---- Background jobs ------------------------------------------------------ */

struct job {
    int pid;
    char command[48];
};
static struct job jobs[MAX_JOBS];

static void job_add(int pid, const char *command) {
    for (int i = 0; i < MAX_JOBS; i++)
        if (!jobs[i].pid) {
            jobs[i].pid = pid;
            snprintf(jobs[i].command, sizeof(jobs[i].command), "%s", command);
            return;
        }
}

static struct job *job_find(int pid) {
    for (int i = 0; i < MAX_JOBS; i++)
        if (jobs[i].pid && jobs[i].pid == pid) return &jobs[i];
    return 0;
}

static int job_latest(void) {
    int pid = 0;
    for (int i = 0; i < MAX_JOBS; i++)
        if (jobs[i].pid > pid) pid = jobs[i].pid;
    return pid;
}

/* Explain how a program ended; nothing for exit code 0 or Ctrl+C. The kernel
 * itself reports crashes (segmentation fault, ...). */
static void report_exit(const char *name, int pid, int code) {
    if (code == 137)
        printf("[%d] %s: killed\n", pid, name);
    else if (code != 0 && code < 128)
        printf("[%s exited with code %d]\n", name, code);
}

/* Before each prompt: tell about background programs that have ended. */
static void collect_jobs(void) {
    int status, pid;
    while ((pid = wait(-1, &status, WAIT_NOHANG)) > 0) {
        struct job *j = job_find(pid);
        const char *what = j ? j->command : "program";
        if (status == 137)
            printf("[%d] Killed: %s\n", pid, what);
        else
            printf("[%d] Done: %s (exit code %d)\n", pid, what, status);
        if (j) j->pid = 0;
    }
}

/* ---- Line editing --------------------------------------------------------- */

static char history[HISTORY][LINE_MAX];
static int history_count;

static void history_add(const char *line) {
    if (!line[0]) return;
    if (history_count && strcmp(history[(history_count - 1) % HISTORY], line) == 0) return;
    snprintf(history[history_count % HISTORY], LINE_MAX, "%s", line);
    history_count++;
}

static void erase(int n) {
    char bs[LINE_MAX];
    for (int i = 0; i < n; i++) bs[i] = '\b';
    if (n > 0) write(1, bs, (size_t)n);
}

/* One line from the keyboard with Backspace and Up/Down for the history.
 * Returns its length, or -1 for Ctrl+C. */
static int read_line(char *buf, int size) {
    int len = 0, browse = history_count;
    char draft[LINE_MAX] = "";
    while (1) {
        int c = waitkey();
        if (c == '\n' || c == '\r') {
            write(1, "\n", 1);
            buf[len] = 0;
            return len;
        }
        if (c == 3) {                         // Ctrl+C: forget the line
            write(1, "^C\n", 3);
            buf[0] = 0;
            return -1;
        }
        if (c == '\b') {
            if (len > 0) {
                len--;
                erase(1);
            }
            continue;
        }
        if (c == KEYCODE_UP || c == KEYCODE_DOWN) {
            int oldest = history_count > HISTORY ? history_count - HISTORY : 0;
            int to = browse + (c == KEYCODE_UP ? -1 : 1);
            if (to < oldest || to > history_count) continue;
            if (browse == history_count) {    // keep what was typed so far
                buf[len] = 0;
                snprintf(draft, sizeof(draft), "%s", buf);
            }
            browse = to;
            erase(len);
            snprintf(buf, (size_t)size, "%s", browse == history_count ? draft : history[browse % HISTORY]);
            len = (int)strlen(buf);
            write(1, buf, (size_t)len);
            continue;
        }
        if (c >= 32 && c < 127 && len < size - 1) {
            buf[len++] = (char)c;
            char ch = (char)c;
            write(1, &ch, 1);
        }
    }
}

static void prompt(void) {
    char cwd[128];
    if (getcwd(cwd, sizeof(cwd)) < 0) strcpy(cwd, "?");
    int root = me.uid == 0;
    if (cursor_column() != 0) putchar('\n');     // never start the prompt mid-line
    set_theme_color(root ? THEME_BAD : THEME_GOOD);
    printf("%s@%s", me.name, sys.hostname);
    set_theme_color(THEME_TEXT);
    putchar(':');
    set_theme_color(THEME_ACCENT);
    printf("%s", cwd);
    set_theme_color(THEME_TEXT);
    printf(root ? "# " : "$ ");
}

/* ---- Parsing -------------------------------------------------------------- */

/* Replace $?, $USER, $HOME and $PWD (not inside single quotes). */
static void expand(const char *in, char *out, size_t size) {
    size_t n = 0;
    int single = 0;
    while (*in && n + 1 < size) {
        if (*in == '\'') single = !single;
        if (*in == '$' && !single) {
            char value[128];
            const char *rest = 0;
            if (in[1] == '?')                          { snprintf(value, sizeof(value), "%d", last_status); rest = in + 2; }
            else if (strncmp(in + 1, "USER", 4) == 0)  { snprintf(value, sizeof(value), "%s", me.name); rest = in + 5; }
            else if (strncmp(in + 1, "HOME", 4) == 0)  { snprintf(value, sizeof(value), "%s", me.home); rest = in + 5; }
            else if (strncmp(in + 1, "PWD", 3) == 0)   { if (getcwd(value, sizeof(value)) < 0) value[0] = 0; rest = in + 4; }
            if (rest) {
                for (const char *v = value; *v && n + 1 < size; v++) out[n++] = *v;
                in = rest;
                continue;
            }
        }
        out[n++] = *in++;
    }
    out[n] = 0;
}

/* Split `s` into words in place; "double" and 'single' quotes group words. */
static int split(char *s, char **argv) {
    int argc = 0;
    char *out = s;
    while (*s) {
        while (*s == ' ') s++;
        if (!*s) break;
        if (argc == MAX_ARGV - 1) break;
        argv[argc++] = out;
        char quote = 0;
        while (*s && (quote || *s != ' ')) {
            if (!quote && (*s == '"' || *s == '\'')) quote = *s++;
            else if (quote && *s == quote) { quote = 0; s++; }
            else *out++ = *s++;
        }
        if (*s) s++;
        *out++ = 0;
    }
    argv[argc] = 0;
    return argc;
}

static int parse_pid(const char *s) {
    if (!s || *s < '0' || *s > '9') return -1;
    return atoi(s);
}

/* ---- Shell commands ------------------------------------------------------- */

static void cmd_help(void) {
    set_theme_color(THEME_ACCENT);
    printf("Shell (%s, a user-mode program):\n", "SHELL.ELF");
    set_theme_color(THEME_TEXT);
    printf("cd [dir]           - change directory (no dir: your home)\n");
    printf("pwd                - print working directory\n");
    printf("<program> [args]   - run a program; add & to run it in the background\n");
    printf("run <file> [args]  - run a program by file name\n");
    printf("a ; b    a && b    - several commands (&&: only if the first succeeds)\n");
    printf("jobs / ps          - your background programs / every task\n");
    printf("fg [pid]           - bring a background program to the foreground\n");
    printf("kill <pid>         - stop a program\n");
    printf("su [user]          - a shell as another user (default root); exit to go back\n");
    printf("history            - the commands typed so far\n");
    printf("echo $? $USER $HOME $PWD - last exit code and session variables\n");
    printf("exit [code] / logout - leave this shell\n\n");
    kcommand("help");
}

static const char *state_text(const struct os_task *t) {
    switch (t->state) {
        case TASKSTATE_RUNNING:  return "running";
        case TASKSTATE_READY:    return "ready";
        case TASKSTATE_SLEEPING: return "sleeping";
        default:                 return "done";
    }
}

static void cmd_ps(void) {
    struct os_task t;
    printf("  %5s %5s  %-9s %-9s %9s  %s\n", "PID", "PPID", "USER", "STATE", "CPU", "NAME");
    for (int slot = 0; ; slot++) {
        int r = taskinfo(slot, &t);
        if (r < 0) break;
        if (r == 0) continue;
        printf("  %5d %5d  %-9s %-9s %5u.%02us  %s%s\n", t.pid, t.pid ? t.parent : 0, t.user, state_text(&t),
               t.cpu_ms / 1000, (t.cpu_ms % 1000) / 10, t.name, t.pid == getpid() ? "  (this shell)" : "");
    }
}

static int task_by_pid(int pid, struct os_task *out) {
    for (int slot = 0; ; slot++) {
        int r = taskinfo(slot, out);
        if (r < 0) return 0;
        if (r == 1 && out->pid == pid) return 1;
    }
}

static void cmd_jobs(void) {
    int shown = 0;
    struct os_task t;
    for (int i = 0; i < MAX_JOBS; i++) {
        if (!jobs[i].pid) continue;
        const char *state = task_by_pid(jobs[i].pid, &t) ? state_text(&t) : "done";
        printf("  [%d] %-9s %s\n", jobs[i].pid, state, jobs[i].command);
        shown++;
    }
    if (!shown) printf("  (no background programs; start one with `name &`)\n");
}

static int cmd_kill(const char *arg) {
    int pid = parse_pid(arg);
    if (pid < 0) {
        printf("Usage: kill <pid>   (see `jobs` or `ps`)\n");
        return 1;
    }
    struct os_task t;
    int known = task_by_pid(pid, &t);
    int r = kill(pid);
    if (r == 0) {
        printf("Sent kill to %d (%s)\n", pid, known ? t.name : "?");
        return 0;
    }
    if (r == SYSERR_PERM && known && !t.is_program)
        printf("kill: %d is the %s, part of the kernel: it cannot be stopped\n", pid, t.name);
    else if (r == SYSERR_PERM && known)
        printf("kill: %d belongs to %s: permission denied\n", pid, t.user);
    else
        printf("kill: %d: %s\n", pid, os_strerror(r));
    return 1;
}

static int cmd_fg(const char *arg) {
    int pid = arg ? parse_pid(arg) : job_latest();
    if (pid <= 0) {
        printf("fg: no background programs (see `jobs`)\n");
        return 1;
    }
    struct job *j = job_find(pid);
    char name[48];
    snprintf(name, sizeof(name), "%s", j ? j->command : "program");
    printf("%s (pid %d) is now in the foreground. Ctrl+C stops it.\n", name, pid);
    int status;
    if (wait(pid, &status, WAIT_FOREGROUND) < 0) {
        printf("fg: %d is not a program started from this shell\n", pid);
        return 1;
    }
    if (j) j->pid = 0;
    report_exit(name, pid, status);
    return status;
}

static int cmd_cd(const char *dir) {
    if (!dir || strcmp(dir, "~") == 0) dir = me.home;
    if (chdir(dir) != 0) {
        printf("cd: %s: no such directory\n", dir);
        return 1;
    }
    return 0;
}

static int cmd_su(const char *user) {
    if (!user) user = "root";
    char a0[] = "shell", a1[] = "-su";
    char *argv[] = { a0, a1, 0 };
    int pid = spawn("shell", argv, user);
    if (pid < 0) {
        if (pid == SYSERR_NOUSER) printf("su: no user %s\n", user);
        else printf("su: %s\n", os_strerror(pid));
        return 1;
    }
    int status;
    wait(pid, &status, WAIT_FOREGROUND);
    printf("Back to %s.\n", me.name);
    return 0;
}

/* Run a program: argv[0] is its name. */
static int run_program(char **argv, int background, const char *command) {
    int pid = spawn(argv[0], argv, 0);
    if (pid < 0) {
        if (pid == SYSERR_NOENT)
            printf("%s: unknown command (`help` lists them, `programs` the programs)\n", argv[0]);
        else
            printf("%s: %s\n", argv[0], os_strerror(pid));
        return pid == SYSERR_NOENT ? 127 : 126;
    }
    if (background) {
        job_add(pid, command);
        printf("[%d] %s running in the background\n", pid, argv[0]);
        return 0;
    }
    int status = 0;
    wait(pid, &status, WAIT_FOREGROUND);
    report_exit(argv[0], pid, status);
    return status;
}

/* One command (no ; or &&). Returns its exit status. */
static int run_command(const char *raw, int background) {
    char expanded[LINE_MAX * 2], *text = expanded;
    expand(raw, expanded, sizeof(expanded));    // now, so $? is the previous command's
    while (*text == ' ') text++;
    size_t n = strlen(text);
    while (n && text[n - 1] == ' ') text[--n] = 0;
    if (!*text) return last_status;

    char words[LINE_MAX], *argv[MAX_ARGV];
    snprintf(words, sizeof(words), "%s", text);
    int argc = split(words, argv);
    if (argc == 0) return last_status;
    const char *cmd = argv[0], *arg = argc > 1 ? argv[1] : 0;

    if (strcmp(cmd, "exit") == 0 || strcmp(cmd, "logout") == 0) exit(arg ? atoi(arg) : 0);
    if (strcmp(cmd, "cd") == 0)      return cmd_cd(arg);
    if (strcmp(cmd, "pwd") == 0)     { char cwd[128]; getcwd(cwd, sizeof(cwd)); printf("%s\n", cwd); return 0; }
    if (strcmp(cmd, "help") == 0)    { cmd_help(); return 0; }
    if (strcmp(cmd, "ps") == 0)      { cmd_ps(); return 0; }
    if (strcmp(cmd, "jobs") == 0)    { cmd_jobs(); return 0; }
    if (strcmp(cmd, "kill") == 0)    return cmd_kill(arg);
    if (strcmp(cmd, "fg") == 0)      return cmd_fg(arg);
    if (strcmp(cmd, "su") == 0)      return cmd_su(arg);
    if (strcmp(cmd, "history") == 0) {
        int first = history_count > HISTORY ? history_count - HISTORY : 0;
        for (int i = first; i < history_count; i++) printf("%5d  %s\n", i + 1, history[i % HISTORY]);
        return 0;
    }
    if (strcmp(cmd, "run") == 0) {
        if (argc < 2) { printf("Usage: run <file> [arguments] [&]\n"); return 2; }
        return run_program(argv + 1, background, text + 4);
    }

    // Built into the kernel? (ls, cat, edit, ping, ...) They run in the foreground.
    if (!background) {
        int r = kcommand(text);
        if (r == 1) return 0;
    }
    return run_program(argv, background, text);
}

/* A whole line: commands separated by ;  &&  and & (background). */
static void run_line(const char *line) {
    char text[LINE_MAX];
    snprintf(text, sizeof(text), "%s", line);
    char *s = text, *start = text;
    int skip = 0;                     // after a failed `a && ...`
    char quote = 0;
    while (1) {
        char c = *s;
        if (quote) {
            if (c == quote) quote = 0;
            if (c) { s++; continue; }
        }
        if (c == '"' || c == '\'') { quote = c; s++; continue; }
        if (c && c != ';' && c != '&') { s++; continue; }

        int background = 0, and_then = 0;
        if (c == '&' && s[1] == '&') and_then = 1;
        else if (c == '&') background = 1;
        *s = 0;
        if (!skip) last_status = run_command(start, background);
        skip = and_then && last_status != 0;
        if (!c) break;
        s += and_then ? 2 : 1;
        start = s;
    }
}

int main(int argc, char **argv) {
    getuser(&me);
    uname(&sys);
    ctrlc(CTRLC_KEY);                 // Ctrl+C clears the line instead of ending the shell
    if (argc > 1 && strcmp(argv[1], "-su") == 0)
        printf("Now %s. Type exit to go back.\n", me.name);

    char line[LINE_MAX];
    while (1) {
        collect_jobs();
        prompt();
        if (read_line(line, sizeof(line)) < 0) continue;
        history_add(line);
        run_line(line);
    }
}
