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
 * @return 0 成功（含"未连接时已登记，连接后自动订阅"），-1 失败
 * @note  订阅会被登记；连接建立/重连成功后由 mqtt_client 自动重订阅
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
 * @brief 设置消息回调（兼容接口：全局唯一，后设置者覆盖前者）
 * @param cb 回调函数
 * @param user_data 用户数据
 */
void mqtt_client_set_callback(mqtt_message_cb cb, void *user_data);

/**
 * @brief 注册消息处理器（多订阅者，按主题过滤器分发）
 * @param topic_filter MQTT 主题过滤器（支持 + 和 # 通配），NULL/空视为 "#"
 * @param cb 回调函数
 * @param user_data 用户数据
 * @return 句柄 >= 0 成功，-1 失败
 * @note  与 mqtt_client_set_callback 并存：兼容回调 + 全部处理器都会被调用
 */
int mqtt_client_add_message_handler(const char *topic_filter,
                                    mqtt_message_cb cb, void *user_data);

/**
 * @brief 注销消息处理器
 * @param handle mqtt_client_add_message_handler 返回的句柄
 * @return 0 成功，-1 失败
 */
int mqtt_client_remove_message_handler(int handle);

/**
 * @brief 主题过滤器匹配（真实 MQTT 通配语义：+ 单层、# 多层含父层）
 * @return 1 匹配，0 不匹配
 */
int mqtt_client_topic_matches(const char *filter, const char *topic);

/**
 * @brief 确保已连接；未连接时按指数退避触发重连（非阻塞，供监督线程调用）
 * @return 1 已连接，0 未连接（退避期内不重复尝试）
 */
int mqtt_client_ensure_connected(void);

/**
 * @brief 启用/禁用自动重连
 */
void mqtt_client_set_reconnect_enabled(int enabled);

/**
 * @brief 查询自动重连开关
 * @return 1 启用，0 禁用
 */
int mqtt_client_get_reconnect_enabled(void);

/**
 * @brief 查询连续重连失败次数（成功连接后清零）
 */
int mqtt_client_get_reconnect_attempts(void);

/**
 * @brief 获取发布统计（真实计数，失败含未连接/未初始化/参数错误）
 * @param ok 输出成功次数（可为 NULL）
 * @param failed 输出失败次数（可为 NULL）
 */
void mqtt_client_get_publish_stats(unsigned long *ok, unsigned long *failed);

/**
 * @brief 当前已登记的订阅数（连接建立后会自动重订阅）
 */
int mqtt_client_get_subscription_count(void);

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