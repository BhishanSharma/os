// spin [seconds] - keep the CPU busy without ever giving it up voluntarily.
// The timer still switches to other programs and the shell: try `spin 20 &`,
// then `ps` to watch the CPU time grow.
#include <stdio.h>
#include <stdlib.h>
#include <os.h>

int main(int argc, char **argv) {
    unsigned long seconds = argc > 1 ? (unsigned long)atoi(argv[1]) : 10;
    unsigned long end = uptime_ms() + seconds * 1000;
    unsigned long loops = 0, next_report = uptime_ms() + 1000;
    while (uptime_ms() < end) {
        for (volatile int i = 0; i < 100000; i++) { }
        loops++;
        if (uptime_ms() >= next_report) {
            next_report += 1000;
            printf("[spin %d] %lu million loops so far\n", getpid(), loops / 10);
        }
    }
    printf("[spin %d] finished: %lu million loops in %lu s\n", getpid(), loops / 10, seconds);
    return 0;
}
