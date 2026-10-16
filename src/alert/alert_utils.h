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

/**
 * 【第9期·生命线可读性】预警类型 → 用户可读名称（中英成对，与 notify_shell 同源）
 *   @param type alert_type_t（0..8，越界返回"未知/Unknown"）
 *   @return 静态字符串（tr() 结果，勿 free）
 */
const char *alert_type_label(int type);

/**
 * 【第9期】预警级别 → 面向 App/Web 的 L 级标签（"info"/"L0".."L5"）
 *   背景：历史 JSON 原先写整数 level，Python alert_summary 用 {"L0".."L3","info"} 统计
 *   → str(2) 不匹配任何键 → 所有预警都被算成 info（级别统计失效）。现输出 L 级字符串。
 */
const char *alert_level_label(int level);

#endif /* ALERT_UTILS_H */