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

#define MQTT_MAX_SUBSCRIPTIONS 32
#define MQTT_MAX_HANDLERS      8
#define RECONNECT_BACKOFF_BASE 2
#define RECONNECT_BACKOFF_MAX  60
#define CONNECT_WAIT_STEPS     20   /* 20 * 100ms = 2s */

/* 订阅登记表：连接（含重连）建立后自动重订阅——此前重连后订阅全丢 */
typedef struct {
    char topic[256];
    int  qos;
    int  active;
} mqtt_sub_entry_t;

/* 多订阅者消息处理器：按主题过滤器分发（HA 命令 / 同步数据互不干扰） */
typedef struct {
    char filter[256];
    mqtt_message_cb cb;
    void *user_data;
    int  active;
} mqtt_handler_entry_t;

static struct mosquitto *g_mosq = NULL;
static mqtt_config_t g_config;
static mqtt_state_t g_state = MQTT_STATE_DISCONNECTED;
static int g_initialized = 0;
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static int g_reconnect_attempts = 0;
static int g_reconnect_enabled = 1;
static int g_loop_started = 0;          /* mosquitto_loop_start 是否已启动 */
static time_t g_next_attempt_at = 0;    /* 指数退避：下次允许重连的时刻 */
static mqtt_sub_entry_t g_subs[MQTT_MAX_SUBSCRIPTIONS];
static mqtt_handler_entry_t g_handlers[MQTT_MAX_HANDLERS];
static unsigned long g_pub_ok = 0;
static unsigned long g_pub_fail = 0;

static void resubscribe_all(void);
static void dispatch_message(const mqtt_message_t *message);
static void note_reconnect_failure(const char *who);
static void note_publish_result(int ok);
static int  remember_subscription(const char *topic, int qos);
static int  forget_subscription(const char *topic);

/* ============================================================
 * Libmosquitto 回调函数（C 风格）
 * ============================================================ */

static void on_connect_cb(struct mosquitto *mosq, void *obj, int rc) {
    (void)mosq;
    (void)obj;

    mqtt_connect_cb user_cb = NULL;
    void *user_data = NULL;

    /* 只在锁内更新状态并取出回调：用户回调可能反向调用 publish/subscribe，
     * 持锁调用会与 g_lock（非递归）自死锁 */
    pthread_mutex_lock(&g_lock);
    user_cb = g_config.on_connect;
    user_data = g_config.user_data;
    if (rc == 0) {
        g_state = MQTT_STATE_CONNECTED;
        g_reconnect_attempts = 0;
        g_next_attempt_at = 0;
    } else {
        g_state = MQTT_STATE_ERROR;
    }
    pthread_mutex_unlock(&g_lock);

    if (rc == 0) {
        LOG_INFO_T("MQTTClient", "Connect", "OK", "connected to broker");
        /* 【真实重连链】重连成功后恢复全部订阅（否则 HA 命令/同步数据在重连后失聪） */
        resubscribe_all();
    } else {
        LOG_ERROR_T("MQTTClient", "Connect", "Fail", "connection failed: %d (%s)",
                    rc, mosquitto_strerror(rc));
        note_reconnect_failure("broker-connect");
    }

    if (user_cb) {
        user_cb(rc == 0 ? 1 : 0, user_data);
    }
}

static void on_disconnect_cb(struct mosquitto *mosq, void *obj, int rc) {
    (void)mosq;
    (void)obj;

    int want_reconnect = 0;

    pthread_mutex_lock(&g_lock);
    g_state = MQTT_STATE_DISCONNECTED;
    if (rc != 0 && g_reconnect_enabled && g_initialized) {
        want_reconnect = 1;
    }
    pthread_mutex_unlock(&g_lock);

    if (rc != 0) {
        LOG_WARN_T("MQTTClient", "Disconnect", "Unexpected", "disconnected unexpectedly, rc=%d", rc);
        if (want_reconnect) {
            /* 真实重连由监督线程 mqtt_client_ensure_connected() 执行（指数退避） */
            LOG_INFO_T("MQTTClient", "Disconnect", "Reconnect", "supervisor will retry with backoff");
        }
    } else {
        LOG_DEBUG_T("MQTTClient", "Disconnect", "OK", "disconnected cleanly");
    }
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

    dispatch_message(&message);
}

/* ============================================================
 * 内部：消息分发（兼容回调 + 主题过滤器匹配的处理器）
 * ============================================================ */

