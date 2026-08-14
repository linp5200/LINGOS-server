/**
 * @file    mqtt_sync.c
 * @brief   MQTT 多设备同步实现
 * @version LN-B-5.0.0.0
 * @changes 自动重连增强；安全字符串替换；双文支持
 */

#include "mqtt_sync.h"
#include "mqtt_client.h"
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
    safe_strncpy(config.broker, "127.0.0.1", sizeof(config.broker));
    config.port = 1883;
    config.keepalive = 60;
    config.clean_session = 1;
    config.on_message = on_sync_message;

    int ret = mqtt_client_init(&config);
    if (ret != 0) {
        LOG_ERROR_T("MQTTSync", "Init", "MQTTFail", "mqtt_client_init failed");
        return -1;
    }

    LOG_INFO_T("MQTTSync", "Init", "OK", "sync system initialized, device='%s'", g_device_id);
    return 0;
}

int mqtt_sync_start(void) {
    LOG_INFO_T("MQTTSync", "Start", "Enter", "starting MQTT sync");

    if (g_sync_running) {
        LOG_WARN_T("MQTTSync", "Start", "Already", "sync already running");
        return 0;
    }

    if (!ensure_mqtt_connected()) {
        LOG_ERROR_T("MQTTSync", "Start", "ConnectFail", "MQTT connection failed");
        return -1;
    }

    char topic[256];
    build_broadcast_topic(topic, sizeof(topic), "memory");
    mqtt_client_subscribe(topic, 1);
    build_broadcast_topic(topic, sizeof(topic), "config");
    mqtt_client_subscribe(topic, 1);
    build_broadcast_topic(topic, sizeof(topic), "history");
    mqtt_client_subscribe(topic, 1);
    build_broadcast_topic(topic, sizeof(topic), "alias");
    mqtt_client_subscribe(topic, 1);
    build_broadcast_topic(topic, sizeof(topic), "reminder");
    mqtt_client_subscribe(topic, 1);
    build_broadcast_topic(topic, sizeof(topic), "ha_state");
    mqtt_client_subscribe(topic, 1);

    g_sync_running = 1;
    g_reconnect_attempts = 0;
    LOG_INFO_T("MQTTSync", "Start", "OK", "sync started");
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

void mqtt_sync_set_reconnect_enabled(int enabled) {
    g_reconnect_enabled = enabled;
    LOG_INFO_T("MQTTSync", "Reconnect", "auto-reconnect %s",
               enabled ? tr("enabled", "启用") : tr("disabled", "禁用"));
}

void mqtt_sync_cleanup(void) {
    LOG_INFO_T("MQTTSync", "Cleanup", "Enter", "cleaning up sync system");

    mqtt_sync_stop();
    mqtt_client_cleanup();

    pthread_mutex_lock(&g_sync_lock);
    memset(g_callbacks, 0, sizeof(g_callbacks));
    g_callback_count = 0;
    pthread_mutex_unlock(&g_sync_lock);

    g_sync_running = 0;

    LOG_INFO_T("MQTTSync", "Cleanup", "OK", "sync system cleaned up");
}
