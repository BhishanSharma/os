// keyboard.h
#ifndef KEYBOARD_H
#define KEYBOARD_H

#include <stdint.h>
#include <stddef.h>

// Special key codes
#define KEY_UP_ARROW    0x48
#define KEY_DOWN_ARROW  0x50
#define KEY_LEFT_ARROW  0x4B
#define KEY_RIGHT_ARROW 0x4D

void keyboard_handler(void);
void init_keyboard(void);
char get_char(void);
void get_line(char* buffer, size_t max_len);

// History functions
void history_add(const char* cmd);
const char* history_prev(void);
const char* history_next(void);

#endif
