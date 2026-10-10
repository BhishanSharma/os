// httpd [port] - a small web server: serves the files of the current folder
//
// One connection at a time: read the request line, answer with the file (or
// a listing for "/"), close. Ctrl+C stops it.
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <os.h>

static void send_text(int s, const char *status, const char *type, const char *body, long len) {
    char head[256];
    int n = snprintf(head, sizeof(head), "HTTP/1.0 %s\r\nContent-Type: %s\r\nContent-Length: %ld\r\n"
                                         "Server: TerminalOS-httpd\r\nConnection: close\r\n\r\n", status, type, len);
    net_send(s, head, (size_t)n);
    if (len) net_send(s, body, (size_t)len);
}

static const char *content_type(const char *name) {
    const char *dot = strchr(name, '.');
    if (!dot) return "application/octet-stream";
    if (!strcmp(dot, ".HTM") || !strcmp(dot, ".htm") || !strcmp(dot, ".HTML")) return "text/html";
    if (!strcmp(dot, ".TXT") || !strcmp(dot, ".txt") || !strcmp(dot, ".C") || !strcmp(dot, ".c")) return "text/plain";
    if (!strcmp(dot, ".PNG") || !strcmp(dot, ".png")) return "image/png";
    if (!strcmp(dot, ".BMP") || !strcmp(dot, ".bmp")) return "image/bmp";
    if (!strcmp(dot, ".WAV") || !strcmp(dot, ".wav")) return "audio/wav";
    return "application/octet-stream";
}

static void serve(int s) {
    char req[1024];
    long got = 0, n;
    while (got < (long)sizeof(req) - 1 && (n = net_recv(s, req + got, sizeof(req) - 1 - (size_t)got, 5000)) > 0) {
        got += n;
        req[got] = 0;
        if (strstr(req, "\r\n\r\n") || strstr(req, "\n\n")) break;
    }
    req[got] = 0;
    char method[8] = "", path[128] = "";
    int i = 0, j = 0;
    while (req[i] && req[i] != ' ' && j < 7) method[j++] = req[i++];
    method[j] = 0;
    while (req[i] == ' ') i++;
    j = 0;
    while (req[i] && req[i] != ' ' && req[i] != '\r' && j < 127) path[j++] = req[i++];
    path[j] = 0;
    printf("%s %s\n", method, path);
    if (strcmp(method, "GET")) {
        send_text(s, "405 Method Not Allowed", "text/plain", "only GET\n", 9);
        return;
    }
    if (!strcmp(path, "/")) {                          /* a listing of the folder */
        static char page[8192];
        int len = snprintf(page, sizeof(page), "<html><body><h1>Terminal OS</h1><ul>\n");
        struct os_dirent e;
        for (int k = 0; readdir(k, &e) == 1 && len < (int)sizeof(page) - 200; k++)
            if (!e.is_dir) len += snprintf(page + len, sizeof(page) - (size_t)len, "<li><a href=\"/%s\">%s</a> (%ld bytes)</li>\n",
                                           e.name, e.name, (long)e.size);
        len += snprintf(page + len, sizeof(page) - (size_t)len, "</ul></body></html>\n");
        send_text(s, "200 OK", "text/html", page, len);
        return;
    }
    int fd = open(path + 1, OPEN_READ);
    if (fd < 0) {
        send_text(s, "404 Not Found", "text/plain", "not found\n", 10);
        return;
    }
    static char data[256 * 1024];
    long len = 0;
    while (len < (long)sizeof(data) && (n = read(fd, data + len, sizeof(data) - (size_t)len)) > 0) len += n;
    close(fd);
    send_text(s, "200 OK", content_type(path), data, len);
}

int main(int argc, char **argv) {
    int port = argc > 1 ? atoi(argv[1]) : 8080;
    int l = net_listen(port);
    if (l < 0) {
        printf("httpd: port %d: %s\n", port, net_strerror(l));
        return 1;
    }
    printf("Serving this folder on port %d (Ctrl+C stops).\n", port);
    for (;;) {
        int c = net_accept(l, 0);
        if (c < 0) {
            printf("httpd: %s\n", net_strerror(c));
            break;
        }
        serve(c);
        net_close(c);
    }
    net_close(l);
    return 0;
}
