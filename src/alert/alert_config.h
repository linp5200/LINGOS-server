/**
 * @file    src/alert/alert_config.h
 * @brief   预警配置文件头文件
 * @version LN-B-4.3.0.0
 * @changes 添加 city、mqtt_enabled、sound_enabled 字段（配置向导集成）
 */

#ifndef ALERT_CONFIG_H
#define ALERT_CONFIG_H

#include <stdint.h>
#include <time.h>

typedef struct alert_config {
    /* 轮询间隔（秒） */
    int base_interval;              /* 默认 3600 */
    int exception_interval;         /* 异常时 600 */
    int realtime_interval;          /* 实时模式 2 */
    int earthquake_realtime_interval; /* 地震实时 1 */

    /* 异常阈值 */
    int typhoon_distance_threshold;   /* km */
    int typhoon_level_threshold;      /* 1-5 */
    double earthquake_magnitude_threshold;
    double rainfall_threshold;        /* mm/24h */

    /* 看门狗 */
    int max_restart_attempts;
    int restart_window_seconds;
    int fallback_to_offline;
    int enable_core_dump;

    /* ====== 新增：配置向导集成 ====== */
    char city[64];                   /* 用户城市 */
    int mqtt_enabled;                /* MQTT 推送开关 */
    int sound_enabled;               /* 声音提示开关 */

    /* R7: 系统健康阈值（%） */
    int cpu_threshold;            /* CPU 使用率阈值，默认 80 */
    int memory_threshold;         /* 内存使用率阈值，默认 85 */
    int disk_threshold;           /* 磁盘使用率阈值，默认 90 */

    /* R7: 安全阈值 */
    int login_fail_threshold;     /* 登录失败次数阈值，默认 5 */

    /* 用户自定义异常检查回调（插件） */
    int (*custom_exception_check)(const void *event);
} alert_config_t;

void alert_config_set_defaults(alert_config_t *cfg);
int alert_config_load(alert_config_t *cfg);
int alert_config_validate(alert_config_t *cfg);
int alert_config_save(const alert_config_t *cfg);

#endif /* ALERT_CONFIG_H */