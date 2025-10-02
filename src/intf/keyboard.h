#ifndef KEYBOARD_H
#define KEYBOARD_H

#include <stdint.h>
#include <stddef.h>

void init_keyboard();
char get_char();
void get_line(char* buffer, size_t max_len);

#endif
