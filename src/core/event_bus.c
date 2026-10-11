/**
 * @file    src/core/event_bus.c
 * @brief   系统事件总线实现（发布—订阅 + 最近事件环形缓冲 + 观测计数）
 * @version LN-0.9.0
 * @par     线程安全：订阅表/计数用互斥锁保护；回调在**解锁后**执行（避免回调重入死锁）。
 *          发布重入深度上限 4；超限丢弃并计数（防事件风暴）。
 */

#include "event_bus.h"
#include "../common/safe_string.h"
#include "../lib/log_extra.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <time.h>
#include <pthread.h>

/* ============================================================
 * 内部状态
 * ============================================================ */
typedef struct {
    event_handler_t handler;
    void *userdata;
    int active;
} event_sub_t;

typedef struct {
    event_type_t type;
    time_t at;
    char payload[EVENT_PAYLOAD_MAX];
} event_recent_t;

static event_sub_t g_subs[EVT_COUNT][EVENT_MAX_SUBSCRIBERS];
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static unsigned long g_published[EVT_COUNT];
static unsigned long g_delivered = 0;
static unsigned long g_dropped = 0;
static event_recent_t g_recent[EVENT_RECENT_MAX];
static int g_recent_head = 0;
static int g_initialized = 0;
static __thread int t_publish_depth = 0;

static const char *const EVENT_NAMES[EVT_COUNT] = {
    "none",
    "config.changed",
    "log_level.changed",
    "defense_mode.changed",
    "permission.denied",
    "permission.whitelist_changed",
    "skill.loaded",
    "plugin.loaded",
    "monitor.state_changed",
    "monitor.frame",
    "vision.result",
    "alert.raised",
    "crisis.entered",
    "ai_server.state",
    "device.state_changed",
    "ha.state_changed"
};

/* ============================================================
 * 初始化
 * ============================================================ */
int event_bus_init(void) {
    int i, j;
    pthread_mutex_lock(&g_lock);
    for (i = 0; i < EVT_COUNT; i++) {
        g_published[i] = 0;
        for (j = 0; j < EVENT_MAX_SUBSCRIBERS; j++) {
            g_subs[i][j].handler = NULL;
            g_subs[i][j].userdata = NULL;
            g_subs[i][j].active = 0;
        }
    }
    for (i = 0; i < EVENT_RECENT_MAX; i++) {
        g_recent[i].type = EVT_NONE;
        g_recent[i].at = 0;
        g_recent[i].payload[0] = '\0';
    }
    g_recent_head = 0;
    g_delivered = 0;
    g_dropped = 0;
    g_initialized = 1;
    pthread_mutex_unlock(&g_lock);
    LOG_INFO_T("EventBus", "Init", "OK", "event bus ready (%d types, %d recent)",
               (int)EVT_COUNT, EVENT_RECENT_MAX);
    return 0;
}

/* ============================================================
 * 订阅 / 退订
 * ============================================================ */
int event_bus_subscribe(event_type_t type, event_handler_t handler, void *userdata) {
    int i;
    int free_slot = -1;
    if (type <= EVT_NONE || type >= EVT_COUNT || !handler) return -1;
    if (!g_initialized) event_bus_init();

    pthread_mutex_lock(&g_lock);
    for (i = 0; i < EVENT_MAX_SUBSCRIBERS; i++) {
        if (g_subs[type][i].active && g_subs[type][i].handler == handler &&
            g_subs[type][i].userdata == userdata) {
            pthread_mutex_unlock(&g_lock);
            LOG_INFO_T("EventBus", "Subscribe", "Exists", "handler already bound to '%s'",
                       event_type_name(type));
            return 0;
        }
        if (!g_subs[type][i].active && free_slot < 0) free_slot = i;
    }
    if (free_slot < 0) {
        pthread_mutex_unlock(&g_lock);
        LOG_WARN_T("EventBus", "Subscribe", "Full", "'%s' subscriber table full (%d)",
                   event_type_name(type), EVENT_MAX_SUBSCRIBERS);
        return -1;
    }
    g_subs[type][free_slot].handler = handler;
    g_subs[type][free_slot].userdata = userdata;
    g_subs[type][free_slot].active = 1;
    pthread_mutex_unlock(&g_lock);
    LOG_INFO_T("EventBus", "Subscribe", "OK", "subscribed to '%s'", event_type_name(type));
    return 0;
}

int event_bus_unsubscribe(event_type_t type, event_handler_t handler) {
    int i, found = 0;
    if (type <= EVT_NONE || type >= EVT_COUNT || !handler) return -1;
    pthread_mutex_lock(&g_lock);
    for (i = 0; i < EVENT_MAX_SUBSCRIBERS; i++) {
        if (g_subs[type][i].active && g_subs[type][i].handler == handler) {
            g_subs[type][i].active = 0;
            g_subs[type][i].handler = NULL;
            g_subs[type][i].userdata = NULL;
            found++;
        }
    }
    pthread_mutex_unlock(&g_lock);
    return found > 0 ? 0 : -1;
}

/* ============================================================
 * 发布
 * ============================================================ */
