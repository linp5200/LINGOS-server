/**
 * @file    execution_gate.c
 * @brief   执行门核心实现
 * @version LN-B-5.0.0.0
 */

#include "execution_gate.h"
#include "security_config.h"
#include "defense_mode.h"
#include "../common/safe_string.h"
#include "../lib/log_extra.h"
#include "../security/audit.h"
#include <string.h>
#include <pthread.h>
#include <ctype.h>

#define MAX_WHITELIST 64

static char g_whitelist[MAX_WHITELIST][128];
static int g_whitelist_count = 0;
static char g_mode[16] = "balanced";
static pthread_mutex_t g_gate_lock = PTHREAD_MUTEX_INITIALIZER;

const char* execution_gate_source_name(gate_source_t source) {
    switch (source) {
        case GATE_SOURCE_SHELL:          return "shell";
        case GATE_SOURCE_AI_PROMPT:      return "ai_prompt";
        case GATE_SOURCE_AI_TOOL:        return "ai_tool";
        case GATE_SOURCE_CONFIG_WIZARD:  return "config_wizard";
        case GATE_SOURCE_TUI_TERMINAL:   return "tui_terminal";
        case GATE_SOURCE_RULE_ENGINE:    return "rule_engine";
        case GATE_SOURCE_HOST_CMD:       return "host_cmd";
        case GATE_SOURCE_API_REQUEST:    return "api_request";
        case GATE_SOURCE_MQTT:           return "mqtt";
        case GATE_SOURCE_FILE:           return "file";
        case GATE_SOURCE_PLUGIN:         return "plugin";
        default:                         return "unknown";
    }
}

/* ============================================================
 * 危险模式检查
 * ============================================================ */

static int contains_dangerous_pattern(const char *input) {
    if (!input) return 0;

    const char *dangerous[] = {
        "rm -rf /",
        "rm -rf /*",
        "mkfs",
        "dd if=",
        "> /dev/sd",
        "shutdown",
        "reboot",
        "poweroff",
        "systemctl stop",
        "kill -9",
        "chmod 777",
        "chown root",
        "passwd",
        "sudo",
        "su",
        "eval(",
        "exec(",
        "system(",
        "popen(",
        "; rm ",
        "| sh",
        "`",
        "$(",
        NULL
    };

    /* 如果输入与任一危险模式匹配，返回 1 */
    for (int i = 0; dangerous[i]; i++) {
        if (strstr(input, dangerous[i]) != NULL) {
            return 1;
        }
    }
    return 0;
}

/* ============================================================
 * 白名单匹配
 * ============================================================ */

static int is_whitelisted(const char *input) {
    if (!input) return 0;

    pthread_mutex_lock(&g_gate_lock);

    for (int i = 0; i < g_whitelist_count; i++) {
        if (strstr(input, g_whitelist[i]) != NULL) {
            pthread_mutex_unlock(&g_gate_lock);
            return 1;
        }
    }

    pthread_mutex_unlock(&g_gate_lock);
    return 0;
}

/* ============================================================
 * 核心检查函数
 * ============================================================ */

