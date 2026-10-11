// threads [n] - n threads (default 4) add to one counter under a mutex
#include <stdio.h>
#include <stdlib.h>
#include <os.h>

#define ROUNDS 100000

static mutex_t lock;
static volatile long counter;

static void work(void *arg) {
    (void)arg;
    for (int i = 0; i < ROUNDS; i++) {
        mutex_lock(&lock);
        counter++;
        mutex_unlock(&lock);
    }
}

int main(int argc, char **argv) {
    int n = argc > 1 ? atoi(argv[1]) : 4;
    if (n < 1 || n > 16) n = 4;
    int tid[16];
    unsigned long start = uptime_ms();
    for (int i = 0; i < n; i++) tid[i] = thread_create(work, 0);
    for (int i = 0; i < n; i++) {
        int status;
        if (tid[i] > 0) wait(tid[i], &status, 0);
    }
    long want = (long)n * ROUNDS;
    printf("threads: %d threads, counter %ld (expected %ld) in %lu ms: %s\n", n, counter, want,
           uptime_ms() - start, counter == want ? "ok" : "WRONG");
    return counter == want ? 0 : 1;
}
