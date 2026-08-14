/**
 * @file    alert_notify.h
 * @brief   预警通知分发头文件
 * @version LN-B-4.3.0.0
 */

#ifndef ALERT_NOTIFY_H
#define ALERT_NOTIFY_H

#include "alert_manager.h"
#include "alert_config.h"

void alert_notify_init(const alert_config_t *config);
void alert_notify_send(const alert_event_t *event, const alert_config_t *config);
void alert_notify_force_send(const alert_event_t *event);
void alert_notify_cleanup(void);

#endif /* ALERT_NOTIFY_H */