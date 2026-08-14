#include "../lib/platform.h"
#include <time.h>
#include "linux_timer.h"
#include "log_extra.h"

static uint64_t start_ms = 0;
volatile uint64_t tick_counter = 0;

void linux_timer_init(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    start_ms = (uint64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
    tick_counter = 0;
    LOG_INFO_T("LinuxTimer", "Init", "Start", "Timer ready, start=%llu ms",
               (unsigned long long)start_ms);
}

uint64_t timer_get_ticks(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    uint64_t now = (uint64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
    uint64_t ticks = now - start_ms;
    tick_counter = ticks;
    return ticks;
}

uint64_t timer_get_freq(void) { return 1000; }