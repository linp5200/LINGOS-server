/**
 * @file    mqtt_sync.c
 * @brief   MQTT 多设备同步实现
 * @version LN-B-5.0.0.0
 * @changes 自动重连增强；安全字符串替换；双文支持
 */

#include "mqtt_sync.h"
#include "mqtt_client.h"
#include "mqtt_ha.h"      /* 服务: HA 初始化/发现/真实指标上报/可用性 */
#include "mqtt_sync_enhanced.h" /* 服务: 多设备同步（服务端权威）初始化 */
#include "../common/safe_string.h"
#include "../common/lang.h"
#include "../lib/log_extra.h"
#include "../lib/cJSON/cJSON.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <pthread.h>

#define MAX_CALLBACKS 16
#define DEFAULT_TOPIC_PREFIX "lingos/sync"
#define DEFAULT_DEVICE_ID "lingos_device"
#define MAX_RECONNECT_ATTEMPTS 5
#define RECONNECT_BACKOFF_BASE 2

/* ============================================================
 * 回调注册结构
 * ============================================================ */

typedef struct {
    sync_data_type_t type;
    sync_receive_cb cb;
    void *user_data;
    int active;
} sync_callback_t;

/* ============================================================
 * 全局状态
 * ============================================================ */

static char g_device_id[64] = {0};
static char g_topic_prefix[128] = {0};
static int g_sync_running = 0;
static sync_callback_t g_callbacks[MAX_CALLBACKS];
static int g_callback_count = 0;
static pthread_mutex_t g_sync_lock = PTHREAD_MUTEX_INITIALIZER;
static int g_reconnect_attempts = 0;
static int g_reconnect_enabled = 1;
static int g_sync_handler = -1;      /* mqtt_client 多处理器句柄（替代被覆盖的全局回调） */

/* ============================================================
 * 内部辅助：构建主题
 * ============================================================ */

static void build_topic(char *out, size_t out_len, const char *suffix) {
    safe_snprintf(out, out_len, "%s/%s/%s", g_topic_prefix, g_device_id, suffix);
}

static void build_broadcast_topic(char *out, size_t out_len, const char *suffix) {
    safe_snprintf(out, out_len, "%s/+/%s", g_topic_prefix, suffix);
}

/* ============================================================
 * 内部辅助：处理接收到的消息
 * ============================================================ */

static void on_sync_message(const mqtt_message_t *msg, void *user_data) {
    (void)user_data;

    LOG_DEBUG_T("MQTTSync", "OnMessage", "Enter", "topic='%s'", msg->topic);

    sync_message_t sync_msg;
    memset(&sync_msg, 0, sizeof(sync_msg));

    cJSON *root = cJSON_Parse(msg->payload);
    if (!root) {
        LOG_WARN_T("MQTTSync", "OnMessage", "ParseFail", "invalid JSON");
        return;
    }

    cJSON *type = cJSON_GetObjectItem(root, "type");
    cJSON *device = cJSON_GetObjectItem(root, "device_id");
    cJSON *data = cJSON_GetObjectItem(root, "data");
    cJSON *timestamp = cJSON_GetObjectItem(root, "timestamp");
    cJSON *version = cJSON_GetObjectItem(root, "version");

    if (!type || !cJSON_IsNumber(type)) {
        cJSON_Delete(root);
        LOG_WARN_T("MQTTSync", "OnMessage", "NoType", "missing type");
        return;
    }

    sync_msg.type = (sync_data_type_t)type->valueint;
    if (device && cJSON_IsString(device)) {
        safe_strncpy(sync_msg.device_id, device->valuestring, sizeof(sync_msg.device_id));
    }
    if (data && cJSON_IsString(data)) {
        safe_strncpy(sync_msg.data, data->valuestring, sizeof(sync_msg.data));
    }
    if (timestamp && cJSON_IsNumber(timestamp)) {
        sync_msg.timestamp = (int64_t)timestamp->valuedouble;
    }
    if (version && cJSON_IsNumber(version)) {
        sync_msg.version = (int64_t)version->valuedouble;
    }

    cJSON_Delete(root);

    if (strcmp(sync_msg.device_id, g_device_id) == 0) {
        LOG_DEBUG_T("MQTTSync", "OnMessage", "Self", "ignoring self message");
        return;
    }

    LOG_INFO_T("MQTTSync", "OnMessage", "Received", "type=%d, device='%s'",
               sync_msg.type, sync_msg.device_id);

    pthread_mutex_lock(&g_sync_lock);
    for (int i = 0; i < g_callback_count; i++) {
        if (g_callbacks[i].active && g_callbacks[i].type == sync_msg.type) {
            if (g_callbacks[i].cb) {
                g_callbacks[i].cb(&sync_msg, g_callbacks[i].user_data);
            }
        }
    }
    pthread_mutex_unlock(&g_sync_lock);
}

