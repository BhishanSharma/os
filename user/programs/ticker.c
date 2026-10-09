// ticker [seconds] [interval] - print the time every few seconds; try `ticker 30 &`
#include <stdio.h>
#include <stdlib.h>
#include <os.h>

int main(int argc, char **argv) {
    int seconds = argc > 1 ? atoi(argv[1]) : 10;
    int every = argc > 2 ? atoi(argv[2]) : 1;
    if (seconds < 1) seconds = 1;
    if (every < 1) every = 1;
    int pid = getpid();
    for (int left = seconds; left > 0; left -= every) {
        struct os_time t;
        gettime(&t);
        printf("[ticker %d] %02d:%02d:%02d  (%d s left)\n", pid, t.hour, t.minute, t.second, left);
        sleep_ms((unsigned long)every * 1000);
    }
    printf("[ticker %d] done\n", pid);
    return 0;
}
