// date [-u] - the date and time (local, or UTC with -u)
#include <stdio.h>
#include <string.h>
#include <os.h>

int main(int argc, char **argv) {
    int utc = argc > 1 && strcmp(argv[1], "-u") == 0;
    struct os_time t;
    if ((utc ? gettime_utc(&t) : gettime(&t)) != 0) {
        printf("date: real-time clock unreadable\n");
        return 1;
    }
    printf("%d-%02d-%02d %02d:%02d:%02d %s\n", t.year, t.month, t.day, t.hour, t.minute, t.second, t.zone);
    return 0;
}