/* ============================================================
 * 内部辅助：获取数据类型的主题后缀
 * ============================================================ */

static const char* get_topic_suffix(sync_data_type_t type) {
    switch (type) {
        case SYNC_TYPE_MEMORY:    return "memory";
        case SYNC_TYPE_CONFIG:    return "config";
        case SYNC_TYPE_HISTORY:   return "history";
        case SYNC_TYPE_ALIAS:     return "alias";
        case SYNC_TYPE_REMINDER:  return "reminder";
        case SYNC_TYPE_HA_STATE:  return "ha_state";
        default:                  return "unknown";
    }
}

/* ============================================================
 * 【修改】自动重连增强
 * ============================================================ */

static int ensure_mqtt_connected(void) {
    if (mqtt_client_is_connected()) {
        g_reconnect_attempts = 0;
        return 1;
    }

    if (!g_reconnect_enabled) {
        LOG_DEBUG_T("MQTTSync", "Reconnect", "Disabled", "reconnect disabled");
        return 0;
    }

    if (g_reconnect_attempts >= MAX_RECONNECT_ATTEMPTS) {
        LOG_WARN_T("MQTTSync", "Reconnect", "MaxAttempts", "max reconnect attempts reached");
        return 0;
    }

    int backoff = RECONNECT_BACKOFF_BASE * (1 << g_reconnect_attempts);
    if (backoff > 60) backoff = 60;

    LOG_INFO_T("MQTTSync", "Reconnect", "Attempt", "attempt %d/%d, backoff %ds",
               g_reconnect_attempts + 1, MAX_RECONNECT_ATTEMPTS, backoff);

    if (mqtt_client_connect() == 0) {
        /* 等待连接建立 */
        for (int i = 0; i < backoff * 2; i++) {
            if (mqtt_client_is_connected()) {
                g_reconnect_attempts = 0;
                LOG_INFO_T("MQTTSync", "Reconnect", "Success", "reconnected successfully");
                /* 重新订阅 */
                char topic[256];
                build_broadcast_topic(topic, sizeof(topic), "memory");
                mqtt_client_subscribe(topic, 1);
                build_broadcast_topic(topic, sizeof(topic), "config");
                mqtt_client_subscribe(topic, 1);
                build_broadcast_topic(topic, sizeof(topic), "history");
                mqtt_client_subscribe(topic, 1);
                return 1;
            }
            usleep(500000);
        }
    }

    g_reconnect_attempts++;
    LOG_WARN_T("MQTTSync", "Reconnect", "Fail", "reconnect attempt %d failed", g_reconnect_attempts);
    return 0;
}

/* ============================================================
 * 内部辅助：解析 broker 配置（环境变量优先，默认 127.0.0.1:1883）
 * ============================================================ */

static void resolve_broker_config(mqtt_config_t *config) {
    const char *broker = getenv("LINGOS_MQTT_BROKER");
    const char *port_s = getenv("LINGOS_MQTT_PORT");
    const char *user   = getenv("LINGOS_MQTT_USERNAME");
    const char *pass   = getenv("LINGOS_MQTT_PASSWORD");
    const char *source = "default 127.0.0.1:1883";

    safe_strncpy(config->broker, (broker && *broker) ? broker : "127.0.0.1",
                 sizeof(config->broker));
    if (broker && *broker) source = "env LINGOS_MQTT_BROKER";

    int port = (port_s && *port_s) ? atoi(port_s) : 1883;
    if (port <= 0 || port > 65535) {
        LOG_WARN_T("MQTTSync", "Config", "BadPort", "invalid LINGOS_MQTT_PORT='%s', using 1883",
                   port_s ? port_s : "");
        port = 1883;
    }
    config->port = port;

    if (user && *user) {
        safe_strncpy(config->username, user, sizeof(config->username));
        safe_strncpy(config->password, pass ? pass : "", sizeof(config->password));
    }

    LOG_INFO_T("MQTTSync", "Config", "Broker", "broker='%s', port=%d (%s)",
               config->broker, config->port, source);
}

