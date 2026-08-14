/**
 * @file    mqtt_client.c
 * @brief   MQTT 客户端实现（基于 libmosquitto）
 * @version LN-B-5.0.0.0
 * @changes 连接状态回调完善；安全字符串替换；自动重连增强
 */

#include "mqtt_client.h"
#include "../common/safe_string.h"
#include "../common/lang.h"
#include "../lib/log_extra.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <mosquitto.h>
#include <errno.h>
#include <pthread.h>

/* ============================================================
 * 全局状态
 * ============================================================ */

static struct mosquitto *g_mosq = NULL;
static mqtt_config_t g_config;
static mqtt_state_t g_state = MQTT_STATE_DISCONNECTED;
static int g_initialized = 0;
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static int g_reconnect_attempts = 0;
static int g_reconnect_enabled = 1;

/* ============================================================
 * Libmosquitto 回调函数（C 风格）
 * ============================================================ */

static void on_connect_cb(struct mosquitto *mosq, void *obj, int rc) {
    (void)mosq;
    (void)obj;

    pthread_mutex_lock(&g_lock);
    if (rc == 0) {
        g_state = MQTT_STATE_CONNECTED;
        g_reconnect_attempts = 0;
        LOG_INFO_T("MQTTClient", "Connect", "OK", "connected to broker");
        if (g_config.on_connect) {
            g_config.on_connect(1, g_config.user_data);
        }
    } else {
        g_state = MQTT_STATE_ERROR;
        LOG_ERROR_T("MQTTClient", "Connect", "Fail", "connection failed: %d (%s)",
                    rc, mosquitto_strerror(rc));
        if (g_config.on_connect) {
            g_config.on_connect(0, g_config.user_data);
        }
        /* 触发自动重连 */
        if (g_reconnect_enabled) {
            LOG_INFO_T("MQTTClient", "Connect", "Reconnect", "auto-reconnect scheduled");
        }
    }
    pthread_mutex_unlock(&g_lock);
}

static void on_disconnect_cb(struct mosquitto *mosq, void *obj, int rc) {
    (void)mosq;
    (void)obj;

    pthread_mutex_lock(&g_lock);
    if (rc != 0) {
        g_state = MQTT_STATE_DISCONNECTED;
        LOG_WARN_T("MQTTClient", "Disconnect", "Unexpected", "disconnected unexpectedly, rc=%d", rc);
        if (g_reconnect_enabled && g_initialized) {
            LOG_INFO_T("MQTTClient", "Disconnect", "Reconnect", "will attempt to reconnect");
        }
    } else {
        g_state = MQTT_STATE_DISCONNECTED;
        LOG_DEBUG_T("MQTTClient", "Disconnect", "OK", "disconnected cleanly");
    }
    pthread_mutex_unlock(&g_lock);
}

static void on_message_cb(struct mosquitto *mosq, void *obj,
                          const struct mosquitto_message *msg) {
    (void)mosq;
    (void)obj;

    if (!msg) return;

    mqtt_message_t message;
    memset(&message, 0, sizeof(message));
    safe_strncpy(message.topic, msg->topic, sizeof(message.topic));
    if (msg->payload && msg->payloadlen > 0) {
        int len = msg->payloadlen;
        if (len > sizeof(message.payload) - 1) len = sizeof(message.payload) - 1;
        memcpy(message.payload, msg->payload, len);
        message.payload[len] = '\0';
        message.payload_len = len;
    }
    message.qos = msg->qos;
    message.retained = msg->retain;

    LOG_DEBUG_T("MQTTClient", "Message", "Received", "topic='%s', len=%d",
                message.topic, message.payload_len);

    if (g_config.on_message) {
        g_config.on_message(&message, g_config.user_data);
    }
}

/* ============================================================
 * 公共 API 实现
 * ============================================================ */

