// ioreg - the device tree: the machine, its buses and devices, what drives them
#include <stdio.h>
#include <os.h>

#define MAX 128

static struct os_regnode nodes[MAX];
static int count;

static void show(int id, int depth) {
    for (int i = 0; i < depth; i++) printf("  ");
    set_theme_color(THEME_ACCENT);
    printf("%s", nodes[id].name);
    set_theme_color(THEME_TEXT);
    printf("  <%s>%s%s\n", nodes[id].cls, nodes[id].info[0] ? "  " : "", nodes[id].info);
    for (int c = 0; c < count; c++)
        if (nodes[c].parent == id && c != id) show(c, depth + 1);
}

int main(void) {
    while (count < MAX && registry(count, &nodes[count]) == 1) count++;
    if (count) show(0, 0);
    return 0;
}