/* ============================================================
 * 公共 API 实现
 * ============================================================ */

int mqtt_sync_init(const char *device_id, const char *sync_topic_prefix) {
    LOG_INFO_T("MQTTSync", "Init", "Enter", "device='%s', prefix='%s'",
               device_id ? device_id : "(null)", sync_topic_prefix ? sync_topic_prefix : "(null)");

    if (device_id && *device_id) {
        safe_strncpy(g_device_id, device_id, sizeof(g_device_id));
    } else {
        safe_snprintf(g_device_id, sizeof(g_device_id), "%s_%d", DEFAULT_DEVICE_ID, getpid());
    }

    if (sync_topic_prefix && *sync_topic_prefix) {
        safe_strncpy(g_topic_prefix, sync_topic_prefix, sizeof(g_topic_prefix));
    } else {
        safe_strncpy(g_topic_prefix, DEFAULT_TOPIC_PREFIX, sizeof(g_topic_prefix));
    }

    mqtt_config_t config;
    memset(&config, 0, sizeof(config));
    resolve_broker_config(&config);
    config.keepalive = 60;
    config.clean_session = 1;
    safe_strncpy(config.client_id, g_device_id, sizeof(config.client_id));
    /* 不再使用全局 on_message（会被 HA 命令处理覆盖）→ 用带主题过滤的多处理器 */
    config.on_message = NULL;

    int ret = mqtt_client_init(&config);
    if (ret != 0) {
        LOG_ERROR_T("MQTTSync", "Init", "MQTTFail", "mqtt_client_init failed");
        return -1;
    }

    if (g_sync_handler < 0) {
        char filter[256];
        safe_snprintf(filter, sizeof(filter), "%s/+/#", g_topic_prefix);
        g_sync_handler = mqtt_client_add_message_handler(filter, on_sync_message, NULL);
        if (g_sync_handler < 0) {
            LOG_ERROR_T("MQTTSync", "Init", "HandlerFail", "could not register sync message handler");
            return -1;
        }
    }

    LOG_INFO_T("MQTTSync", "Init", "OK", "sync system initialized, device='%s', broker='%s:%d'",
               g_device_id, config.broker, config.port);
    return 0;
}

int mqtt_sync_start(void) {
    LOG_INFO_T("MQTTSync", "Start", "Enter", "starting MQTT sync");

    if (g_sync_running) {
        LOG_WARN_T("MQTTSync", "Start", "Already", "sync already running");
        return 0;
    }

    /* 先登记全部同步主题订阅：未连接时登记，连接/重连建立后自动生效 */
    static const char *suffixes[] = {"memory", "config", "history",
                                     "alias", "reminder", "ha_state"};
    char topic[256];
    int registered = 0;

    for (size_t i = 0; i < sizeof(suffixes) / sizeof(suffixes[0]); i++) {
        build_broadcast_topic(topic, sizeof(topic), suffixes[i]);
        if (mqtt_client_subscribe(topic, 1) == 0) {
            registered++;
        } else {
            LOG_WARN_T("MQTTSync", "Start", "SubscribeFail", "could not register '%s'", topic);
        }
    }

    g_sync_running = 1;
    g_reconnect_attempts = 0;

    if (!ensure_mqtt_connected()) {
        /* 诚实降级：订阅与运行态已登记，连接由服务监督线程按退避重试 */
        LOG_WARN_T("MQTTSync", "Start", "Deferred",
                   "broker not reachable yet, %d subscription(s) queued, supervisor will retry",
                   registered);
        return -1;
    }

    LOG_INFO_T("MQTTSync", "Start", "OK", "sync started with %d subscription(s)", registered);
    return 0;
}

