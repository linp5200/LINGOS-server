/**
 * @file    alert_utils.h
 * @brief   预警工具函数头文件
 * @version LN-B-4.3.0.0
 */

#ifndef ALERT_UTILS_H
#define ALERT_UTILS_H

double alert_utils_distance(double lat1, double lon1, double lat2, double lon2);
double alert_utils_get_source_weight(const char *source);
int alert_utils_is_china_source(const char *source);
int alert_utils_parse_time_range(const char *str);
int alert_utils_get_user_location(double *lat, double *lon);

#endif /* ALERT_UTILS_H */