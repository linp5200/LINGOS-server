/**
 * @file    alert_notify.c
 * @brief   预警通知分发（Shell/TUI/MQTT）
 * @version LN-B-5.0.0.0
 * @par     核心协议：容错编程（推送失败不影响主流程）
 * @changes MQTT 状态检查（mqtt_client_is_connected 前置）
 */

#include "alert_notify.h"
#include "alert_config.h"
#include "../common/safe_string.h"
#include "../common/lang.h"
#include "../drivers/uart.h"
#include "../lib/log_extra.h"
#include "../net/mqtt/mqtt_client.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static alert_config_t g_notify_config;
static int g_mqtt_available = 0;

/* ============================================================
 * 通知方式枚举
 * ============================================================ */

typedef enum {
    NOTIFY_SHELL = 1 << 0,
    NOTIFY_TUI = 1 << 1,
    NOTIFY_MQTT = 1 << 2
} notify_method_t;

/* ============================================================
 * 初始化
 * ============================================================ */

void alert_notify_init(const alert_config_t *config) {
    if (config) {
        g_notify_config = *config;
    }

    /* 【修改】MQTT 状态检查 */
    if (mqtt_client_is_connected()) {
        g_mqtt_available = 1;
        LOG_DEBUG_T("AlertNotify", "Init", "MQTT", "MQTT available");
    } else {
        g_mqtt_available = 0;
        LOG_DEBUG_T("AlertNotify", "Init", "MQTT", "MQTT not available");
    }
}

/* ============================================================
 * Shell 通知（横幅）
 * ============================================================ */

static void notify_shell(const alert_event_t *event) {
    if (!event) return;

    const char *level_colors[] = {
        "\033[37m",
        "\033[34m",
        "\033[33m",
        "\033[38;5;214m",
        "\033[31m",
        "\033[35m"
    };
    const char *type_names[] = {
        tr("Unknown", "未知"),
        tr("Typhoon", "台风"),
        tr("Earthquake", "地震"),
        tr("Rain", "暴雨"),
        tr("High Temp", "高温"),
        tr("Storm", "风暴"),
        tr("Fire", "火灾")
    };

    const char *color = (event->level >= 0 && event->level <= 5) ? level_colors[event->level] : "\033[37m";

    uart_puts("\n");
    uart_puts(color);
    uart_puts("╔══════════════════════════════════════════════════════════════════╗\n");
    uart_puts("║  ⚠️  ");
    uart_puts(tr("ALERT", "预警"));
    uart_puts(": ");
    uart_puts(type_names[event->type]);
    uart_puts(" (");
    uart_puts(tr("Level", "等级"));
    uart_puts(" ");
    char level_str[8];
    safe_snprintf(level_str, sizeof(level_str), "%d", event->level);
    uart_puts(level_str);
    uart_puts(")  ║\n");
    uart_puts("║  ");
    uart_puts(event->description);
    int len = strlen(event->description);
    for (int i = len; i < 58; i++) uart_puts(" ");
    uart_puts(" ║\n");
    if (event->distance_km >= 0) {
        char dist_str[32];
        safe_snprintf(dist_str, sizeof(dist_str), "║  %s: %d km",
                      tr("Distance", "距离"), event->distance_km);
        uart_puts(dist_str);
        int dlen = strlen(dist_str);
        for (int i = dlen; i < 58; i++) uart_puts(" ");
        uart_puts(" ║\n");
    }
    uart_puts("╚══════════════════════════════════════════════════════════════════╝\n");
    uart_puts("\033[0m");
}

/* ============================================================
 * TUI 通知（弹窗）
 * ============================================================ */

static void notify_tui(const alert_event_t *event) {
    LOG_INFO_T("AlertNotify", "TUI", "Alert", "type=%d, level=%d, desc=%s",
               event->type, event->level, event->description);
}

/* ============================================================
 * 【修改】MQTT 推送（含状态检查）
 * ============================================================ */

static void notify_mqtt(const alert_event_t *event) {
    /* MQTT 状态检查 */
    if (!mqtt_client_is_connected()) {
        g_mqtt_available = 0;
        LOG_DEBUG_T("AlertNotify", "MQTT", "Skip", "MQTT not connected");
        return;
    }

    if (!g_mqtt_available) {
        LOG_DEBUG_T("AlertNotify", "MQTT", "Skip", "MQTT not available");
        return;
    }

    char json[512];
    safe_snprintf(json, sizeof(json),
                  "{\"type\":%d,\"level\":%d,\"desc\":\"%s\",\"source\":\"%s\",\"timestamp\":%ld}",
                  event->type, event->level, event->description, event->source, (long)event->timestamp);

    int ret = mqtt_client_publish("lingos/alert", json, strlen(json), 1, 0);
    if (ret == 0) {
        LOG_DEBUG_T("AlertNotify", "MQTT", "Sent", "alert published");
    } else {
        LOG_WARN_T("AlertNotify", "MQTT", "Fail", "publish failed, ret=%d", ret);
    }
}

/* ============================================================
 * 主发送函数
 * ============================================================ */

void alert_notify_send(const alert_event_t *event, const alert_config_t *config) {
    if (!event || !config) return;

    int forced = (event->level >= 3);

    /* Shell 通知（始终启用） */
    notify_shell(event);

    /* TUI 通知 */
    notify_tui(event);

    /* 【修改】MQTT 通知（含状态检查） */
    if (config->mqtt_enabled && mqtt_client_is_connected()) {
        notify_mqtt(event);
    }

    if (config->sound_enabled || forced) {
        uart_puts("\a");
    }
}

/* ============================================================
 * 强制发送（不受配置限制）
 * ============================================================ */

void alert_notify_force_send(const alert_event_t *event) {
    if (!event) return;
    notify_shell(event);
    notify_tui(event);
    notify_mqtt(event);
    uart_puts("\a");
    LOG_INFO_T("AlertNotify", "ForceSend", "OK", "forced alert sent");
}

/* ============================================================
 * 清理
 * ============================================================ */

void alert_notify_cleanup(void) {
    g_mqtt_available = 0;
    LOG_DEBUG_T("AlertNotify", "Cleanup", "OK", "notification system cleaned up");
}