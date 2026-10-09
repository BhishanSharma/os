// primes [limit] - sieve of Eratosthenes with a malloc'd table
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <os.h>

int main(int argc, char **argv) {
    long limit = argc > 1 ? atol(argv[1]) : 1000000;
    if (limit < 2 || limit > 100000000) {
        printf("usage: primes [limit 2..100000000]\n");
        return 1;
    }

    unsigned long start = uptime_ms();
    char *composite = malloc((size_t)limit + 1);
    if (!composite) {
        printf("primes: not enough memory for %ld bytes\n", limit + 1);
        return 1;
    }
    memset(composite, 0, (size_t)limit + 1);

    long count = 0, last = 0;
    for (long i = 2; i <= limit; i++) {
        if (composite[i]) continue;
        count++;
        last = i;
        for (long j = i * i; j <= limit; j += i) composite[j] = 1;
    }
    unsigned long ms = uptime_ms() - start;

    printf("%ld primes up to %ld; the largest is %ld.\n", count, limit, last);
    printf("Took %lu ms, using %ld KiB of heap.\n", ms, (limit + 1) / 1024);
    free(composite);
    return 0;
}
