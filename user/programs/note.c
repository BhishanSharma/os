// note [text] - add a time-stamped line to NOTES.TXT, then show the file
#include <stdio.h>
#include <string.h>
#include <os.h>

#define NOTES "notes.txt"

int main(int argc, char **argv) {
    if (argc > 1) {
        char line[256];
        struct os_time t;
        gettime(&t);
        int n = snprintf(line, sizeof(line), "%d-%02d-%02d %02d:%02d ", t.year, t.month, t.day, t.hour, t.minute);
        for (int i = 1; i < argc && n < (int)sizeof(line) - 2; i++)
            n += snprintf(line + n, sizeof(line) - n, "%s%s", argv[i], i + 1 < argc ? " " : "");
        line[n++] = '\n';

        int fd = open(NOTES, OPEN_APPEND);
        if (fd < 0 || write(fd, line, n) != n || close(fd) != 0) {
            printf("note: cannot write %s\n", NOTES);
            return 1;
        }
    }

    int fd = open(NOTES, OPEN_READ);
    if (fd < 0) {
        printf("No notes yet. Add one with: note <text>\n");
        return 0;
    }
    char buf[512];
    long n;
    while ((n = read(fd, buf, sizeof(buf))) > 0) write(1, buf, n);
    close(fd);
    return 0;
}
