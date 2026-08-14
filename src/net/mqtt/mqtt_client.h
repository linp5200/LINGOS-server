/**
 * @file    mqtt_client.h
 * @brief   MQTT 客户端封装（基于 libmosquitto）
 * @version LN-B-4.2.0.0
 */

#ifndef NET_MQTT_MQTT_CLIENT_H
#define NET_MQTT_MQTT_CLIENT_H

#include <stdint.h>
#include <stddef.h>
#include <time.h>

/* ============================================================
 * MQTT 连接状态枚举
 * ============================================================ */

typedef enum {
    MQTT_STATE_DISCONNECTED = 0,
    MQTT_STATE_CONNECTING,
    MQTT_STATE_CONNECTED,
    MQTT_STATE_ERROR
} mqtt_state_t;

/* ============================================================
 * MQTT 消息结构
 * ============================================================ */

typedef struct {
    char topic[256];
    char payload[4096];
    int payload_len;
    int qos;
    uint8_t retained;
} mqtt_message_t;

/* ============================================================
 * MQTT 回调函数类型
 * ============================================================ */

typedef void (*mqtt_message_cb)(const mqtt_message_t *msg, void *user_data);
typedef void (*mqtt_connect_cb)(int success, void *user_data);

/* ============================================================
 * MQTT 客户端配置
 * ============================================================ */

typedef struct {
    char broker[128];
    int port;
    char username[64];
    char password[64];
    char client_id[64];
    int keepalive;
    int clean_session;
    mqtt_connect_cb on_connect;
    mqtt_message_cb on_message;
    void *user_data;
} mqtt_config_t;

/* ============================================================
 * 公共 API
 * ============================================================ */

/**
 * @brief 初始化 MQTT 客户端
 * @param config 配置结构
 * @return 0 成功，-1 失败
 */
int mqtt_client_init(const mqtt_config_t *config);

/**
 * @brief 连接 MQTT 服务器
 * @return 0 成功，-1 失败
 */
int mqtt_client_connect(void);

/**
 * @brief 断开 MQTT 连接
 */
void mqtt_client_disconnect(void);

/**
 * @brief 发布消息
 * @param topic 主题
 * @param payload 消息内容
 * @param payload_len 内容长度
 * @param qos QoS 级别 (0/1/2)
 * @param retained 是否保留
 * @return 0 成功，-1 失败
 */
int mqtt_client_publish(const char *topic, const char *payload,
                        int payload_len, int qos, int retained);

/**
 * @brief 订阅主题
 * @param topic 主题（支持通配符）
 * @param qos QoS 级别
 * @return 0 成功，-1 失败
 */
int mqtt_client_subscribe(const char *topic, int qos);

/**
 * @brief 取消订阅
 * @param topic 主题
 * @return 0 成功，-1 失败
 */
int mqtt_client_unsubscribe(const char *topic);

/**
 * @brief 获取当前连接状态
 * @return 状态枚举
 */
mqtt_state_t mqtt_client_get_state(void);

/**
 * @brief 检查是否已连接
 * @return 1 已连接，0 未连接
 */
int mqtt_client_is_connected(void);

/**
 * @brief 设置消息回调
 * @param cb 回调函数
 * @param user_data 用户数据
 */
void mqtt_client_set_callback(mqtt_message_cb cb, void *user_data);

/**
 * @brief 清理 MQTT 客户端
 */
void mqtt_client_cleanup(void);

/**
 * @brief 获取状态名称
 * @param state 状态枚举
 * @return 名称字符串
 */
const char* mqtt_state_name(mqtt_state_t state);

#endif /* NET_MQTT_MQTT_CLIENT_H */