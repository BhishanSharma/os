// echo <text> - print the text (the shell has already put in $USER, $? ...)
#include <stdio.h>
#include <args.h>

int main(int argc, char **argv) {
    char text[512];
    printf("%s\n", args_join(text, sizeof(text), argc, argv, 1));
    return 0;
}
