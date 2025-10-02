#include "print.h"

void kernel_main(){
    print_clear();

    print_set_color(PRINT_COLOR_WHITE, PRINT_COLOR_BLACK);
    print_str("welcome to our 64 bit kernel! This is statement was printed by kernel\n");
    
    print_set_color(PRINT_COLOR_LIGHT_GREEN, PRINT_COLOR_BLACK);
    print_str("SUCCESS\n");

    // print_set_color(PRINT_COLOR_LIGHT_RED, PRINT_COLOR_BLACK);
    // print_str("ERROR: Something failed!\n");

    print_str("Number: ");
    print_int(12345);

    print_str("\nAddress: ");
    print_hex(0xB8000);
    print_str("\n");

    kprintf("String: %s\n", "Hello Kernel!");
}