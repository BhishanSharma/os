// mouse.h - PS/2 mouse (and VirtualBox mouse integration)
//
// The mouse moves a pointer over the text console. Without a program that
// asks for the mouse: the wheel scrolls through earlier output, dragging with
// the left button selects text (and copies it), and the right or middle
// button pastes it as if it were typed. A program that calls getmouse() gets
// the pointer and buttons instead while it is in the foreground.
#ifndef MOUSE_H
#define MOUSE_H

#include <stdint.h>

#define MOUSE_LEFT    1
#define MOUSE_RIGHT   2
#define MOUSE_MIDDLE  4

/* Set up the PS/2 mouse. Call with interrupts off (before `sti`), after the
 * PIC is set up. Returns 0 if a mouse answered. */
int mouse_init(void);

/* After the VirtualBox guest device is set up: use the host's pointer
 * position (no mouse capture needed in the VM window). */
void mouse_init_vbox(void);

/* A report from a USB mouse or I2C touchpad (`source` names it): buttons
 * (MOUSE_*), movement (positive dy = down) and wheel steps (positive = away
 * from the user). The first one turns the pointer on even without a PS/2
 * mouse. */
void mouse_report(const char *source, int buttons, int dx, int dy, int wheel);

int mouse_present(void);
const char *mouse_description(void);   /* "PS/2, wheel" */

/* Interrupt side: IRQ 12, and stray mouse bytes the keyboard IRQ finds. */
void mouse_irq(void);
void mouse_handle_byte(uint8_t byte);

/* Called often while idle: move the pointer, scroll, select, paste. */
void mouse_poll(void);

/* For programs (getmouse): the pointer cell in text-area coordinates (row -1
 * is the status bar), buttons held, wheel steps since the last call
 * (positive = towards the user). `owner` takes the mouse away from the
 * console while that pid is in the foreground; 0 gives it back. */
void mouse_read(int *col, int *row, int *buttons, int *wheel);
void mouse_set_owner(int pid);
int mouse_owner(void);

#endif
