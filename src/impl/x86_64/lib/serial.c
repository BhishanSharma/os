#include "lib/serial.h"
#include "ports.h"

#define COM1 0x3F8

static int serial_ok = 0;

void serial_init(void) {
    // No UART present (some real machines): reads back 0xFF / ignores writes.
    outb(COM1 + 7, 0xAE);                 // scratch register loopback test
    if (inb(COM1 + 7) != 0xAE) { serial_ok = 0; return; }

    outb(COM1 + 1, 0x00);                 // no UART interrupts, we poll
    outb(COM1 + 3, 0x80);                 // DLAB on
    outb(COM1 + 0, 0x01);                 // divisor 1 -> 115200 baud
    outb(COM1 + 1, 0x00);
    outb(COM1 + 3, 0x03);                 // 8 bits, no parity, 1 stop
    outb(COM1 + 2, 0xC7);                 // enable + clear FIFOs
    outb(COM1 + 4, 0x03);                 // DTR + RTS
    serial_ok = 1;
}

void serial_putc(char c) {
    if (!serial_ok) return;
    if (c == '\n') serial_putc('\r');
    for (int spin = 0; spin < 100000; spin++) {
        if (inb(COM1 + 5) & 0x20) {       // transmit holding register empty
            outb(COM1, (uint8_t)c);
            return;
        }
    }
    // Transmitter stuck: drop the character rather than hang the kernel.
}

void serial_puts(const char* s) {
    while (*s) serial_putc(*s++);
}
