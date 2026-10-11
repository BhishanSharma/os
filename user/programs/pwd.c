// pwd - the current folder
#include <stdio.h>
#include <os.h>

int main(void) {
    char cwd[256];
    getcwd(cwd, sizeof(cwd));
    printf("%s\n", cwd);
    return 0;
}
