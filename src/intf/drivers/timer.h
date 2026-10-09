#ifndef TIMER_H
#define TIMER_H

#include <stdint.h>
#include "core/isr.h"

// Frequency of PIT interrupts (100 Hz = 10ms per tick)
#define TIMER_FREQ 100

void timer_init();
uint32_t get_tick();           // Returns total ticks since boot
uint32_t get_seconds();        // Returns uptime in seconds
void sleep(uint32_t ms);       // Sleep for given milliseconds

/* Called from the timer interrupt on every tick (IRQ context), e.g. to poll a
 * device whose interrupt is not routed. Pass 0 to remove. */
void timer_set_poll_hook(void (*hook)(void));

#endif
