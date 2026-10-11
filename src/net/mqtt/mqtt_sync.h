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

/**
 * @brief 启用/禁用同步模块的自动重连
 */
void mqtt_sync_set_reconnect_enabled(int enabled);

/**
 * @brief 当前设备 ID（mqtt_sync_init 解析后的真实值）
 */
const char *mqtt_sync_get_device_id(void);

/* ============================================================
 * MQTT 服务（唯一对外接线入口）
 *   职责：初始化客户端 → 建立连接 → 订阅 → HA 发现 →
 *         监督线程（重连 + 真实系统指标周期上报 + 可用性）
 *
 * 【集成】守护进程启动时调用一次（lingosd.c / main.c）：
 *     extern int mqtt_service_start(const char *, const char *);
 *     mqtt_service_start(NULL, NULL);     // 设备ID/主题前缀用默认值
 *   退出前调用 mqtt_service_stop()。broker 由环境变量配置，见下。
 * ============================================================ */

/**
 * @brief MQTT 服务运行状态（真实计数，供观测/上报）
 */
typedef struct {
    int running;                  /* 监督线程运行中 */
    int connected;                /* MQTT 已连接 */
    int reconnect_attempts;       /* 连续重连失败次数（成功后清零） */
    int subscriptions;            /* 已登记订阅数（重连后自动恢复） */
    unsigned long publish_ok;     /* 发布成功计数 */
    unsigned long publish_failed; /* 发布失败计数 */
    unsigned long report_cycles;  /* HA 指标上报轮次 */
    long uptime_sec;              /* 服务运行秒数（未运行 = 0） */
} mqtt_service_status_t;

/**
 * @brief 启动 MQTT 服务（幂等）
 * @param device_id 设备 ID，NULL 用 "lingos_device_<pid>"
 * @param sync_topic_prefix 同步主题前缀，NULL 用 "lingos/sync"
 * @return 0 服务已启动；-1 初始化/线程创建失败
 * @note  broker 取环境变量 LINGOS_MQTT_BROKER / LINGOS_MQTT_PORT /
 *        LINGOS_MQTT_USERNAME / LINGOS_MQTT_PASSWORD，默认 127.0.0.1:1883；
 *        broker 暂不可达时服务仍启动，由监督线程按指数退避重连。
 */
int mqtt_service_start(const char *device_id, const char *sync_topic_prefix);

/**
 * @brief 停止 MQTT 服务并回收监督线程（幂等）
 */
void mqtt_service_stop(void);

/**
 * @brief 服务监督线程是否运行中
 * @return 1 运行中，0 未运行
 */
int mqtt_service_is_running(void);

/**
 * @brief 读取服务真实状态
 * @param out 输出结构（可为 NULL）
 */
void mqtt_service_get_status(mqtt_service_status_t *out);

#endif /* NET_MQTT_MQTT_SYNC_H */