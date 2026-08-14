/**
 * @file    alert_history.h
 * @brief   预警历史记录存储头文件
 * @version LN-B-4.3.0.0
 */

#ifndef ALERT_HISTORY_H
#define ALERT_HISTORY_H

#include "alert_manager.h"

void alert_history_init(void);
int alert_history_save(const alert_event_t *event);
int alert_history_query(const char *location, const char *type_str, int time_range_hours,
                        alert_event_t *out, int max_count);
void alert_history_cleanup(int keep_days);
void alert_history_cleanup_all(void);
void alert_history_init(void);

#endif /* ALERT_HISTORY_H */