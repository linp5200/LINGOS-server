/**
 * @file    alert_sources.h
 * @brief   默认数据源适配头文件
 * @version LN-B-4.3.0.0
 */

#ifndef ALERT_SOURCES_H
#define ALERT_SOURCES_H

#include "alert_manager.h"

int alert_sources_fetch_all(alert_event_t *events, int max_count);

#endif /* ALERT_SOURCES_H */