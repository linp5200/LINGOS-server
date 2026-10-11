/**
 * @file    src/core/event_bus.h
 * @brief   系统事件总线（先生方案要求：event_publish/subscribe/EventType）
 * @version LN-0.9.0
 * @par     目的：把"状态变化"从"各处自己通知自己"改为"发布—订阅"，
 *          让监控/AI/语音/规则/审计/MQTT 等系统解耦（架构落地第 4 期）。
 * @par     约定：
 *           · payload 为 JSON 字符串（可为 NULL）；回调内**不要**做重活，尽快返回
 *           · 回调里可以订阅/退订/再发布（总线对订阅者快照后再回调，深度受限）
 * @note    与旧代码零冲突：本模块只新增；未订阅的类型发布时零开销（仅计数）。
 */

#ifndef EVENT_BUS_H
#define EVENT_BUS_H

#include "cJSON.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================
 * 事件类型（EventType）
 * ============================================================ */
typedef enum {
    EVT_NONE = 0,
    EVT_CONFIG_CHANGED,          /* 配置保存/重载完成 */
    EVT_LOG_LEVEL_CHANGED,       /* 日志级别变化 */
    EVT_DEFENSE_MODE_CHANGED,    /* 防御/影子模式变化 */
    EVT_PERMISSION_DENIED,       /* 权限拒绝（安全审计用） */
    EVT_PERMISSION_WHITELIST_CHANGED,
    EVT_SKILL_LOADED,            /* 技能加载/热重载 */
    EVT_PLUGIN_LOADED,
    EVT_MONITOR_STATE_CHANGED,   /* 监控采集状态（在线/离线/重连） */
    EVT_MONITOR_FRAME,           /* 帧通道有新帧（轻量信号，不带图像） */
    EVT_VISION_RESULT,           /* AI 识别结果 */
    EVT_ALERT_RAISED,            /* 生命线：预警/警报 */
    EVT_CRISIS_ENTERED,          /* §2B 危机态进入/退出 */
    EVT_AI_SERVER_STATE,         /* AI 服务上下线 */
    EVT_DEVICE_STATE_CHANGED,    /* 设备状态（摄像头/麦克风/存储） */
    EVT_HA_STATE_CHANGED,        /* 家居状态（MQTT/HomeAssistant） */
    EVT_COUNT
} event_type_t;

typedef void (*event_handler_t)(event_type_t type, const char *payload_json, void *userdata);

#define EVENT_MAX_SUBSCRIBERS   8     /* 每种事件类型最大订阅者数 */
#define EVENT_RECENT_MAX        32    /* 最近事件环形缓冲容量 */
#define EVENT_PAYLOAD_MAX       192
#define EVENT_PUBLISH_MAX_DEPTH 4     /* 发布重入深度上限（防递归风暴） */

/* ============================================================
 * 生命周期与订阅
 * ============================================================ */
int event_bus_init(void);
int event_bus_subscribe(event_type_t type, event_handler_t handler, void *userdata);
int event_bus_unsubscribe(event_type_t type, event_handler_t handler);

/* ============================================================
 * 发布
 * ============================================================ */
int event_publish(event_type_t type, const char *payload_json);
int event_publish_fmt(event_type_t type, const char *fmt, ...);

/* ============================================================
 * 观测
 * ============================================================ */
const char* event_type_name(event_type_t type);
int event_type_from_name(const char *name);
int event_bus_subscriber_count(event_type_t type);
unsigned long event_bus_published_count(event_type_t type);
unsigned long event_bus_delivered_count(void);
unsigned long event_bus_dropped_count(void);
cJSON* event_bus_stats(void);      /* 新分配对象，调用者 cJSON_Delete */
cJSON* event_bus_recent(int limit);/* 新分配数组，调用者 cJSON_Delete */

#ifdef __cplusplus
}
#endif

#endif /* EVENT_BUS_H */