gate_result_t execution_gate_check(gate_source_t source,
                                   const char *input,
                                   const char *context,
                                   char *reason,
                                   size_t reason_len) {
    if (!input) {
        if (reason) safe_snprintf(reason, reason_len, "Empty input");
        return GATE_RESULT_DENY;
    }

    /* 绝对保护模式：禁止所有外部输入（除系统豁免） */
    if (defense_mode_get() == DEFENSE_MODE_ABSOLUTE) {
        if (source != GATE_SOURCE_SHELL || (
            strstr(input, "exit") == NULL &&
            strstr(input, "help") == NULL &&
            strstr(input, "logdump") == NULL &&
            strstr(input, "system security") == NULL &&
            strstr(input, "system privilege") == NULL)) {
            if (reason) safe_snprintf(reason, reason_len, "Absolute protect mode: input blocked");
            return GATE_RESULT_DENY;
        }
    }

    const security_config_t *cfg = security_config_get();
    const char *mode = cfg ? cfg->input_mode : "balanced";

    /* 宽松模式：只记录日志，全部放行 */
    if (strcmp(mode, "permissive") == 0) {
        LOG_DEBUG_T("ExecutionGate", "Check", "Permissive", "allowing input from %s", execution_gate_source_name(source));
        return GATE_RESULT_ALLOW;
    }

    /* 白名单检查 */
    if (is_whitelisted(input)) {
        LOG_DEBUG_T("ExecutionGate", "Check", "Whitelisted", "input matched whitelist");
        return GATE_RESULT_ALLOW;
    }

    /* 严格模式：默认拒绝 */
    if (strcmp(mode, "strict") == 0) {
        if (reason) safe_snprintf(reason, reason_len, "Strict mode: default deny");
        LOG_WARN_T("ExecutionGate", "Check", "StrictDeny", "blocked input from %s: %.50s",
                   execution_gate_source_name(source), input);
        return GATE_RESULT_DENY;
    }

    /* 平衡模式：阻止危险模式 */
    if (contains_dangerous_pattern(input)) {
        if (reason) safe_snprintf(reason, reason_len, "Dangerous pattern detected");
        LOG_WARN_T("ExecutionGate", "Check", "Dangerous", "blocked input from %s: %.50s",
                   execution_gate_source_name(source), input);
        return GATE_RESULT_DENY;
    }

    LOG_DEBUG_T("ExecutionGate", "Check", "Allow", "allowing input from %s", execution_gate_source_name(source));
    return GATE_RESULT_ALLOW;
}

/* ============================================================
 * 模式管理
 * ============================================================ */

int execution_gate_set_mode(const char *mode) {
    if (!mode) return -1;
    if (strcmp(mode, "strict") != 0 &&
        strcmp(mode, "balanced") != 0 &&
        strcmp(mode, "permissive") != 0) {
        return -1;
    }

    pthread_mutex_lock(&g_gate_lock);
    safe_strncpy(g_mode, mode, sizeof(g_mode));
    pthread_mutex_unlock(&g_gate_lock);

    LOG_INFO_T("ExecutionGate", "SetMode", "OK", "mode set to %s", mode);
    return 0;
}

const char* execution_gate_get_mode(void) {
    return g_mode;
}

/* ============================================================
 * 白名单管理
 * ============================================================ */

int execution_gate_whitelist_add(const char *pattern) {
    if (!pattern) return -1;

    pthread_mutex_lock(&g_gate_lock);

    if (g_whitelist_count >= MAX_WHITELIST) {
        pthread_mutex_unlock(&g_gate_lock);
        return -1;
    }

    safe_strncpy(g_whitelist[g_whitelist_count], pattern, sizeof(g_whitelist[0]));
    g_whitelist_count++;

    pthread_mutex_unlock(&g_gate_lock);
    LOG_INFO_T("ExecutionGate", "WhitelistAdd", "OK", "added '%s'", pattern);
    return 0;
}

int execution_gate_whitelist_remove(const char *pattern) {
    if (!pattern) return -1;

    pthread_mutex_lock(&g_gate_lock);

    for (int i = 0; i < g_whitelist_count; i++) {
        if (strcmp(g_whitelist[i], pattern) == 0) {
            for (int j = i; j < g_whitelist_count - 1; j++) {
                safe_strncpy(g_whitelist[j], g_whitelist[j+1], sizeof(g_whitelist[0]));
            }
            g_whitelist_count--;
            pthread_mutex_unlock(&g_gate_lock);
            LOG_INFO_T("ExecutionGate", "WhitelistRemove", "OK", "removed '%s'", pattern);
            return 0;
        }
    }

    pthread_mutex_unlock(&g_gate_lock);
    return -1;
}

int execution_gate_whitelist_list(char *out, size_t out_len) {
    if (!out || out_len == 0) return -1;

    pthread_mutex_lock(&g_gate_lock);

    size_t pos = 0;
    for (int i = 0; i < g_whitelist_count && pos < out_len - 1; i++) {
        pos += safe_snprintf(out + pos, out_len - pos, "%s\n", g_whitelist[i]);
    }

    pthread_mutex_unlock(&g_gate_lock);
    return 0;
}