#include "print.h"

void kernel_main(){
    print_clear();
    print_set_color(PRINT_COLOR_WHITE, PRINT_COLOR_BLACK);
    print_str("welcome to our 64 bit kernel! This is statement was printed by kernel");
}