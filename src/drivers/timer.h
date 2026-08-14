#ifndef DRIVERS_TIMER_H
#define DRIVERS_TIMER_H
#include <stdint.h>

void timer_init(void);
uint64_t timer_get_ticks(void);
uint64_t timer_get_freq(void);
extern volatile uint64_t tick_counter;

#endif