void mqtt_sync_stop(void) {
    LOG_INFO_T("MQTTSync", "Stop", "Enter", "stopping sync");

    if (!g_sync_running) {
        LOG_WARN_T("MQTTSync", "Stop", "NotRunning", "sync not running");
        return;
    }

    mqtt_client_disconnect();
    g_sync_running = 0;

    LOG_INFO_T("MQTTSync", "Stop", "OK", "sync stopped");
}

int mqtt_sync_send(sync_data_type_t type, const char *data, int data_len) {
    LOG_DEBUG_T("MQTTSync", "Send", "Enter", "type=%d, len=%d", type, data_len);

    if (!g_sync_running) {
        LOG_WARN_T("MQTTSync", "Send", "NotRunning", "sync not running, starting");
        if (mqtt_sync_start() != 0) {
            LOG_ERROR_T("MQTTSync", "Send", "StartFail", "failed to start sync");
            return -1;
        }
    }

    /* 【修改】自动重连检查 */
    if (!ensure_mqtt_connected()) {
        LOG_WARN_T("MQTTSync", "Send", "NotConnected", "not connected");
        return -1;
    }

    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "type", type);
    cJSON_AddStringToObject(root, "device_id", g_device_id);
    if (data && data_len > 0) {
        cJSON_AddStringToObject(root, "data", data);
    } else {
        cJSON_AddStringToObject(root, "data", "");
    }
    cJSON_AddNumberToObject(root, "timestamp", (double)time(NULL) * 1000000);
    cJSON_AddNumberToObject(root, "version", (double)time(NULL));

    char *json_str = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);

    if (!json_str) {
        LOG_ERROR_T("MQTTSync", "Send", "JSONFail", "cJSON_PrintUnformatted failed");
        return -1;
    }

    char topic[256];
    build_topic(topic, sizeof(topic), get_topic_suffix(type));
    int ret = mqtt_client_publish(topic, json_str, strlen(json_str), 1, 0);
    free(json_str);

    if (ret == 0) {
        LOG_DEBUG_T("MQTTSync", "Send", "OK", "sent type=%d", type);
    } else {
        LOG_ERROR_T("MQTTSync", "Send", "Fail", "publish failed");
    }
    return ret;
}

int mqtt_sync_register_callback(sync_data_type_t type, sync_receive_cb cb, void *user_data) {
    LOG_INFO_T("MQTTSync", "RegisterCB", "Enter", "type=%d", type);

    pthread_mutex_lock(&g_sync_lock);

    if (g_callback_count >= MAX_CALLBACKS) {
        pthread_mutex_unlock(&g_sync_lock);
        LOG_ERROR_T("MQTTSync", "RegisterCB", "Overflow", "max callbacks reached");
        return -1;
    }

    g_callbacks[g_callback_count].type = type;
    g_callbacks[g_callback_count].cb = cb;
    g_callbacks[g_callback_count].user_data = user_data;
    g_callbacks[g_callback_count].active = 1;
    g_callback_count++;

    pthread_mutex_unlock(&g_sync_lock);

    LOG_INFO_T("MQTTSync", "RegisterCB", "OK", "callback registered for type=%d", type);
    return 0;
}

int mqtt_sync_is_running(void) {
    return g_sync_running && mqtt_client_is_connected();
}

const char *mqtt_sync_get_device_id(void) {
    return g_device_id;
}

void mqtt_sync_set_reconnect_enabled(int enabled) {
    g_reconnect_enabled = enabled;
    LOG_INFO_T("MQTTSync", "Reconnect", "auto-reconnect %s",
               enabled ? tr("enabled", "启用") : tr("disabled", "禁用"));
}

void mqtt_sync_cleanup(void) {
    LOG_INFO_T("MQTTSync", "Cleanup", "Enter", "cleaning up sync system");

    if (g_sync_handler >= 0) {
        mqtt_client_remove_message_handler(g_sync_handler);
        g_sync_handler = -1;
    }

    mqtt_sync_stop();
    mqtt_client_cleanup();

    pthread_mutex_lock(&g_sync_lock);
    memset(g_callbacks, 0, sizeof(g_callbacks));
    g_callback_count = 0;
    pthread_mutex_unlock(&g_sync_lock);

    g_sync_running = 0;

    LOG_INFO_T("MQTTSync", "Cleanup", "OK", "sync system cleaned up");
}

