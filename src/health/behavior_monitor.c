/**
 * @file    behavior_monitor.c
 * @brief   行为监控核心实现（EWMA + 孤立森林）
 * @version LN-B-5.0.0.0
 * @changes 修复 behavior_monitor_get_data_count 函数原型以匹配头文件声明
 */

#include "behavior_monitor.h"
#include "security_config.h"
#include "defense_mode.h"
#include "../common/safe_string.h"
#include "../lib/log_extra.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <pthread.h>
#include <time.h>

#define MAX_EVENTS 10000
#define MAX_APPS 32
#define LEARNING_PERIOD 300  /* 5 秒 */

/* ============================================================
 * 应用统计结构
 * ============================================================ */

typedef struct {
    char app_id[64];
    int event_count;
    double ewma_baseline;
    double ewma_std;
    int is_learning;
    time_t first_event_time;
    int isolation_forest_ready;
    double *recent_scores;
    int recent_count;
    int window_size;
} app_stats_t;

static app_stats_t g_apps[MAX_APPS];
static int g_app_count = 0;
static behavior_algorithm_t g_algorithm = ALGORITHM_EWMA;
static pthread_mutex_t g_monitor_lock = PTHREAD_MUTEX_INITIALIZER;
static int g_initialized = 0;

/* ============================================================
 * 内部辅助
 * ============================================================ */

static app_stats_t* find_or_create_app(const char *app_id) {
    if (!app_id) return NULL;

    for (int i = 0; i < g_app_count; i++) {
        if (strcmp(g_apps[i].app_id, app_id) == 0) {
            return &g_apps[i];
        }
    }

    if (g_app_count >= MAX_APPS) return NULL;

    app_stats_t *app = &g_apps[g_app_count++];
    memset(app, 0, sizeof(app_stats_t));
    safe_strncpy(app->app_id, app_id, sizeof(app->app_id));
    app->window_size = 100;
    app->ewma_baseline = 50.0;
    app->ewma_std = 10.0;
    app->first_event_time = time(NULL);
    app->is_learning = 1;

    app->recent_scores = malloc(sizeof(double) * app->window_size);
    if (app->recent_scores) {
        memset(app->recent_scores, 0, sizeof(double) * app->window_size);
    }

    return app;
}

/* ============================================================
 * EWMA 更新
 * ============================================================ */

static void update_ewma(app_stats_t *app, int score) {
    if (!app) return;

    double alpha = 0.2;  /* 可根据配置调整 */

    if (app->event_count == 0) {
        app->ewma_baseline = score;
        app->ewma_std = 10.0;
    } else {
        app->ewma_baseline = alpha * score + (1 - alpha) * app->ewma_baseline;
    }

    app->event_count++;
}

/* ============================================================
 * 计算异常分数
 * ============================================================ */

static int calculate_anomaly_score(app_stats_t *app, int current_score) {
    if (!app) return 0;

    /* 学习期：不告警 */
    if (app->is_learning) {
        time_t now = time(NULL);
        if (now - app->first_event_time > LEARNING_PERIOD) {
            app->is_learning = 0;
            LOG_DEBUG_T("BehaviorMonitor", "Learning", "Done", "app '%s' learning period ended", app->app_id);
        }
        return 0;
    }

    double deviation = current_score - app->ewma_baseline;
    double threshold = 2.0 * app->ewma_std;

    if (deviation > threshold) {
        int anomaly = (int)((deviation / threshold) * 50 + 50);
        if (anomaly > 100) anomaly = 100;
        return anomaly;
    }

    return 0;
}

/* ============================================================
 * 公共 API
 * ============================================================ */

int behavior_monitor_init(void) {
    LOG_INFO_T("BehaviorMonitor", "Init", "Enter", "initializing behavior monitor");

    pthread_mutex_lock(&g_monitor_lock);

    if (g_initialized) {
        pthread_mutex_unlock(&g_monitor_lock);
        return 0;
    }

    memset(g_apps, 0, sizeof(g_apps));
    g_app_count = 0;
    g_algorithm = ALGORITHM_EWMA;

    g_initialized = 1;
    pthread_mutex_unlock(&g_monitor_lock);

    LOG_INFO_T("BehaviorMonitor", "Init", "OK", "behavior monitor ready");
    return 0;
}

