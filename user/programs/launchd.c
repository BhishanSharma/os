// launchd - the first program: starts the system's services and keeps them
// running, like macOS's launchd. The kernel starts it as root after boot.
//
// Services come from LAUNCHD.CFG in the system folder (/SYS, or / on the RAM
// disk), one per line:   name  program [arguments]  [console|keepalive|once]
//   console    owns the keyboard and screen (loginwindow, the graphical login,
//              which is the text `login` without graphics); restarted when it ends
//   keepalive  restarted whenever it ends (a crash too)
//   once       started at boot, left alone
#include <stdio.h>
#include <string.h>
#include <os.h>

#define MAX_SERVICES 16
#define MAX_WORDS 8

typedef struct {
    char line[128];
    char *argv[MAX_WORDS + 1];
    char mode[12];
    int pid, starts;
} service_t;

static service_t services[MAX_SERVICES];
static int count;

static const char *defaults =
    "loginwindow loginwindow console\n"
    "echod echod keepalive\n";

static void parse(const char *text) {
    while (*text && count < MAX_SERVICES) {
        service_t *s = &services[count];
        int n = 0;
        while (*text && *text != '\n' && n < (int)sizeof(s->line) - 1) s->line[n++] = *text++;
        s->line[n] = 0;
        if (*text == '\n') text++;
        if (!s->line[0] || s->line[0] == '#') continue;
        char *w[MAX_WORDS + 2];
        int k = 0;
        for (char *p = s->line; *p && k < MAX_WORDS + 2;) {
            while (*p == ' ' || *p == '\t' || *p == '\r') *p++ = 0;
            if (!*p) break;
            w[k++] = p;
            while (*p && *p != ' ' && *p != '\t' && *p != '\r') p++;
        }
        if (k < 2) continue;
        const char *last = w[k - 1];
        if (!strcmp(last, "console") || !strcmp(last, "keepalive") || !strcmp(last, "once")) {
            snprintf(s->mode, sizeof(s->mode), "%s", last);
            k--;
        } else {
            snprintf(s->mode, sizeof(s->mode), "once");
        }
        for (int i = 1; i < k; i++) s->argv[i - 1] = w[i];   /* w[0] is the service's name */
        s->argv[k - 1] = 0;
        count++;
    }
}

static void load(void) {
    static char text[2048];
    const char *paths[] = { "/SYS/LAUNCHD.CFG", "/LAUNCHD.CFG" };
    for (int i = 0; i < 2; i++) {
        int fd = open(paths[i], OPEN_READ);
        if (fd < 0) continue;
        long n = read(fd, text, sizeof(text) - 1);
        close(fd);
        if (n > 0) {
            text[n] = 0;
            parse(text);
            return;
        }
    }
    parse(defaults);
}

static void start(service_t *s) {
    s->pid = spawn(s->argv[0], s->argv, 0);
    s->starts++;
    if (s->pid < 0) printf("launchd: cannot start %s: %s\n", s->argv[0], os_strerror(s->pid));
}

/* The console service runs on a thread of its own, which waits for it with
 * the keyboard handed over (a foreground wait), and starts it again. */
static void console_loop(void *arg) {
    service_t *s = arg;
    for (;;) {
        start(s);
        if (s->pid < 0) {
            sleep_ms(2000);
            continue;
        }
        int status;
        wait(s->pid, &status, WAIT_FOREGROUND);
        clear_screen();
    }
}

int main(void) {
    load();
    for (int i = 0; i < count; i++) {
        if (!strcmp(services[i].mode, "console")) thread_create(console_loop, &services[i]);
        else start(&services[i]);
    }
    for (;;) {                                         /* restart what has to keep running */
        int status, pid;
        while ((pid = wait(-1, &status, WAIT_NOHANG)) > 0)
            for (int i = 0; i < count; i++) {
                service_t *s = &services[i];
                if (s->pid != pid) continue;
                s->pid = 0;
                if (!strcmp(s->mode, "keepalive")) {
                    if (s->starts > 50) {
                        printf("launchd: %s keeps ending; giving up\n", s->argv[0]);
                        continue;
                    }
                    start(s);
                }
            }
        sleep_ms(200);
    }
}
