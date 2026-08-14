#ifndef HEALTH_HEALTH_WATCHDOG_H
#define HEALTH_HEALTH_WATCHDOG_H

#include <stdint.h>

int health_watchdog_start(void);
void health_watchdog_stop(void);
int health_watchdog_is_running(void);
int health_watchdog_set_interval(int seconds);
int health_watchdog_get_interval(void);
int health_watchdog_trigger_now(void);
int health_config_save(int interval);

#endif