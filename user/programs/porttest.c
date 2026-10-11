// porttest - messages through ports: 1000 round trips to the echo service and
// one 1 MiB message (starts echod if launchd has not)
#include <stdio.h>
#include <string.h>
#include <os.h>

static char out[1024 * 1024], in[1024 * 1024];

int main(void) {
    int echo = port_lookup("echo");
    if (echo < 0) {
        char *args[] = { "echod", 0 };
        spawn("echod", args, 0);
        for (int i = 0; i < 50 && echo < 0; i++) {
            sleep_ms(20);
            echo = port_lookup("echo");
        }
    }
    if (echo < 0) {
        printf("porttest: no echo service\n");
        return 1;
    }
    unsigned long start = uptime_ms();
    int ok = 1;
    for (int i = 0; i < 1000 && ok; i++) {
        char msg[16], back[16];
        int n = snprintf(msg, sizeof(msg), "ping %d", i);
        ok = port_call(echo, msg, (size_t)n + 1, back, sizeof(back), 2000) == n + 1 && strcmp(msg, back) == 0;
    }
    printf("1000 round trips: %s in %lu ms\n", ok ? "ok" : "FAILED", uptime_ms() - start);
    for (int i = 0; i < (int)sizeof(out); i++) out[i] = (char)(i * 7);
    int n = port_call(echo, out, sizeof(out), in, sizeof(in), 5000);
    int big = n == (int)sizeof(out) && memcmp(out, in, sizeof(out)) == 0;
    printf("1 MiB message: %s\n", big ? "ok" : "FAILED");
    printf(ok && big ? "porttest: all good\n" : "porttest: FAILED\n");
    return ok && big ? 0 : 1;
}
