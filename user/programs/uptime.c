// uptime - how long since the computer started
#include <stdio.h>
#include <os.h>

int main(void) {
    unsigned long s = uptime_ms() / 1000;
    printf("Up %lu:%02lu:%02lu (%lu seconds)\n", s / 3600, s / 60 % 60, s % 60, s);
    return 0;
}
