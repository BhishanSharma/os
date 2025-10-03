#ifndef TIMER_H
#define TIMER_H

#include <stdint.h>
#include "isr.h"

// Frequency of PIT interrupts (100 Hz = 10ms per tick)
#define TIMER_FREQ 100

void timer_init();
uint32_t get_tick();           // Returns total ticks since boot
uint32_t get_seconds();        // Returns uptime in seconds
void sleep(uint32_t ms);       // Sleep for given milliseconds

#endif
