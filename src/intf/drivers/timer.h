#ifndef TIMER_H
#define TIMER_H

#include <stdint.h>
#include "core/isr.h"

// Kernel ticks: 100 Hz (10 ms per tick)
#define TIMER_FREQ 100
// The boot core's timer interrupts 10 times as often: the extra interrupts
// only move the pointer (smoothly); every 10th is a tick.
#define TIMER_HW_FREQ 1000

void timer_init();
uint32_t get_tick();           // Returns total ticks since boot
uint32_t get_seconds();        // Returns uptime in seconds
void sleep(uint32_t ms);       // Sleep for given milliseconds

/* Called from the timer interrupt on every tick (IRQ context), e.g. to poll a
 * device whose interrupt is not routed. Pass 0 to remove. */
void timer_set_poll_hook(void (*hook)(void));
/* Another hook alongside it (up to four). */
void timer_add_poll_hook(void (*hook)(void));
/* Called on every timer interrupt (1 kHz) with the kernel lock free, so it
 * may skip a round: for quick work only (the pointer). */
void timer_set_fast_hook(void (*hook)(void));
uint32_t timer_ms(void);       // milliseconds since the timer started (1 kHz)

#endif