int behavior_monitor_record_event(const behavior_event_t *event) {
    if (!event || !g_initialized) return -1;

    pthread_mutex_lock(&g_monitor_lock);

    app_stats_t *app = find_or_create_app(event->app_id);
    if (!app) {
        pthread_mutex_unlock(&g_monitor_lock);
        return -1;
    }

    update_ewma(app, event->risk_score);

    int anomaly = calculate_anomaly_score(app, event->risk_score);

    /* 处理异常 */
    if (anomaly > 0) {
        const security_config_t *cfg = security_config_get();
        int threshold = cfg ? cfg->behavior_threshold : 70;

        LOG_DEBUG_T("BehaviorMonitor", "Anomaly", "Score", "app='%s', score=%d, threshold=%d",
                    event->app_id, anomaly, threshold);

        if (anomaly >= threshold) {
            /* 极高级：AI检测提醒 */
            LOG_WARN_T("BehaviorMonitor", "Anomaly", "Critical", "app='%s' critical anomaly score=%d",
                       event->app_id, anomaly);
            /* 触发 AI 检测提醒（由外部模块处理） */
            /* 此处作为占位，实际由调用者处理 */
        } else if (anomaly >= 60) {
            /* 高级：暗影模式 */
            if (defense_mode_get() != DEFENSE_MODE_ABSOLUTE) {
                defense_mode_set(DEFENSE_MODE_DARK);
                LOG_WARN_T("BehaviorMonitor", "Anomaly", "Escalate", "app='%s' escalating to dark mode", event->app_id);
            }
        } else if (anomaly >= 30) {
            /* 中级：影子模式 */
            if (defense_mode_get() == DEFENSE_MODE_NONE) {
                defense_mode_set(DEFENSE_MODE_SHADOW);
                LOG_WARN_T("BehaviorMonitor", "Anomaly", "Escalate", "app='%s' escalating to shadow mode", event->app_id);
            }
        }
    }

    pthread_mutex_unlock(&g_monitor_lock);
    return 0;
}

int behavior_monitor_get_anomaly_score(const char *app_id, int *score) {
    if (!app_id || !score || !g_initialized) return -1;

    pthread_mutex_lock(&g_monitor_lock);

    app_stats_t *app = NULL;
    for (int i = 0; i < g_app_count; i++) {
        if (strcmp(g_apps[i].app_id, app_id) == 0) {
            app = &g_apps[i];
            break;
        }
    }

    if (!app) {
        pthread_mutex_unlock(&g_monitor_lock);
        return -1;
    }

    *score = (int)app->ewma_baseline;
    pthread_mutex_unlock(&g_monitor_lock);
    return 0;
}

int behavior_monitor_check_threshold(const char *app_id, int *triggered) {
    if (!app_id || !triggered || !g_initialized) return -1;

    int score;
    if (behavior_monitor_get_anomaly_score(app_id, &score) != 0) {
        return -1;
    }

    const security_config_t *cfg = security_config_get();
    int threshold = cfg ? cfg->behavior_threshold : 70;

    *triggered = (score >= threshold) ? 1 : 0;
    return 0;
}

int behavior_monitor_set_algorithm(behavior_algorithm_t algo) {
    LOG_INFO_T("BehaviorMonitor", "SetAlgorithm", "Enter", "algo=%d", algo);

    pthread_mutex_lock(&g_monitor_lock);

    if (algo == ALGORITHM_ISOLATION_FOREST || algo == ALGORITHM_HYBRID) {
        LOG_WARN_T("BehaviorMonitor", "SetAlgorithm", "NotImplemented",
                   "algorithm '%s' not yet fully implemented, using EWMA as fallback",
                   behavior_monitor_algorithm_name(algo));
        /* 不阻塞，允许切换，但实际仍使用 EWMA */
    }

    g_algorithm = algo;
    pthread_mutex_unlock(&g_monitor_lock);

    LOG_INFO_T("BehaviorMonitor", "SetAlgorithm", "OK", "algorithm set to %s (note: fallback may apply)",
               behavior_monitor_algorithm_name(algo));
    return 0;
}

behavior_algorithm_t behavior_monitor_get_algorithm(void) {
    return g_algorithm;
}

int behavior_monitor_is_learning(void) {
    /* 检查是否所有应用都已结束学习期 */
    pthread_mutex_lock(&g_monitor_lock);

    int learning = 0;
    for (int i = 0; i < g_app_count; i++) {
        if (g_apps[i].is_learning) {
            learning = 1;
            break;
        }
    }

    pthread_mutex_unlock(&g_monitor_lock);
    return learning;
}

const char* behavior_monitor_algorithm_name(behavior_algorithm_t algo) {
    switch (algo) {
        case ALGORITHM_EWMA:             return "ewma";
        case ALGORITHM_ISOLATION_FOREST: return "isolation_forest";
        case ALGORITHM_HYBRID:           return "hybrid";
        default:                         return "unknown";
    }
}

/* ============================================================
 * 【修正】获取指定应用的样本数量
 * ============================================================ */
int behavior_monitor_get_data_count(const char *app_id) {
    if (!app_id) return -1;

    pthread_mutex_lock(&g_monitor_lock);

    for (int i = 0; i < g_app_count; i++) {
        if (strcmp(g_apps[i].app_id, app_id) == 0) {
            int count = g_apps[i].event_count;
            pthread_mutex_unlock(&g_monitor_lock);
            return count;
        }
    }

    pthread_mutex_unlock(&g_monitor_lock);
    return -1;  /* 未找到指定应用 */
}