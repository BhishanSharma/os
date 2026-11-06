// uname [-a] [-s] [-n] [-r] [-m] - the system's name, the computer's name, the
// version, the processor's architecture (no option: the name; -a: all)
#include <stdio.h>
#include <os.h>

int main(int argc, char **argv) {
    struct os_uname u;
    if (uname(&u) != 0) return 1;
    int s = 0, n = 0, r = 0, m = 0;
    for (int i = 1; i < argc; i++)
        for (const char *f = argv[i][0] == '-' ? argv[i] + 1 : ""; *f; f++) {
            if (*f == 'a') s = n = r = m = 1;
            else if (*f == 's') s = 1;
            else if (*f == 'n') n = 1;
            else if (*f == 'r') r = 1;
            else if (*f == 'm' || *f == 'p') m = 1;
        }
    if (!s && !n && !r && !m) s = 1;
    const char *sep = "";
    if (s) { printf("%s%s", sep, u.sysname); sep = " "; }
    if (n) { printf("%s%s", sep, u.hostname); sep = " "; }
    if (r) { printf("%s%s", sep, u.release); sep = " "; }
    if (m) printf("%sx86_64", sep);
    printf("\n");
    return 0;
}
