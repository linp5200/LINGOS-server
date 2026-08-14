/**
 * @file    alert_utils.c
 * @brief   预警工具函数（距离计算/时间解析/国家优先级）
 * @version LN-B-4.3.0.0
 * @par     核心协议：防御性编程
 */

#include "alert_utils.h"
#include "../common/safe_string.h"
#include "../lib/log_extra.h"
#include <math.h>
#include <string.h>
#include <stdlib.h>

/* ============================================================
 * 距离计算（Haversine公式）
 * ============================================================ */

double alert_utils_distance(double lat1, double lon1, double lat2, double lon2) {
    const double R = 6371.0; /* 地球半径（km） */
    double dlat = (lat2 - lat1) * M_PI / 180.0;
    double dlon = (lon2 - lon1) * M_PI / 180.0;
    double a = sin(dlat/2) * sin(dlat/2) +
               cos(lat1 * M_PI / 180.0) * cos(lat2 * M_PI / 180.0) *
               sin(dlon/2) * sin(dlon/2);
    double c = 2 * atan2(sqrt(a), sqrt(1-a));
    return R * c;
}

/* ============================================================
 * 国家权重（地理距离权重）
 * ============================================================ */

double alert_utils_get_source_weight(const char *source) {
    if (!source) return 1.0;

    /* 中国数据源权重最高 */
    if (strstr(source, "CN") || strstr(source, "China") || strstr(source, "中国")) {
        return 1.0;
    }
    /* 日本次之 */
    if (strstr(source, "JP") || strstr(source, "Japan") || strstr(source, "日本")) {
        return 0.8;
    }
    /* 美国再次 */
    if (strstr(source, "US") || strstr(source, "USA") || strstr(source, "美国")) {
        return 0.6;
    }
    /* 其他默认较低 */
    return 0.4;
}

/* ============================================================
 * 判断是否为中国数据源
 * ============================================================ */

int alert_utils_is_china_source(const char *source) {
    if (!source) return 0;
    return (strstr(source, "CN") || strstr(source, "China") || strstr(source, "中国")) ? 1 : 0;
}

/* ============================================================
 * 时间解析（相对时间字符串）
 * ============================================================ */

int alert_utils_parse_time_range(const char *str) {
    if (!str) return 24; /* 默认24小时 */

    /* 尝试解析数字 + 单位 */
    int num = 0;
    char unit[8] = {0};
    if (sscanf(str, "%d%s", &num, unit) == 2) {
        if (strcmp(unit, "h") == 0 || strcmp(unit, "hour") == 0 || strcmp(unit, "hours") == 0) {
            return num > 0 ? num : 24;
        }
        if (strcmp(unit, "d") == 0 || strcmp(unit, "day") == 0 || strcmp(unit, "days") == 0) {
            return num * 24;
        }
        if (strcmp(unit, "m") == 0 || strcmp(unit, "min") == 0 || strcmp(unit, "minute") == 0) {
            return num / 60 > 0 ? num / 60 : 1;
        }
    }

    /* 简单关键字 */
    if (strstr(str, "today")) return 24;
    if (strstr(str, "week")) return 168;
    if (strstr(str, "month")) return 720;

    return 24;
}

/* ============================================================
 * 获取用户位置（从配置文件或IP）
 * ============================================================ */

int alert_utils_get_user_location(double *lat, double *lon) {
    /* 简化：从配置文件读取，或使用默认值 */
    /* 实际实现可以调用IP地理位置API */
    *lat = 39.9042; /* 北京 */
    *lon = 116.4074;
    return 0;
}