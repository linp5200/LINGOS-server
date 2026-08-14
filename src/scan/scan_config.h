#ifndef SCAN_CONFIG_H
#define SCAN_CONFIG_H

#include <stdint.h>

int scan_get_interval(void);
int scan_set_interval(int seconds);
int scan_is_enabled(void);
void scan_set_enabled(int enabled);
uint64_t scan_get_last_completed(void);
void scan_set_last_completed(uint64_t ts);
void scan_config_load(void);
void scan_config_save(void);

#endif