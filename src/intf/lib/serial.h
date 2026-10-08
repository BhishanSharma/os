#ifndef SERIAL_H
#define SERIAL_H

#include <stdint.h>

// COM1 (0x3F8), 115200 8N1, polled. Every character the kernel prints is
// mirrored here, so `qemu -serial stdio` shows the whole boot log and any
// panic report even when the VGA screen is wiped by a crash.
void serial_init(void);
void serial_putc(char c);     // '\n' is sent as "\r\n"
void serial_puts(const char* s);

#endif
