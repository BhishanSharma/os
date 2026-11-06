// dyncheck: a program linked against the shared C library (ld.so and
// libc.so in /SYS/LIB or /LIB). It starts through the dynamic linker; dlopen
// then loads one more library and dlsym finds a function. System test `dynamic`.
#include <dlfcn.h>
#include <stdio.h>
#include <sys/auxv.h>

int main(void) {
    unsigned long base = getauxval(AT_BASE);              /* where the kernel put ld.so */
    void *m = dlopen("libm.so", RTLD_NOW);                /* a library not loaded yet */
    void *libc = dlopen("libc.so", RTLD_NOW);
    unsigned long (*len)(const char *) = libc ? (unsigned long (*)(const char *))dlsym(libc, "strlen") : 0;
    int ok = base && m && len && len("four") == 4;
    printf("dyncheck: ld.so at %#lx, libm.so %s, strlen %s: %s\n", base, m ? "loaded" : dlerror(),
           len ? "found" : "missing", ok ? "dynamic linking works" : "FAILED");
    return ok ? 0 : 1;
}
