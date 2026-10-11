// sleep <seconds> - wait (Ctrl+C stops it)
#include <stdio.h>
#include <stdlib.h>
#include <os.h>

int main(int argc, char **argv) {
    if (argc < 2 || argv[1][0] < '0' || argv[1][0] > '9') {
        printf("usage: sleep <seconds>\n");
        return 2;
    }
    sleep_ms((unsigned long)atol(argv[1]) * 1000);
    return 0;
}
