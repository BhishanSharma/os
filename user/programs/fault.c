// fault <kind> - misbehave on purpose to show that the kernel is protected
#include <stdio.h>
#include <string.h>

int main(int argc, char **argv) {
    const char *kind = argc > 1 ? argv[1] : "";

    if (strcmp(kind, "kernel") == 0) {
        printf("Reading kernel memory at 0x100000...\n");
        volatile unsigned char *kernel = (unsigned char *)0x100000;
        printf("Got %d (this line should never print)\n", *kernel);
    } else if (strcmp(kind, "write") == 0) {
        printf("Overwriting the kernel at 0x100000...\n");
        *(volatile unsigned char *)0x100000 = 0;
    } else if (strcmp(kind, "null") == 0) {
        printf("Writing through a NULL pointer...\n");
        *(volatile int *)0 = 42;
    } else if (strcmp(kind, "div") == 0) {
        // Both operands volatile: GCC turns 1 / x into a comparison, with no division.
        volatile int dividend = 100, zero = 0;
        printf("Dividing by zero...\n");
        printf("%d\n", dividend / zero);
    } else if (strcmp(kind, "cli") == 0) {
        printf("Trying to disable interrupts (a privileged instruction)...\n");
        __asm__ volatile("cli");
    } else if (strcmp(kind, "loop") == 0) {
        printf("Spinning forever. Press Ctrl+C to stop me.\n");
        for (;;) { }
    } else {
        printf("usage: fault <kernel|write|null|div|cli|loop>\n");
        printf("Each one breaks a rule; the kernel stops this program and keeps running.\n");
        return 1;
    }
    return 0;
}
