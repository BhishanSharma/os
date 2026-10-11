// echod - a port service: sends every message back to whoever asked
// (registered as "echo"; launchd keeps it running)
#include <stdio.h>
#include <os.h>

static char buf[1024 * 1024];

int main(void) {
    int port = port_create();
    if (port < 0 || port_register("echo", port) != 0) {
        printf("echod: cannot register the echo service\n");
        return 1;
    }
    for (;;) {
        int reply;
        int n = port_receive(port, buf, sizeof(buf), 0, &reply);
        if (n >= 0 && reply >= 0) port_send(reply, buf, (size_t)n, -1);
    }
}