int mqtt_client_init(const mqtt_config_t *config) {
    LOG_INFO_T("MQTTClient", "Init", "Enter", "broker='%s', port=%d",
               config ? config->broker : "(null)", config ? config->port : 0);

    if (!config) {
        LOG_ERROR_T("MQTTClient", "Init", "Invalid", "config is NULL");
        return -1;
    }

    if (g_initialized) {
        LOG_WARN_T("MQTTClient", "Init", "Already", "already initialized");
        return 0;
    }

    memcpy(&g_config, config, sizeof(mqtt_config_t));

    mosquitto_lib_init();

    char client_id[64];
    if (g_config.client_id[0] == '\0') {
        safe_snprintf(client_id, sizeof(client_id), "lingos_%d", getpid());
    } else {
        safe_strncpy(client_id, g_config.client_id, sizeof(client_id));
    }

    g_mosq = mosquitto_new(client_id, g_config.clean_session, &g_config);
    if (!g_mosq) {
        LOG_ERROR_T("MQTTClient", "Init", "MosquittoFail", "mosquitto_new failed");
        return -1;
    }

    if (g_config.username[0] != '\0') {
        if (mosquitto_username_pw_set(g_mosq, g_config.username, g_config.password) != 0) {
            LOG_WARN_T("MQTTClient", "Init", "AuthFail", "username/password set failed");
        }
    }

    mosquitto_connect_callback_set(g_mosq, on_connect_cb);
    mosquitto_disconnect_callback_set(g_mosq, on_disconnect_cb);
    mosquitto_message_callback_set(g_mosq, on_message_cb);

    g_initialized = 1;
    g_state = MQTT_STATE_DISCONNECTED;
    g_reconnect_attempts = 0;

    LOG_INFO_T("MQTTClient", "Init", "OK", "client initialized, id='%s'", client_id);
    return 0;
}

int mqtt_client_connect(void) {
    LOG_INFO_T("MQTTClient", "Connect", "Enter", "broker='%s', port=%d",
               g_config.broker, g_config.port);

    if (!g_initialized || !g_mosq) {
        LOG_ERROR_T("MQTTClient", "Connect", "NotInit", "client not initialized");
        return -1;
    }

    pthread_mutex_lock(&g_lock);
    g_state = MQTT_STATE_CONNECTING;
    pthread_mutex_unlock(&g_lock);

    int rc = mosquitto_connect(g_mosq, g_config.broker, g_config.port, g_config.keepalive);
    if (rc != 0) {
        LOG_ERROR_T("MQTTClient", "Connect", "Fail", "mosquitto_connect: %s",
                    mosquitto_strerror(rc));
        pthread_mutex_lock(&g_lock);
        g_state = MQTT_STATE_ERROR;
        pthread_mutex_unlock(&g_lock);
        return -1;
    }

    rc = mosquitto_loop_start(g_mosq);
    if (rc != 0) {
        LOG_ERROR_T("MQTTClient", "Connect", "LoopStartFail", "mosquitto_loop_start: %s",
                    mosquitto_strerror(rc));
        pthread_mutex_lock(&g_lock);
        g_state = MQTT_STATE_ERROR;
        pthread_mutex_unlock(&g_lock);
        return -1;
    }

    LOG_INFO_T("MQTTClient", "Connect", "OK", "connecting...");
    return 0;
}

void mqtt_client_disconnect(void) {
    LOG_INFO_T("MQTTClient", "Disconnect", "Enter", "disconnecting");

    if (!g_initialized || !g_mosq) {
        LOG_WARN_T("MQTTClient", "Disconnect", "NotInit", "client not initialized");
        return;
    }

    mosquitto_loop_stop(g_mosq, 0);
    mosquitto_disconnect(g_mosq);

    pthread_mutex_lock(&g_lock);
    g_state = MQTT_STATE_DISCONNECTED;
    pthread_mutex_unlock(&g_lock);

    LOG_INFO_T("MQTTClient", "Disconnect", "OK", "disconnected");
}

int mqtt_client_publish(const char *topic, const char *payload,
                        int payload_len, int qos, int retained) {
    LOG_DEBUG_T("MQTTClient", "Publish", "Enter", "topic='%s', len=%d, qos=%d",
                topic ? topic : "(null)", payload_len, qos);

    if (!topic || !*topic) {
        LOG_ERROR_T("MQTTClient", "Publish", "Invalid", "topic is empty");
        return -1;
    }

    if (!g_initialized || !g_mosq) {
        LOG_ERROR_T("MQTTClient", "Publish", "NotInit", "client not initialized");
        return -1;
    }

    if (!mqtt_client_is_connected()) {
        LOG_WARN_T("MQTTClient", "Publish", "NotConnected", "not connected");
        return -1;
    }

    int len = (payload_len < 0) ? strlen(payload) : payload_len;
    int rc = mosquitto_publish(g_mosq, NULL, topic, len, payload, qos, retained);
    if (rc != 0) {
        LOG_ERROR_T("MQTTClient", "Publish", "Fail", "mosquitto_publish: %s",
                    mosquitto_strerror(rc));
        return -1;
    }

    LOG_DEBUG_T("MQTTClient", "Publish", "OK", "published to %s", topic);
    return 0;
}

