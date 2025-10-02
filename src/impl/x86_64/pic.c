#include <stdint.h>

static inline void outb(uint16_t port, uint8_t val) {
    __asm__ volatile("outb %0, %1" : : "a"(val), "Nd"(port));
}

void pic_remap() {
    uint8_t a1, a2;

    // start init
    outb(0x20, 0x11);
    outb(0xA0, 0x11);

    // set offsets
    outb(0x21, 0x20); // Master PIC vector offset = 0x20
    outb(0xA1, 0x28); // Slave PIC offset = 0x28

    // tell master/slave about each other
    outb(0x21, 0x04);
    outb(0xA1, 0x02);

    // set mode
    outb(0x21, 0x01);
    outb(0xA1, 0x01);

    // unmask keyboard (IRQ1)
    outb(0x21, 0xFD); // 11111101 -> enable only IRQ1
    outb(0xA1, 0xFF);
}