/* ============================================================
 * MQTT 服务：唯一对外接线入口
 *   启动链：mqtt_sync_init → mqtt_ha_init → mqtt_sync_start（登记订阅）
 *   监督线程：指数退避重连 + 上线后发布可用性/HA 发现 + 周期真实指标上报
 * ============================================================ */

#define MQTT_SERVICE_TICK_MS 1000
#define MQTT_SERVICE_HA_REPORT_INTERVAL_SEC 30

static pthread_t g_service_thread;
static int g_service_running = 0;
static volatile int g_service_stop = 0;
static int g_service_ha_ready = 0;
static time_t g_service_started_at = 0;
static unsigned long g_service_report_cycles = 0;

/* 用最近一次真实采样构造快照，经 mqtt_sync_send 同步给其它设备（无采样则不发） */
static void publish_sync_snapshot(void) {
    int cpu = -1, mem = -1, disk = -1, ai_online = 0;
    double load = -1.0;

    if (!mqtt_ha_get_last_metrics(&cpu, &mem, &disk, &load, &ai_online)) {
        LOG_DEBUG_T("MQTTService", "Sync", "NoSample", "no metrics sample yet, snapshot skipped");
        return;
    }

    char json[256];
    safe_snprintf(json, sizeof(json),
                  "{\"cpu\":%d,\"mem\":%d,\"disk\":%d,\"load\":%.2f,"
                  "\"ai_online\":%d,\"ts\":%ld}",
                  cpu, mem, disk, load, ai_online, (long)time(NULL));

    int ret = mqtt_sync_send(SYNC_TYPE_HA_STATE, json, (int)strlen(json));
    if (ret != 0) {
        LOG_WARN_T("MQTTService", "Sync", "SnapshotFail", "state snapshot sync failed");
    }
}

static void *mqtt_service_thread(void *arg) {
    (void)arg;

    LOG_INFO_T("MQTTService", "Thread", "Start", "supervisor thread started");

    int was_connected = 0;
    time_t last_report = 0;
    time_t last_discovery_attempt = 0;

    while (!g_service_stop) {
        /* 1s 心跳（200ms 粒度，保证 stop 响应及时） */
        for (int i = 0; i < (MQTT_SERVICE_TICK_MS / 200) && !g_service_stop; i++) {
            usleep(200 * 1000);
        }
        if (g_service_stop) break;

        if (!mqtt_client_ensure_connected()) {
            if (was_connected) {
                LOG_WARN_T("MQTTService", "Link", "Down", "MQTT link lost, supervisor retrying with backoff");
                was_connected = 0;
            }
            continue;
        }

        if (!was_connected) {
            was_connected = 1;
            LOG_INFO_T("MQTTService", "Link", "Up", "broker online");
            if (g_service_ha_ready) {
                mqtt_ha_publish_availability(1);
            }
        }

        if (!g_service_ha_ready) continue;

        time_t now = time(NULL);

        /* HA 发现：未真正成功前每 60s 重试（retained 配置，重复发布幂等） */
        if (!mqtt_ha_is_discovery_sent() &&
            (last_discovery_attempt == 0 || now - last_discovery_attempt >= 60)) {
            last_discovery_attempt = now;
            if (mqtt_ha_send_discovery() == 0) {
                LOG_INFO_T("MQTTService", "Discovery", "Sent", "HA discovery published");
            } else {
                LOG_WARN_T("MQTTService", "Discovery", "Fail",
                           "HA discovery publish failed, will retry in 60s");
            }
        }
        if (last_report == 0 || now - last_report >= MQTT_SERVICE_HA_REPORT_INTERVAL_SEC) {
            last_report = now;
            if (mqtt_ha_report_system() >= 0) {
                g_service_report_cycles++;
                publish_sync_snapshot();   /* 真实指标 → 多设备同步 */
            } else {
                LOG_WARN_T("MQTTService", "Report", "Skip", "metrics cycle skipped (see MQTTHA logs)");
            }
        }
    }

    /* 退出前尽力发布离线可用性（HA 侧据此显示不可用） */
    if (g_service_ha_ready && mqtt_client_is_connected()) {
        mqtt_ha_publish_availability(0);
    }

    LOG_INFO_T("MQTTService", "Thread", "Stop", "supervisor thread stopped");
    return NULL;
}

