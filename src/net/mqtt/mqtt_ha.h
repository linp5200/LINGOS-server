/**
 * @file    mqtt_ha.h
 * @brief   Home Assistant 联动接口（Discovery + 命令订阅 + 状态上报）
 * @version LN-B-4.2.0.0
 */

#ifndef NET_MQTT_MQTT_HA_H
#define NET_MQTT_MQTT_HA_H

#include <stdint.h>
#include <stddef.h>

/* ============================================================
 * 命令审查钩子（由审查方注册：ai_master Guard 审查）
 *   返回值 0 = 放行，非 0 = 拒绝（reason 回填拒绝原因）
 *   未注册钩子时 on_ha_command 采取 fail-closed（拒绝执行）
 * ============================================================ */

typedef int (*mqtt_ha_review_fn)(const char *command, const char *args_json,
                                 char *reason, size_t reason_len);

/* ============================================================
 * 公共 API
 * ============================================================ */

/**
 * @brief 注册命令审查钩子
 * @param fn 审查函数，NULL 表示注销（注销后命令一律 fail-closed）
 * @return 0 成功，-1 失败（fn 为 NULL 时仍返回 0，表示已注销）
 */
int mqtt_ha_set_review_hook(mqtt_ha_review_fn fn);

/**
 * @brief 是否已注册审查钩子
 * @return 1 已注册，0 未注册
 */
int mqtt_ha_has_review_hook(void);

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
 * @brief 采集真实系统指标（CPU/内存/磁盘/负载/AI 探活）并逐项上报
 * @return 成功上报的指标数（>=0），-1 表示未初始化或未连接（跳过上报，不伪造数据）
 * @note  CPU 来自 /proc/stat 差分、内存来自 sysinfo、磁盘来自 statvfs、
 *        负载来自 /proc/loadavg、AI 状态来自 AF_UNIX 连接探活
 */
int mqtt_ha_report_system(void);

/**
 * @brief 发布可用性（HA avty_t 主题，retained）——离线时由 HA 侧自动置为不可用
 * @param online 1 在线，0 离线
 * @return 0 成功，-1 失败
 */
int mqtt_ha_publish_availability(int online);

/**
 * @brief 可用性主题名（Discovery 与发布共用同一常量）
 */
const char *mqtt_ha_availability_topic(void);

/**
 * @brief 读取最近一次真实采样快照
 * @param cpu 输出 CPU 使用率(%)，-1 表示本次不可用（可为 NULL）
 * @param mem 输出内存使用率(%)，-1 表示不可用（可为 NULL）
 * @param disk 输出磁盘使用率(%)，-1 表示不可用（可为 NULL）
 * @param load 输出 1 分钟负载，<0 表示不可用（可为 NULL）
 * @param ai_online 输出 AI 服务探活结果（0/1，可为 NULL）
 * @return 1 至少有一项真实采样，0 尚无采样
 */
int mqtt_ha_get_last_metrics(int *cpu, int *mem, int *disk, double *load, int *ai_online);

/**
 * @brief 清理 HA 联动模块
 */
void mqtt_ha_cleanup(void);

#endif /* NET_MQTT_MQTT_HA_H */