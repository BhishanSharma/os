// fetch <host> [path] [port] - an HTTP GET with the socket calls: prints the reply
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <os.h>

int main(int argc, char **argv) {
    if (argc < 2) {
        printf("usage: fetch <host> [path] [port]   e.g. fetch example.com /\n");
        return 1;
    }
    const char *host = argv[1], *path = argc > 2 ? argv[2] : "/";
    int port = argc > 3 ? atoi(argv[3]) : 80;
    unsigned char ip[4];
    int r = resolve(host, ip);
    if (r < 0) {
        printf("fetch: %s: %s\n", host, net_strerror(r));
        return 1;
    }
    printf("Connecting to %s (%d.%d.%d.%d) port %d...\n", host, ip[0], ip[1], ip[2], ip[3], port);
    int s = net_connect(ip, port);
    if (s < 0) {
        printf("fetch: %s\n", net_strerror(s));
        return 1;
    }
    char req[512];
    int n = snprintf(req, sizeof(req), "GET %s HTTP/1.0\r\nHost: %s\r\nUser-Agent: fetch/1.0\r\nConnection: close\r\n\r\n",
                     path, host);
    if (net_send(s, req, (size_t)n) != n) {
        printf("fetch: could not send the request\n");
        net_close(s);
        return 1;
    }
    char buf[1024];
    long total = 0, got;
    while ((got = net_recv(s, buf, sizeof(buf) - 1, 15000)) > 0) {
        buf[got] = 0;
        printf("%s", buf);
        total += got;
    }
    net_close(s);
    printf("\n-- %ld bytes%s\n", total, got < 0 ? ", then an error" : "");
    return got < 0;
}
