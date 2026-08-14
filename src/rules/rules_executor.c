/**
 * @file    rules_executor.c
 * @brief   规则执行器（动作触发）
 * @version LN-B-4.3.0.0
 * @par     核心协议：容错编程（动作失败不影响其他动作）
 */

#include "rules_executor.h"
#include "../common/error_report.h"
#include "../common/safe_string.h"
#include "../lib/log_extra.h"
#include "../drivers/uart.h"
#include "../alert/alert_notify.h"
#include "../net/mqtt/mqtt_client.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* ============================================================
 * 执行单个动作
 * ============================================================ */

static int execute_action(const char *action) {
    LOG_DEBUG_T("RulesExecutor", "ExecAction", "Enter", "action=%s", action ? action : "(null)");

    if (!action || !*action) {
        LOG_WARN_T("RulesExecutor", "ExecAction", "Empty", "action is empty");
        return -1;
    }

    /* 解析动作 */
    char action_type[64];
    char action_param[256];
    action_type[0] = '\0';
    action_param[0] = '\0';

    const char *p = action;
    int i = 0;
    while (*p && *p != ':' && i < 63) {
        action_type[i++] = *p++;
    }
    action_type[i] = '\0';

    if (*p == ':') {
        p++;
        i = 0;
        while (*p && i < 255) {
            action_param[i++] = *p++;
        }
        action_param[i] = '\0';
    }

    /* 执行动作 */
    if (strcmp(action_type, "notify_user") == 0) {
        uart_puts(COLOR_YELLOW);
        uart_puts("\n[RULE] ");
        uart_puts(action_param[0] ? action_param : "规则触发");
        uart_puts("\n");
        uart_puts(COLOR_RESET);
        LOG_INFO_T("RulesExecutor", "Action", "Notify", "user notified: %s", action_param);
        return 0;

    } else if (strcmp(action_type, "mqtt_publish") == 0) {
        if (mqtt_client_is_connected()) {
            char topic[128];
            safe_snprintf(topic, sizeof(topic), "lingos/rules/%s", action_param);
            mqtt_client_publish(topic, "triggered", 9, 1, 0);
            LOG_INFO_T("RulesExecutor", "Action", "MQTT", "published to %s", topic);
            return 0;
        } else {
            LOG_WARN_T("RulesExecutor", "Action", "MQTTFail", "MQTT not connected");
            return -1;
        }

    } else if (strcmp(action_type, "execute_script") == 0) {
        if (action_param[0]) {
            char cmd[512];
            safe_snprintf(cmd, sizeof(cmd), "%s &", action_param);
            int ret = system(cmd);
            LOG_INFO_T("RulesExecutor", "Action", "Script", "executed: %s (ret=%d)", action_param, ret);
            return ret == 0 ? 0 : -1;
        }
        return -1;

    } else if (strcmp(action_type, "restart_service") == 0) {
        if (action_param[0]) {
            char cmd[512];
            safe_snprintf(cmd, sizeof(cmd), "systemctl restart %s &", action_param);
            int ret = system(cmd);
            LOG_INFO_T("RulesExecutor", "Action", "Service", "restarted: %s (ret=%d)", action_param, ret);
            return ret == 0 ? 0 : -1;
        }
        return -1;

    } else if (strcmp(action_type, "log_event") == 0) {
        LOG_INFO_T("RulesExecutor", "Action", "LogEvent", "event: %s", action_param[0] ? action_param : "rule_triggered");
        return 0;

    } else if (strcmp(action_type, "send_alert") == 0) {
        /* 重用预警通知 */
        alert_event_t ev;
        memset(&ev, 0, sizeof(ev));
        ev.type = ALERT_TYPE_UNKNOWN;
        ev.level = 2;
        safe_strncpy(ev.description, action_param[0] ? action_param : "Rule triggered", sizeof(ev.description));
        safe_strncpy(ev.source, "rules_engine", sizeof(ev.source));
        ev.timestamp = time(NULL);
        alert_notify_force_send(&ev);
        LOG_INFO_T("RulesExecutor", "Action", "Alert", "alert sent");
        return 0;

    } else {
        LOG_WARN_T("RulesExecutor", "Action", "Unknown", "unknown action type: %s", action_type);
        return -1;
    }
}

/* ============================================================
 * 执行动作列表
 * ============================================================ */

int rules_executor_run(const char actions[RULE_MAX_ACTIONS][RULE_ACTION_MAX], int count) {
    LOG_INFO_T("RulesExecutor", "Run", "Enter", "count=%d", count);

    if (count <= 0) {
        LOG_WARN_T("RulesExecutor", "Run", "Empty", "no actions to execute");
        return -1;
    }

    int success_count = 0;
    for (int i = 0; i < count; i++) {
        if (actions[i][0] == '\0') continue;
        if (execute_action(actions[i]) == 0) {
            success_count++;
        }
    }

    LOG_INFO_T("RulesExecutor", "Run", "Done", "executed %d/%d actions", success_count, count);
    return (success_count > 0) ? 0 : -1;
}