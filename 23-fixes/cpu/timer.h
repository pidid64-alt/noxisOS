#ifndef TIMER_H
#define TIMER_H

#include <stdint.h>

/* Total timer ticks since boot. The timer runs at 50 Hz (see init_timer
 * in irq_install), so uptime in seconds is `tick / 50`. */
extern uint32_t tick;

void init_timer(uint32_t freq);

#endif
