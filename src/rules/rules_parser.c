/**
 * @file    rules_parser.c
 * @brief   规则解析（下拉框 → 内部表达式）
 * @version LN-B-4.3.0.0
 * @par     核心协议：防御性编程（表达式解析容错）
 */

#include "rules_parser.h"
#include "../common/error_report.h"
#include "../lib/log_extra.h"
#include "../common/safe_string.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <math.h>

/* ============================================================
 * 内置条件类型
 * ============================================================ */

typedef struct {
    const char *name;
    const char *pattern;
} condition_template_t;

static const condition_template_t g_condition_templates[] = {
    {"台风橙色预警", "alert.type == typhoon && alert.level >= 3"},
    {"台风红色预警", "alert.type == typhoon && alert.level >= 4"},
    {"有感地震", "alert.type == earthquake && alert.felt == 1"},
    {"地震震级 >= 5", "alert.type == earthquake && alert.magnitude >= 5.0"},
    {"暴雨橙色预警", "alert.type == rain && alert.level >= 3"},
    {"内存 > 90%", "system.memory > 90"},
    {"磁盘 > 85%", "system.disk > 85"},
    {"CPU负载 > 2.0", "system.load_avg > 2.0"},
    {NULL, NULL}
};

/* ============================================================
 * 内置动作类型
 * ============================================================ */

static const char *g_action_templates[] = {
    "notify_user",
    "mqtt_publish",
    "execute_script",
    "restart_service",
    "send_alert",
    "log_event",
    NULL
};

/* ============================================================
 * 从模板生成条件
 * ============================================================ */

int rules_parser_generate_condition(const char *template_name, char *out, size_t out_len) {
    LOG_DEBUG_T("RulesParser", "GenCondition", "Enter", "template=%s", template_name ? template_name : "(null)");

    if (!out || out_len == 0) return -1;

    for (int i = 0; g_condition_templates[i].name; i++) {
        if (strcmp(g_condition_templates[i].name, template_name) == 0) {
            safe_strncpy(out, g_condition_templates[i].pattern, out_len);
            LOG_DEBUG_T("RulesParser", "GenCondition", "OK", "generated: %s", out);
            return 0;
        }
    }

    safe_strncpy(out, "", out_len);
    LOG_WARN_T("RulesParser", "GenCondition", "NotFound", "template '%s' not found", template_name);
    return -1;
}

/* ============================================================
 * 生成动作列表
 * ============================================================ */

int rules_parser_generate_actions(const char **action_names, int count, char *out, size_t out_len) {
    LOG_DEBUG_T("RulesParser", "GenActions", "Enter", "count=%d", count);

    if (!out || out_len == 0) return -1;

    out[0] = '\0';
    size_t pos = 0;

    for (int i = 0; i < count && pos < out_len - 1; i++) {
        if (i > 0) {
            pos += safe_snprintf(out + pos, out_len - pos, ",");
        }
        pos += safe_snprintf(out + pos, out_len - pos, "%s", action_names[i]);
    }

    LOG_DEBUG_T("RulesParser", "GenActions", "OK", "generated: %s", out);
    return 0;
}

/* ============================================================
 * 评估条件表达式
 * ============================================================ */

int rules_parser_evaluate(const char *condition, int *result) {
    LOG_DEBUG_T("RulesParser", "Evaluate", "Enter", "condition=%s", condition ? condition : "(null)");

    if (!condition || !result) {
        LOG_ERROR_T("RulesParser", "Evaluate", "Invalid", "condition or result is NULL");
        return -1;
    }

    if (strlen(condition) == 0) {
        *result = 0;
        return 0;
    }

    /* 简化模拟评估 */
    /* 实际应调用 AI 或系统状态检查 */
    /* 此处模拟：包含 "true" 或 "> 0" 等 */

    if (strstr(condition, "true") || strstr(condition, "True") ||
        strstr(condition, "> 0") || strstr(condition, ">= 1")) {
        *result = 1;
    } else if (strstr(condition, "false") || strstr(condition, "False") ||
               strstr(condition, "== 0") || strstr(condition, "< 1")) {
        *result = 0;
    } else {
        /* 简单逻辑：检查是否包含数字 */
        int has_number = 0;
        for (const char *p = condition; *p; p++) {
            if (*p >= '0' && *p <= '9') { has_number = 1; break; }
        }
        *result = has_number ? 1 : 0;
    }

    LOG_DEBUG_T("RulesParser", "Evaluate", "Result", "condition=%s -> %d", condition, *result);
    return 0;
}

/* ============================================================
 * 获取条件模板列表
 * ============================================================ */

const char** rules_parser_get_condition_templates(void) {
    static const char *names[16];
    int i = 0;
    while (g_condition_templates[i].name && i < 15) {
        names[i] = g_condition_templates[i].name;
        i++;
    }
    names[i] = NULL;
    return names;
}

/* ============================================================
 * 获取动作模板列表
 * ============================================================ */

const char** rules_parser_get_action_templates(void) {
    return g_action_templates;
}