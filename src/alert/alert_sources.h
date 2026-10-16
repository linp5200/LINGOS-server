/**
 * @file    alert_sources.h
 * @brief   默认数据源适配头文件
 * @version LN-B-4.3.0.0
 */

#ifndef ALERT_SOURCES_H
#define ALERT_SOURCES_H

#include "alert_manager.h"

int alert_sources_fetch_all(alert_event_t *events, int max_count);

/* 【第9期·生命线守护】数据源健康与"全源不可达"降级通道
 *   alert_sources_health()          —— 只读快照（attempt/fail/last_fail，参数可为 NULL）
 *   alert_sources_degraded_event()  —— 全源不可达且本次需要提醒时填充并返回 1
 *                                      （首次立即 + 每 30 分钟提醒；其余返回 0）
 *   alert_sources_degraded_recover()—— 恢复检测/复位（返回 1 表示本轮由降级恢复）
 *   调用点：src/alert/alert_manager.c:alert_manager_check_all */
void alert_sources_health(int *attempt, int *fail, time_t *last_fail);
int  alert_sources_degraded_event(alert_event_t *out);
int  alert_sources_degraded_recover(void);

#endif /* ALERT_SOURCES_H */