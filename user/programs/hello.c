// hello - the first user program: arguments, time, memory
#include <stdio.h>
#include <stdlib.h>
#include <os.h>

int main(int argc, char **argv) {
    printf("Hello from user space!\n\n");
    printf("This program runs in ring 3. Its code is at %p and its stack at %p.\n",
           (void *)main, (void *)&argc);

    printf("It was started with %d argument%s:\n", argc, argc == 1 ? "" : "s");
    for (int i = 0; i < argc; i++)
        printf("  argv[%d] = \"%s\"\n", i, argv[i]);

    struct os_time t;
    if (gettime(&t) == 0)
        printf("\nThe time is %02d:%02d:%02d %s on %d-%02d-%02d.\n",
               t.hour, t.minute, t.second, t.zone, t.year, t.month, t.day);

    char *buf = malloc(1000);
    snprintf(buf, 1000, "malloc'd %d bytes at %p", 1000, (void *)buf);
    printf("%s\n", buf);
    free(buf);
    return 0;
}