int mqtt_service_start(const char *device_id, const char *sync_topic_prefix) {
    LOG_INFO_T("MQTTService", "Start", "Enter", "starting MQTT service");

    if (g_service_running) {
        LOG_WARN_T("MQTTService", "Start", "Already", "MQTT service already running");
        return 0;
    }

    if (mqtt_sync_init(device_id, sync_topic_prefix) != 0) {
        LOG_ERROR_T("MQTTService", "Start", "SyncInitFail", "mqtt_sync_init failed");
        return -1;
    }

    if (mqtt_ha_init() != 0) {
        LOG_ERROR_T("MQTTService", "Start", "HaInitFail", "mqtt_ha_init failed");
        mqtt_sync_cleanup();
        return -1;
    }
    g_service_ha_ready = 1;

    /* 多设备同步（mqtt_sync_enhanced.c）此前零调用者 → 真实初始化（服务端权威角色） */
    if (mqtt_sync_enhanced_init(mqtt_sync_get_device_id(), 1) != 0) {
        LOG_WARN_T("MQTTService", "Start", "EnhancedFail",
                   "mqtt_sync_enhanced_init failed (continuing without multi-device sync)");
    } else {
        LOG_INFO_T("MQTTService", "Start", "EnhancedOK",
                   "multi-device sync initialized (server role, device='%s')",
                   mqtt_sync_get_device_id());
    }

    /* 首次连接尝试：失败不致命（监督线程继续按退避重连，订阅已登记） */
    if (mqtt_sync_start() != 0) {
        LOG_WARN_T("MQTTService", "Start", "Offline",
                   "broker not reachable yet; service starting in offline mode");
    }

    g_service_stop = 0;
    if (pthread_create(&g_service_thread, NULL, mqtt_service_thread, NULL) != 0) {
        LOG_ERROR_T("MQTTService", "Start", "ThreadFail", "pthread_create failed");
        mqtt_ha_cleanup();
        g_service_ha_ready = 0;
        mqtt_sync_cleanup();
        return -1;
    }
    pthread_setname_np(g_service_thread, "mqtt-service");

    g_service_running = 1;
    g_service_started_at = time(NULL);
    g_service_report_cycles = 0;

    LOG_INFO_T("MQTTService", "Start", "OK", "MQTT service started (HA metrics every %ds)",
               MQTT_SERVICE_HA_REPORT_INTERVAL_SEC);
    return 0;
}

void mqtt_service_stop(void) {
    if (!g_service_running) {
        LOG_DEBUG_T("MQTTService", "Stop", "NotRunning", "MQTT service not running");
        return;
    }

    g_service_stop = 1;
    pthread_join(g_service_thread, NULL);
    g_service_running = 0;

    mqtt_sync_stop();
    if (g_service_ha_ready) {
        mqtt_ha_cleanup();
        g_service_ha_ready = 0;
    }
    mqtt_sync_cleanup();

    g_service_started_at = 0;
    LOG_INFO_T("MQTTService", "Stop", "OK", "MQTT service stopped");
}

int mqtt_service_is_running(void) {
    return g_service_running;
}

void mqtt_service_get_status(mqtt_service_status_t *out) {
    if (!out) return;
    memset(out, 0, sizeof(*out));

    out->running = g_service_running;
    out->connected = mqtt_client_is_connected();
    out->reconnect_attempts = mqtt_client_get_reconnect_attempts();
    out->subscriptions = mqtt_client_get_subscription_count();
    mqtt_client_get_publish_stats(&out->publish_ok, &out->publish_failed);
    out->report_cycles = g_service_report_cycles;
    out->uptime_sec = (g_service_started_at > 0)
                          ? (long)(time(NULL) - g_service_started_at)
                          : 0;
}