static void dispatch_message(const mqtt_message_t *message) {
    mqtt_message_cb legacy = NULL;
    void *legacy_data = NULL;
    mqtt_handler_entry_t snapshot[MQTT_MAX_HANDLERS];
    int n = 0;

    pthread_mutex_lock(&g_lock);
    legacy = g_config.on_message;
    legacy_data = g_config.user_data;
    for (int i = 0; i < MQTT_MAX_HANDLERS; i++) {
        if (g_handlers[i].active) snapshot[n++] = g_handlers[i];
    }
    pthread_mutex_unlock(&g_lock);

    if (legacy) legacy(message, legacy_data);

    for (int i = 0; i < n; i++) {
        if (!snapshot[i].cb) continue;
        if (!mqtt_client_topic_matches(snapshot[i].filter, message->topic)) continue;
        snapshot[i].cb(message, snapshot[i].user_data);
    }
}

/* ============================================================
 * 内部：重订阅 / 退避记账
 * ============================================================ */

static void resubscribe_all(void) {
    mqtt_sub_entry_t snapshot[MQTT_MAX_SUBSCRIPTIONS];
    int n = 0;

    pthread_mutex_lock(&g_lock);
    for (int i = 0; i < MQTT_MAX_SUBSCRIPTIONS; i++) {
        if (g_subs[i].active) snapshot[n++] = g_subs[i];
    }
    struct mosquitto *mosq = g_mosq;
    pthread_mutex_unlock(&g_lock);

    if (!mosq || n == 0) return;

    for (int i = 0; i < n; i++) {
        int rc = mosquitto_subscribe(mosq, NULL, snapshot[i].topic, snapshot[i].qos);
        if (rc != MOSQ_ERR_SUCCESS) {
            LOG_WARN_T("MQTTClient", "Resubscribe", "Fail", "topic='%s': %s",
                       snapshot[i].topic, mosquitto_strerror(rc));
        } else {
            LOG_INFO_T("MQTTClient", "Resubscribe", "OK", "restored subscription '%s'", snapshot[i].topic);
        }
    }
    LOG_INFO_T("MQTTClient", "Resubscribe", "Done", "restored %d subscription(s)", n);
}

static void note_reconnect_failure(const char *who) {
    int attempts, backoff;

    pthread_mutex_lock(&g_lock);
    g_reconnect_attempts++;
    attempts = g_reconnect_attempts;
    int shift = attempts - 1;
    if (shift > 5) shift = 5;
    backoff = RECONNECT_BACKOFF_BASE << shift;
    if (backoff > RECONNECT_BACKOFF_MAX) backoff = RECONNECT_BACKOFF_MAX;
    g_next_attempt_at = time(NULL) + backoff;
    pthread_mutex_unlock(&g_lock);

    LOG_WARN_T("MQTTClient", "Reconnect", "Backoff", "%s failed, attempt %d, retry in %ds",
               who ? who : "connect", attempts, backoff);
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
    g_next_attempt_at = 0;
    g_loop_started = 0;

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

    pthread_mutex_lock(&g_lock);
    g_loop_started = 1;
    pthread_mutex_unlock(&g_lock);

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
    g_loop_started = 0;
    pthread_mutex_unlock(&g_lock);

    LOG_INFO_T("MQTTClient", "Disconnect", "OK", "disconnected");
}