int event_publish(event_type_t type, const char *payload_json) {
    event_sub_t snapshot[EVENT_MAX_SUBSCRIBERS];
    int n = 0;
    int i;

    if (type <= EVT_NONE || type >= EVT_COUNT) return -1;
    if (!g_initialized) event_bus_init();
    if (t_publish_depth >= EVENT_PUBLISH_MAX_DEPTH) {
        g_dropped++;
        LOG_WARN_T("EventBus", "Publish", "DepthLimit", "'%s' dropped (depth %d)",
                   event_type_name(type), t_publish_depth);
        return -1;
    }

    pthread_mutex_lock(&g_lock);
    g_published[type]++;
    for (i = 0; i < EVENT_MAX_SUBSCRIBERS; i++) {
        if (g_subs[type][i].active) snapshot[n++] = g_subs[type][i];
    }
    g_recent[g_recent_head].type = type;
    g_recent[g_recent_head].at = time(NULL);
    safe_strncpy(g_recent[g_recent_head].payload, payload_json ? payload_json : "",
                 sizeof(g_recent[0].payload));
    g_recent_head = (g_recent_head + 1) % EVENT_RECENT_MAX;
    pthread_mutex_unlock(&g_lock);

    t_publish_depth++;
    for (i = 0; i < n; i++) {
        if (snapshot[i].handler) {
            snapshot[i].handler(type, payload_json, snapshot[i].userdata);
            g_delivered++;
        }
    }
    t_publish_depth--;

    LOG_DEBUG_T("EventBus", "Publish", "OK", "'%s' → %d subscriber(s)", event_type_name(type), n);
    return n;
}

int event_publish_fmt(event_type_t type, const char *fmt, ...) {
    char buf[EVENT_PAYLOAD_MAX];
    va_list ap;
    if (!fmt) return event_publish(type, NULL);
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    return event_publish(type, buf);
}

/* ============================================================
 * 观测
 * ============================================================ */
const char* event_type_name(event_type_t type) {
    if (type < 0 || type >= EVT_COUNT) return "unknown";
    return EVENT_NAMES[type];
}

int event_type_from_name(const char *name) {
    int i;
    if (!name) return -1;
    for (i = 1; i < EVT_COUNT; i++) {
        if (strcmp(EVENT_NAMES[i], name) == 0) return i;
    }
    return -1;
}

int event_bus_subscriber_count(event_type_t type) {
    int i, n = 0;
    if (type <= EVT_NONE || type >= EVT_COUNT) return 0;
    pthread_mutex_lock(&g_lock);
    for (i = 0; i < EVENT_MAX_SUBSCRIBERS; i++) {
        if (g_subs[type][i].active) n++;
    }
    pthread_mutex_unlock(&g_lock);
    return n;
}

unsigned long event_bus_published_count(event_type_t type) {
    if (type <= EVT_NONE || type >= EVT_COUNT) return 0;
    return g_published[type];
}

unsigned long event_bus_delivered_count(void) { return g_delivered; }
unsigned long event_bus_dropped_count(void) { return g_dropped; }

cJSON* event_bus_stats(void) {
    cJSON *o = cJSON_CreateObject();
    cJSON *types;
    int i;
    if (!o) return NULL;
    types = cJSON_CreateArray();
    if (!types) { cJSON_Delete(o); return NULL; }
    for (i = 1; i < EVT_COUNT; i++) {
        cJSON *t = cJSON_CreateObject();
        if (!t) continue;
        cJSON_AddStringToObject(t, "event", EVENT_NAMES[i]);
        cJSON_AddNumberToObject(t, "published", (double)g_published[i]);
        cJSON_AddNumberToObject(t, "subscribers", (double)event_bus_subscriber_count((event_type_t)i));
        cJSON_AddItemToArray(types, t);
    }
    cJSON_AddNumberToObject(o, "types", (double)(EVT_COUNT - 1));
    cJSON_AddNumberToObject(o, "delivered", (double)g_delivered);
    cJSON_AddNumberToObject(o, "dropped", (double)g_dropped);
    cJSON_AddItemToObject(o, "events", types);
    return o;
}

cJSON* event_bus_recent(int limit) {
    cJSON *arr = cJSON_CreateArray();
    int i, count;
    if (!arr) return NULL;
    if (limit <= 0 || limit > EVENT_RECENT_MAX) limit = EVENT_RECENT_MAX;
    count = limit;
    pthread_mutex_lock(&g_lock);
    for (i = 0; i < count; i++) {
        int idx = (g_recent_head - 1 - i + EVENT_RECENT_MAX * 2) % EVENT_RECENT_MAX;
        cJSON *e;
        if (g_recent[idx].type == EVT_NONE) continue;
        e = cJSON_CreateObject();
        if (!e) continue;
        cJSON_AddStringToObject(e, "event", event_type_name(g_recent[idx].type));
        cJSON_AddNumberToObject(e, "at", (double)g_recent[idx].at);
        cJSON_AddStringToObject(e, "payload", g_recent[idx].payload);
        cJSON_AddItemToArray(arr, e);
    }
    pthread_mutex_unlock(&g_lock);
    return arr;
}