int mqtt_client_subscribe(const char *topic, int qos) {
    LOG_INFO_T("MQTTClient", "Subscribe", "Enter", "topic='%s', qos=%d",
               topic ? topic : "(null)", qos);

    if (!topic || !*topic) {
        LOG_ERROR_T("MQTTClient", "Subscribe", "Invalid", "topic is empty");
        return -1;
    }

    if (!g_initialized || !g_mosq) {
        LOG_ERROR_T("MQTTClient", "Subscribe", "NotInit", "client not initialized");
        return -1;
    }

    if (!mqtt_client_is_connected()) {
        LOG_WARN_T("MQTTClient", "Subscribe", "NotConnected", "not connected");
        return -1;
    }

    int rc = mosquitto_subscribe(g_mosq, NULL, topic, qos);
    if (rc != 0) {
        LOG_ERROR_T("MQTTClient", "Subscribe", "Fail", "mosquitto_subscribe: %s",
                    mosquitto_strerror(rc));
        return -1;
    }

    LOG_INFO_T("MQTTClient", "Subscribe", "OK", "subscribed to %s", topic);
    return 0;
}

int mqtt_client_unsubscribe(const char *topic) {
    LOG_INFO_T("MQTTClient", "Unsubscribe", "Enter", "topic='%s'", topic ? topic : "(null)");

    if (!topic || !*topic) {
        LOG_ERROR_T("MQTTClient", "Unsubscribe", "Invalid", "topic is empty");
        return -1;
    }

    if (!g_initialized || !g_mosq) {
        LOG_ERROR_T("MQTTClient", "Unsubscribe", "NotInit", "client not initialized");
        return -1;
    }

    if (!mqtt_client_is_connected()) {
        LOG_WARN_T("MQTTClient", "Unsubscribe", "NotConnected", "not connected");
        return -1;
    }

    int rc = mosquitto_unsubscribe(g_mosq, NULL, topic);
    if (rc != 0) {
        LOG_ERROR_T("MQTTClient", "Unsubscribe", "Fail", "mosquitto_unsubscribe: %s",
                    mosquitto_strerror(rc));
        return -1;
    }

    LOG_INFO_T("MQTTClient", "Unsubscribe", "OK", "unsubscribed from %s", topic);
    return 0;
}

mqtt_state_t mqtt_client_get_state(void) {
    pthread_mutex_lock(&g_lock);
    mqtt_state_t state = g_state;
    pthread_mutex_unlock(&g_lock);
    return state;
}

int mqtt_client_is_connected(void) {
    return (mqtt_client_get_state() == MQTT_STATE_CONNECTED);
}

void mqtt_client_set_callback(mqtt_message_cb cb, void *user_data) {
    g_config.on_message = cb;
    g_config.user_data = user_data;
    LOG_DEBUG_T("MQTTClient", "SetCallback", "OK", "callback set");
}

void mqtt_client_set_reconnect_enabled(int enabled) {
    g_reconnect_enabled = enabled;
    LOG_INFO_T("MQTTClient", "Reconnect", "auto-reconnect %s",
               enabled ? tr("enabled", "启用") : tr("disabled", "禁用"));
}

void mqtt_client_cleanup(void) {
    LOG_INFO_T("MQTTClient", "Cleanup", "Enter", "cleaning up MQTT client");

    mqtt_client_disconnect();

    if (g_mosq) {
        mosquitto_destroy(g_mosq);
        g_mosq = NULL;
    }

    mosquitto_lib_cleanup();

    g_initialized = 0;
    pthread_mutex_lock(&g_lock);
    g_state = MQTT_STATE_DISCONNECTED;
    g_reconnect_attempts = 0;
    pthread_mutex_unlock(&g_lock);

    LOG_INFO_T("MQTTClient", "Cleanup", "OK", "MQTT client cleaned up");
}

const char* mqtt_state_name(mqtt_state_t state) {
    switch (state) {
        case MQTT_STATE_DISCONNECTED: return tr("disconnected", "已断开");
        case MQTT_STATE_CONNECTING:   return tr("connecting", "连接中");
        case MQTT_STATE_CONNECTED:    return tr("connected", "已连接");
        case MQTT_STATE_ERROR:        return tr("error", "错误");
        default:                      return tr("unknown", "未知");
    }
}