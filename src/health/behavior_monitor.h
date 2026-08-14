/**
 * @file    behavior_monitor.h
 * @brief   行为监控头文件（EWMA + 孤立森林）
 * @version LN-B-5.0.0.0
 */

#ifndef BEHAVIOR_MONITOR_H
#define BEHAVIOR_MONITOR_H

#include <stdint.h>
#include <stddef.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================
 * 行为事件结构
 * ============================================================ */

typedef struct {
    char app_id[64];
    char operation[64];
    char context[256];
    int risk_score;          /* 0-100 */
    uint64_t timestamp;
} behavior_event_t;

/* ============================================================
 * 算法类型
 * ============================================================ */

typedef enum {
    ALGORITHM_EWMA = 0,
    ALGORITHM_ISOLATION_FOREST,
    ALGORITHM_HYBRID
} behavior_algorithm_t;

/* ============================================================
 * API
 * ============================================================ */

int behavior_monitor_init(void);
int behavior_monitor_record_event(const behavior_event_t *event);
int behavior_monitor_get_anomaly_score(const char *app_id, int *score);
int behavior_monitor_check_threshold(const char *app_id, int *triggered);
void behavior_monitor_reset_baseline(const char *app_id);

int behavior_monitor_set_algorithm(behavior_algorithm_t algo);
behavior_algorithm_t behavior_monitor_get_algorithm(void);
int behavior_monitor_get_data_count(const char *app_id);
int behavior_monitor_is_learning(void);

const char* behavior_monitor_algorithm_name(behavior_algorithm_t algo);

#ifdef __cplusplus
}
#endif

#endif /* BEHAVIOR_MONITOR_H */