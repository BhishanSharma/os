// keyboard.h
#ifndef KEYBOARD_H
#define KEYBOARD_H

#include <stdint.h>
#include <stddef.h>

// Special key codes returned by get_char(). Above 0xFF so they can never be
// mistaken for a typed character (the old values 0x48/0x50 were 'H' and 'P').
#define KEY_UP_ARROW    0x101
#define KEY_DOWN_ARROW  0x102
#define KEY_LEFT_ARROW  0x103
#define KEY_RIGHT_ARROW 0x104
#define KEY_CTRL_Q  17
#define KEY_CTRL_S  19
#define KEY_CTRL_N  14
#define KEY_CTRL_D  4
#define KEY_CTRL_E  5

void keyboard_handler(void);
void init_keyboard(void);
int get_char(void);
void get_line(char* buffer, size_t max_len);

// History functions
void history_add(const char* cmd);
const char* history_prev(void);
const char* history_next(void);

/* Waiting for a key: run the idle hook (e.g. redraw the status bar clock),
 * then halt until the next interrupt. */
void keyboard_set_idle_hook(void (*hook)(void));
void keyboard_idle(void);

#endif
