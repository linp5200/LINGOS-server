/**
 * @file    rules_ai_guard.c
 * @brief   AI 守卫（检测逻辑性误差/递进问题）
 * @version LN-B-4.3.0.0
 * @par     核心协议：契约式编程（检测到危险则拒绝）
 */

#include "rules_ai_guard.h"
#include "../common/error_report.h"
#include "../common/safe_string.h"
#include "../lib/log_extra.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

/* ============================================================
 * 检测逻辑性误差
 * ============================================================ */

static int detect_logic_error(const rule_t *rule, char *reason, size_t reason_len) {
    /* 检测 "开机时关机" 类型的矛盾 */
    if (strstr(rule->condition, "startup") && strstr(rule->condition, "shutdown")) {
        safe_snprintf(reason, reason_len, "矛盾条件: 开机和关机同时出现");
        return -1;
    }

    if (strstr(rule->condition, "shutdown") && strstr(rule->condition, "reboot")) {
        safe_snprintf(reason, reason_len, "矛盾条件: 关机和重启同时出现");
        return -1;
    }

    /* 检测递进问题（无限循环） */
    if (strstr(rule->condition, "rule_triggered") || strstr(rule->condition, "self_trigger")) {
        safe_snprintf(reason, reason_len, "检测到可能的递归触发: 规则触发自身");
        return -1;
    }

    return 0;
}

/* ============================================================
 * 检测系统级危险操作
 * ============================================================ */

static int detect_system_danger(const rule_t *rule, char *reason, size_t reason_len) {
    for (int i = 0; i < rule->action_count; i++) {
        const char *action = rule->actions[i];

        /* 检测危险系统命令 */
        if (strstr(action, "rm -rf") || strstr(action, "mkfs") ||
            strstr(action, "dd if=") || strstr(action, "format")) {
            safe_snprintf(reason, reason_len, "危险操作: %s", action);
            return -1;
        }

        /* 检测破坏性服务操作 */
        if (strstr(action, "systemctl stop") && strstr(action, "critical")) {
            safe_snprintf(reason, reason_len, "禁止停止关键服务: %s", action);
            return -1;
        }
    }

    return 0;
}

/* ============================================================
 * AI 守卫主检查
 * ============================================================ */

int rules_ai_guard_check(const rule_t *rule, char *reason, size_t reason_len) {
    LOG_DEBUG_T("RulesAIGuard", "Check", "Enter", "rule=%s", rule ? rule->name : "(null)");

    if (!rule) {
        safe_snprintf(reason, reason_len, "规则为空");
        return -1;
    }

    if (reason) reason[0] = '\0';

    /* 检查逻辑性误差 */
    if (detect_logic_error(rule, reason, reason_len) != 0) {
        LOG_WARN_T("RulesAIGuard", "Check", "LogicError", "%s", reason);
        return -1;
    }

    /* 检查系统级危险操作 */
    if (detect_system_danger(rule, reason, reason_len) != 0) {
        LOG_WARN_T("RulesAIGuard", "Check", "SystemDanger", "%s", reason);
        return -1;
    }

    LOG_DEBUG_T("RulesAIGuard", "Check", "OK", "rule passed AI guard");
    return 0;
}