int mqtt_client_publish(const char *topic, const char *payload,
                        int payload_len, int qos, int retained) {
    LOG_DEBUG_T("MQTTClient", "Publish", "Enter", "topic='%s', len=%d, qos=%d",
                topic ? topic : "(null)", payload_len, qos);

    if (!topic || !*topic || !payload) {
        LOG_ERROR_T("MQTTClient", "Publish", "Invalid", "topic/payload is empty");
        note_publish_result(0);
        return -1;
    }

    if (!g_initialized || !g_mosq) {
        LOG_ERROR_T("MQTTClient", "Publish", "NotInit", "client not initialized");
        note_publish_result(0);
        return -1;
    }

    if (!mqtt_client_is_connected()) {
        LOG_WARN_T("MQTTClient", "Publish", "NotConnected", "not connected, dropped topic='%s'", topic);
        note_publish_result(0);
        return -1;
    }

    int len = (payload_len < 0) ? (int)strlen(payload) : payload_len;
    int rc = mosquitto_publish(g_mosq, NULL, topic, len, payload, qos, retained);
    if (rc != 0) {
        LOG_ERROR_T("MQTTClient", "Publish", "Fail", "mosquitto_publish: %s",
                    mosquitto_strerror(rc));
        note_publish_result(0);
        return -1;
    }

    note_publish_result(1);
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

    /* 先登记订阅意图：即使当前未连接，连接/重连建立后由 resubscribe_all() 真正订阅 */
    if (remember_subscription(topic, qos) != 0) {
        LOG_ERROR_T("MQTTClient", "Subscribe", "RegistryFull", "subscription table full, '%s' not remembered", topic);
        return -1;
    }

    if (!mqtt_client_is_connected()) {
        LOG_WARN_T("MQTTClient", "Subscribe", "Deferred",
                   "not connected yet, '%s' queued and will be subscribed on connect", topic);
        return 0;
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

    forget_subscription(topic);

    if (!mqtt_client_is_connected()) {
        LOG_WARN_T("MQTTClient", "Unsubscribe", "NotConnected",
                   "not connected, '%s' removed from subscription registry", topic);
        return 0;
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
    pthread_mutex_lock(&g_lock);
    g_config.on_message = cb;
    g_config.user_data = user_data;
    pthread_mutex_unlock(&g_lock);
    LOG_DEBUG_T("MQTTClient", "SetCallback", "OK", "callback set");
}

void mqtt_client_set_reconnect_enabled(int enabled) {
    pthread_mutex_lock(&g_lock);
    g_reconnect_enabled = enabled;
    if (!enabled) g_next_attempt_at = 0;
    pthread_mutex_unlock(&g_lock);
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

/* ============================================================
 * 订阅登记表内部实现
 * ============================================================ */

static int remember_subscription(const char *topic, int qos) {
    int free_slot = -1;

    pthread_mutex_lock(&g_lock);
    for (int i = 0; i < MQTT_MAX_SUBSCRIPTIONS; i++) {
        if (g_subs[i].active && strcmp(g_subs[i].topic, topic) == 0) {
            g_subs[i].qos = qos;
            pthread_mutex_unlock(&g_lock);
            return 0;
        }
        if (!g_subs[i].active && free_slot < 0) free_slot = i;
    }

    if (free_slot < 0) {
        pthread_mutex_unlock(&g_lock);
        return -1;
    }

    safe_strncpy(g_subs[free_slot].topic, topic, sizeof(g_subs[free_slot].topic));
    g_subs[free_slot].qos = qos;
    g_subs[free_slot].active = 1;
    pthread_mutex_unlock(&g_lock);
    return 0;
}

static int forget_subscription(const char *topic) {
    int found = -1;

    pthread_mutex_lock(&g_lock);
    for (int i = 0; i < MQTT_MAX_SUBSCRIPTIONS; i++) {
        if (g_subs[i].active && strcmp(g_subs[i].topic, topic) == 0) {
            g_subs[i].active = 0;
            found = 0;
            break;
        }
    }
    pthread_mutex_unlock(&g_lock);
    return found;
}

static void note_publish_result(int ok) {
    pthread_mutex_lock(&g_lock);
    if (ok) g_pub_ok++;
    else g_pub_fail++;
    pthread_mutex_unlock(&g_lock);
}

int mqtt_client_get_subscription_count(void) {
    int n = 0;

    pthread_mutex_lock(&g_lock);
    for (int i = 0; i < MQTT_MAX_SUBSCRIPTIONS; i++) {
        if (g_subs[i].active) n++;
    }
    pthread_mutex_unlock(&g_lock);
    return n;
}

/* ============================================================
 * 多订阅者消息处理器
 * ============================================================ */

int mqtt_client_add_message_handler(const char *topic_filter,
                                    mqtt_message_cb cb, void *user_data) {
    if (!cb) {
        LOG_ERROR_T("MQTTClient", "AddHandler", "Invalid", "callback is NULL");
        return -1;
    }

    const char *filter = (topic_filter && *topic_filter) ? topic_filter : "#";

    pthread_mutex_lock(&g_lock);
    for (int i = 0; i < MQTT_MAX_HANDLERS; i++) {
        if (!g_handlers[i].active) {
            safe_strncpy(g_handlers[i].filter, filter, sizeof(g_handlers[i].filter));
            g_handlers[i].cb = cb;
            g_handlers[i].user_data = user_data;
            g_handlers[i].active = 1;
            pthread_mutex_unlock(&g_lock);
            LOG_INFO_T("MQTTClient", "AddHandler", "OK", "handle=%d, filter='%s'", i, filter);
            return i;
        }
    }
    pthread_mutex_unlock(&g_lock);

    LOG_ERROR_T("MQTTClient", "AddHandler", "Full", "no free handler slot (max %d)", MQTT_MAX_HANDLERS);
    return -1;
}

int mqtt_client_remove_message_handler(int handle) {
    if (handle < 0 || handle >= MQTT_MAX_HANDLERS) return -1;

    pthread_mutex_lock(&g_lock);
    int was_active = g_handlers[handle].active;
    g_handlers[handle].active = 0;
    g_handlers[handle].cb = NULL;
    g_handlers[handle].user_data = NULL;
    g_handlers[handle].filter[0] = '\0';
    pthread_mutex_unlock(&g_lock);

    if (!was_active) {
        LOG_WARN_T("MQTTClient", "RemoveHandler", "NotActive", "handle=%d was not active", handle);
        return -1;
    }
    LOG_INFO_T("MQTTClient", "RemoveHandler", "OK", "handle=%d removed", handle);
    return 0;
}

int mqtt_client_topic_matches(const char *filter, const char *topic) {
    if (!filter || !topic) return 0;

    const char *f = filter;
    const char *t = topic;

    while (*f) {
        const char *fs = strchr(f, '/');
        const char *ts = strchr(t, '/');
        size_t fl = fs ? (size_t)(fs - f) : strlen(f);
        size_t tl = ts ? (size_t)(ts - t) : strlen(t);

        /* '#' 匹配剩余所有层级（含父层级：a/# 也匹配 a） */
        if (fl == 1 && f[0] == '#') return 1;

        /* '+' 匹配单层，其它必须逐字节相等 */
        if (!(fl == 1 && f[0] == '+')) {
            if (fl != tl || strncmp(f, t, fl) != 0) return 0;
        }

        if (!fs && !ts) return 1;

        if (!fs || !ts) {
            /* 过滤器还有层级：a/# 可匹配 a（父层），其余不匹配 */
            if (fs && fs[1] == '#' && fs[2] == '\0') return 1;
            return 0;
        }

        f = fs + 1;
        t = ts + 1;
    }

    return *t == '\0';
}

/* ============================================================
 * 真实重连（指数退避 + 连接等待）
 * ============================================================ */

int mqtt_client_ensure_connected(void) {
    if (mqtt_client_is_connected()) {
        pthread_mutex_lock(&g_lock);
        g_reconnect_attempts = 0;
        g_next_attempt_at = 0;
        pthread_mutex_unlock(&g_lock);
        return 1;
    }

    int initialized, enabled, loop_started;
    time_t next_at;

    pthread_mutex_lock(&g_lock);
    initialized = g_initialized;
    enabled = g_reconnect_enabled;
    loop_started = g_loop_started;
    next_at = g_next_attempt_at;
    pthread_mutex_unlock(&g_lock);

    if (!initialized || !g_mosq) {
        LOG_WARN_T("MQTTClient", "Ensure", "NotInit", "client not initialized");
        return 0;
    }
    if (!enabled) {
        LOG_DEBUG_T("MQTTClient", "Ensure", "Disabled", "auto-reconnect disabled");
        return 0;
    }

    time_t now = time(NULL);
    if (next_at > now) {
        LOG_DEBUG_T("MQTTClient", "Ensure", "Backoff", "waiting %lds before next attempt",
                    (long)(next_at - now));
        return 0;
    }

    if (!loop_started) {
        /* 首次连接或 loop 已停止（disconnect/cleanup）→ 走完整连接路径 */
        if (mqtt_client_connect() != 0) {
            note_reconnect_failure("full-connect");
            return 0;
        }
    } else {
        int rc = mosquitto_reconnect(g_mosq);
        if (rc != MOSQ_ERR_SUCCESS) {
            LOG_WARN_T("MQTTClient", "Ensure", "ReconnectFail", "mosquitto_reconnect: %s",
                       mosquitto_strerror(rc));
            note_reconnect_failure("reconnect");
            return 0;
        }
        LOG_INFO_T("MQTTClient", "Ensure", "Reconnect", "reconnect requested (attempt %d)",
                   g_reconnect_attempts + 1);
    }

    for (int i = 0; i < CONNECT_WAIT_STEPS; i++) {
        if (mqtt_client_is_connected()) {
            pthread_mutex_lock(&g_lock);
            g_reconnect_attempts = 0;
            g_next_attempt_at = 0;
            pthread_mutex_unlock(&g_lock);
            LOG_INFO_T("MQTTClient", "Ensure", "OK", "connection established");
            return 1;
        }
        usleep(100 * 1000);
    }

    /* on_connect 回调已就此次失败记过退避 → 不重复计数 */
    if (mqtt_client_get_state() == MQTT_STATE_ERROR) {
        LOG_DEBUG_T("MQTTClient", "Ensure", "Failed",
                    "connection attempt failed (backoff already scheduled by connect callback)");
        return 0;
    }

    note_reconnect_failure("connect-wait");
    return 0;
}

int mqtt_client_get_reconnect_enabled(void) {
    pthread_mutex_lock(&g_lock);
    int enabled = g_reconnect_enabled;
    pthread_mutex_unlock(&g_lock);
    return enabled;
}

int mqtt_client_get_reconnect_attempts(void) {
    pthread_mutex_lock(&g_lock);
    int attempts = g_reconnect_attempts;
    pthread_mutex_unlock(&g_lock);
    return attempts;
}

void mqtt_client_get_publish_stats(unsigned long *ok, unsigned long *failed) {
    pthread_mutex_lock(&g_lock);
    unsigned long o = g_pub_ok, f = g_pub_fail;
    pthread_mutex_unlock(&g_lock);
    if (ok) *ok = o;
    if (failed) *failed = f;
}