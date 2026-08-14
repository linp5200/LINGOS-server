/**
 * @file    mqtt_ha.h
 * @brief   Home Assistant 联动接口（Discovery + 命令订阅 + 状态上报）
 * @version LN-B-4.2.0.0
 */

#ifndef NET_MQTT_MQTT_HA_H
#define NET_MQTT_MQTT_HA_H

#include <stdint.h>

/* ============================================================
 * 公共 API
 * ============================================================ */

/**
 * @brief 初始化 HA 联动模块
 * @return 0 成功，-1 失败
 */
int mqtt_ha_init(void);

/**
 * @brief 发送 HA Discovery 消息（自动发现所有传感器）
 * @return 0 成功，-1 失败
 */
int mqtt_ha_send_discovery(void);

/**
 * @brief 上报 CPU 使用率到 HA
 * @param percent CPU 使用率 (0-100)
 * @return 0 成功，-1 失败
 */
int mqtt_ha_report_cpu(int percent);

/**
 * @brief 上报内存使用率到 HA
 * @param percent 内存使用率 (0-100)
 * @return 0 成功，-1 失败
 */
int mqtt_ha_report_memory(int percent);

/**
 * @brief 上报磁盘使用率到 HA
 * @param percent 磁盘使用率 (0-100)
 * @return 0 成功，-1 失败
 */
int mqtt_ha_report_disk(int percent);

/**
 * @brief 上报系统负载到 HA
 * @param load 1分钟平均负载
 * @return 0 成功，-1 失败
 */
int mqtt_ha_report_load(double load);

/**
 * @brief 上报 AI 服务状态到 HA
 * @param online 1 在线，0 离线
 * @return 0 成功，-1 失败
 */
int mqtt_ha_report_ai_status(int online);

/**
 * @brief 检查 Discovery 消息是否已发送
 * @return 1 已发送，0 未发送
 */
int mqtt_ha_is_discovery_sent(void);

/**
 * @brief 清理 HA 联动模块
 */
void mqtt_ha_cleanup(void);

#endif /* NET_MQTT_MQTT_HA_H */