// desktop - start the desktop (the window server), with Terminal open
#include <os.h>

int main(void) {
    char *argv[] = { "windowserver", "terminal", 0 };
    return exec("windowserver", argv);
}
