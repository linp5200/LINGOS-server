/**
 * @file    mqtt_sync.h
 * @brief   MQTT 多设备同步接口
 * @version LN-B-4.2.0.0
 */

#ifndef NET_MQTT_MQTT_SYNC_H
#define NET_MQTT_MQTT_SYNC_H

#include <stdint.h>
#include <stddef.h>
#include <time.h>

/* ============================================================
 * 同步数据类型枚举
 * ============================================================ */

typedef enum {
    SYNC_TYPE_MEMORY = 0,        /* 记忆 */
    SYNC_TYPE_CONFIG,            /* 系统配置 */
    SYNC_TYPE_HISTORY,           /* 命令历史 */
    SYNC_TYPE_ALIAS,             /* 别名 */
    SYNC_TYPE_REMINDER,          /* 任务提醒 */
    SYNC_TYPE_HA_STATE           /* HA 设备状态 */
} sync_data_type_t;

/* ============================================================
 * 同步消息结构
 * ============================================================ */

typedef struct {
    sync_data_type_t type;       /* 数据类型 */
    char device_id[64];          /* 设备 ID */
    char data[4096];             /* JSON 数据 */
    int64_t timestamp;           /* 时间戳（微秒） */
    int64_t version;             /* 版本号（冲突解决） */
} sync_message_t;

/* ============================================================
 * 同步回调函数类型
 * ============================================================ */

typedef int (*sync_receive_cb)(const sync_message_t *msg, void *user_data);

/* ============================================================
 * 公共 API
 * ============================================================ */

/**
 * @brief 初始化 MQTT 同步系统
 * @param device_id 设备唯一标识
 * @param sync_topic_prefix 同步主题前缀（如 "lingos/sync"）
 * @return 0 成功，-1 失败
 */
int mqtt_sync_init(const char *device_id, const char *sync_topic_prefix);

/**
 * @brief 启动同步（连接 MQTT 并订阅）
 * @return 0 成功，-1 失败
 */
int mqtt_sync_start(void);

/**
 * @brief 停止同步
 */
void mqtt_sync_stop(void);

/**
 * @brief 发送同步数据
 * @param type 数据类型
 * @param data JSON 数据
 * @param data_len 数据长度
 * @return 0 成功，-1 失败
 */
int mqtt_sync_send(sync_data_type_t type, const char *data, int data_len);

/**
 * @brief 注册数据接收回调
 * @param type 数据类型
 * @param cb 回调函数
 * @param user_data 用户数据
 * @return 0 成功，-1 失败
 */
int mqtt_sync_register_callback(sync_data_type_t type, sync_receive_cb cb, void *user_data);

/**
 * @brief 检查同步是否运行中
 * @return 1 运行中，0 未运行
 */
int mqtt_sync_is_running(void);

/**
 * @brief 清理同步系统
 */
void mqtt_sync_cleanup(void);

#endif /* NET_MQTT_MQTT_SYNC_H */