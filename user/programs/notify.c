// notify TITLE [TEXT...] - a notification on the desktop: a banner at the
// top right for a few seconds, then kept under the bell in the status pill.
//   notify "Backup done" "214 files copied to /USB"
//   make; notify "Build finished"
#include <stdio.h>
#include <string.h>
#include <os.h>
#include <ui.h>

int main(int argc, char **argv) {
    if (argc < 2) {
        printf("usage: notify TITLE [TEXT...]\n");
        return 1;
    }
    char body[120] = "";
    for (int i = 2; i < argc; i++) {
        if (i > 2) strncat(body, " ", sizeof(body) - strlen(body) - 1);
        strncat(body, argv[i], sizeof(body) - strlen(body) - 1);
    }
    if (ui_notify(argv[1], body) != 0) {
        printf("notify: cannot reach the desktop\n");
        return 1;
    }
    if (!ui_desktop()) printf("notify: the desktop is not running (nothing shown)\n");
    return 0;
}
