// netcheck: the network calls of the mlibc port, one step at a time.
// Usage: netcheck [host] [port]   (default example.com 80)
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
#include <arpa/inet.h>

static void step(const char *what) {
    printf("netcheck: %s\n", what);
    fflush(stdout);
}

int main(int argc, char **argv) {
    const char *host = argc > 1 ? argv[1] : "example.com";
    const char *port = argc > 2 ? argv[2] : "80";

    step("reading /dev/urandom");
    int r = open("/dev/urandom", O_RDONLY);
    unsigned char rnd[8] = {0};
    printf("  fd %d, read %zd\n", r, r >= 0 ? read(r, rnd, sizeof(rnd)) : -1);
    if (r >= 0) close(r);

    step("getaddrinfo");
    struct addrinfo hints = {0}, *res = NULL;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    int e = getaddrinfo(host, port, &hints, &res);
    if (e) {
        printf("  failed: %s\n", gai_strerror(e));
        return 1;
    }
    char ip[32];
    struct sockaddr_in *in = (struct sockaddr_in *)res->ai_addr;
    inet_ntop(AF_INET, &in->sin_addr, ip, sizeof(ip));
    printf("  %s -> %s port %d\n", host, ip, ntohs(in->sin_port));

    step("socket + O_NONBLOCK");
    int s = socket(AF_INET, SOCK_STREAM, 0);
    printf("  fd %d\n", s);
    fcntl(s, F_SETFL, fcntl(s, F_GETFL) | O_NONBLOCK);

    step("connect");
    if (connect(s, res->ai_addr, res->ai_addrlen) != 0 && errno != EINPROGRESS) {
        printf("  failed: %s\n", strerror(errno));
        return 1;
    }

    step("poll for writable");
    struct pollfd p = {s, POLLOUT, 0};
    printf("  poll -> %d, revents %#x\n", poll(&p, 1, 5000), p.revents);

    char req[256];
    int n = snprintf(req, sizeof(req), "GET / HTTP/1.1\r\nHost: %s\r\nConnection: close\r\n\r\n", host);
    step("send");
    printf("  sent %zd of %d\n", send(s, req, n, 0), n);

    step("poll for readable, then recv");
    long total = 0;
    char buf[4096];
    for (;;) {
        p.events = POLLIN;
        int pr = poll(&p, 1, 10000);
        if (pr <= 0) {
            printf("  poll -> %d (timeout)\n", pr);
            break;
        }
        ssize_t got = recv(s, buf, sizeof(buf), 0);
        if (got < 0 && errno == EAGAIN) continue;
        if (got <= 0) {
            printf("  recv -> %zd (%s)\n", got, got < 0 ? strerror(errno) : "end");
            break;
        }
        if (!total) printf("  first line: %.*s\n", (int)(strchr(buf, '\r') ? strchr(buf, '\r') - buf : 40), buf);
        total += got;
    }
    printf("  %ld bytes\n", total);
    close(s);
    freeaddrinfo(res);
    step("done");
    return 0;
}
