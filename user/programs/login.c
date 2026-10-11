// login - asks for a name and password and starts that user's shell in their
// home folder (launchd keeps it running: logout brings it back)
#include <stdio.h>
#include <string.h>
#include <os.h>

static int read_word(char *buf, int size) {
    long n = read(0, buf, (size_t)size - 1);
    if (n <= 0) return -1;
    buf[n] = 0;
    while (n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == ' ')) buf[--n] = 0;
    return (int)n;
}

int main(void) {
    struct os_uname sys;
    uname(&sys);
    for (;;) {
        printf("\n  ");
        set_theme_color(THEME_ACCENT);
        printf("%s %s", sys.sysname, sys.release);
        set_theme_color(THEME_TEXT);
        printf(" (%s)\n\n", sys.hostname);
        char name[32], password[64];
        struct os_user u;
        for (;;) {
            printf("%s login: ", sys.hostname);
            if (read_word(name, sizeof(name)) <= 0) continue;
            printf("Password: ");
            console_echo(0);
            int got = read_word(password, sizeof(password));
            console_echo(1);
            int ok = got >= 0 && auth(name, password, &u) == 0;
            memset(password, 0, sizeof(password));
            if (ok) break;
            printf("Login incorrect\n\n");
        }
        if (chdir(u.home) != 0) chdir("/");
        printf("\nWelcome, %s. ", u.name);
        if (u.uid == 0) printf("You are the administrator: be careful.\n");
        else printf("Your files go in %s.\n", u.home);
        printf("Type ");
        set_theme_color(THEME_ACCENT);
        printf("help");
        set_theme_color(THEME_TEXT);
        printf(" for commands, ");
        set_theme_color(THEME_ACCENT);
        printf("programs");
        set_theme_color(THEME_TEXT);
        printf(" for programs, ");
        set_theme_color(THEME_ACCENT);
        printf("sysinfo");
        set_theme_color(THEME_TEXT);
        printf(" for this machine.\n\n");
        char *args[] = { "shell", 0 };
        int pid = spawn("shell", args, u.name), status;
        if (pid < 0) {
            printf("login: cannot start the shell: %s\n", os_strerror(pid));
            sleep_ms(2000);
            return 1;
        }
        wait(pid, &status, WAIT_FOREGROUND);
        chdir("/");
        return 0;                                      /* launchd starts a fresh login */
    }
}
