/**
 * @file    alert_manager.h
 * @brief   预警核心逻辑头文件
 * @version LN-B-4.3.0.0
 */

#ifndef ALERT_MANAGER_H
#define ALERT_MANAGER_H

#include "alert_config.h"
#include <stdint.h>
#include <time.h>

typedef enum {
    ALERT_TYPE_UNKNOWN = 0,
    ALERT_TYPE_TYPHOON,
    ALERT_TYPE_EARTHQUAKE,
    ALERT_TYPE_RAIN,
    ALERT_TYPE_HIGH_TEMP,
    ALERT_TYPE_STORM,
    ALERT_TYPE_FIRE,
    ALERT_TYPE_HEALTH,    /* R7: 系统健康（CPU/内存/磁盘） */
    ALERT_TYPE_SECURITY   /* R7: 安全威胁（异常登录/端口扫描） */
} alert_type_t;

typedef struct {
    alert_type_t type;
    int level;               /* 0-5: 0=info, 1=blue, 2=yellow, 3=orange, 4=red, 5=critical */
    char source[64];         /* 数据源标识 */
    char location[128];      /* 预警地点 */
    char description[256];   /* 描述 */
    double latitude;
    double longitude;
    int distance_km;         /* 距用户距离（km） */

    /* 台风专属 */
    int typhoon_level;       /* 1-5 */
    int wind_speed;          /* km/h */
    int pressure;            /* hPa */

    /* 地震专属 */
    double magnitude;
    int felt;                /* 1=有感 */

    /* 降雨专属 */
    double rainfall_24h;     /* mm/24h */

    time_t timestamp;
    time_t expire_time;
} alert_event_t;

void alert_manager_check_all(const alert_config_t *config);
int alert_manager_query(const char *location, const char *type_str, int time_range_hours,
                        alert_event_t *out, int max_count);
int alert_manager_has_exception(void);
int alert_manager_get_latest(alert_event_t *out, int max_count);

#endif /* ALERT_MANAGER_H */