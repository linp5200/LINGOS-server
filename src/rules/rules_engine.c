/**
 * @file    src/rules/rules_engine.c
 * @brief   规则引擎核心（IF-THEN 因果模型）
 * @version LN-B-4.3.0.0
 * @changes 新增配置向导步骤注册
 * @par     核心协议：防弹编程（规则执行失败不影响主流程）
 */

#include "rules_engine.h"
#include "rules_parser.h"
#include "rules_executor.h"
#include "rules_storage.h"
#include "rules_ai_guard.h"
#include "../common/error_report.h"
#include "../common/safe_string.h"
#include "../lib/log_extra.h"
#include "../common/data_path.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <pthread.h>

#define MAX_RULES 64
#define RULES_CONFIG_PATH "/system/config/rules.conf"

static rule_t g_rules[MAX_RULES];
static int g_rule_count = 0;
static int g_engine_running = 0;
static pthread_mutex_t g_engine_lock = PTHREAD_MUTEX_INITIALIZER;
static rule_config_t g_config;

/* ============================================================
 * 配置加载
 * ============================================================ */

int rules_config_load(void) {
    LOG_DEBUG_T("RulesEngine", "ConfigLoad", "Enter", "loading config");

    const char *root = lingos_data_root();
    char path[512];
    safe_snprintf(path, sizeof(path), "%s%s", root, RULES_CONFIG_PATH);

    FILE *fp = fopen(path, "r");
    if (!fp) {
        LOG_DEBUG_T("RulesEngine", "ConfigLoad", "NotFound", "using defaults");
        g_config.enabled = 1;
        g_config.check_interval = 60;
        g_config.max_actions_per_rule = 5;
        g_config.require_ai_guard = 1;
        return 0;
    }

    char line[256];
    while (fgets(line, sizeof(line), fp)) {
        if (line[0] == '#' || line[0] == '\n') continue;
        char key[64], val[64];
        if (sscanf(line, "%63[^=]=%63s", key, val) == 2) {
            if (strcmp(key, "enabled") == 0) g_config.enabled = atoi(val);
            else if (strcmp(key, "check_interval") == 0) g_config.check_interval = atoi(val);
            else if (strcmp(key, "max_actions_per_rule") == 0) g_config.max_actions_per_rule = atoi(val);
            else if (strcmp(key, "require_ai_guard") == 0) g_config.require_ai_guard = atoi(val);
        }
    }
    fclose(fp);

    LOG_INFO_T("RulesEngine", "ConfigLoad", "OK", "config loaded: interval=%d, ai_guard=%d",
               g_config.check_interval, g_config.require_ai_guard);
    return 0;
}

/* ============================================================
 * 初始化规则引擎
 * ============================================================ */

int rules_engine_init(void) {
    LOG_INFO_T("RulesEngine", "Init", "Enter", "initializing rules engine");

    pthread_mutex_lock(&g_engine_lock);

    memset(g_rules, 0, sizeof(g_rules));
    g_rule_count = 0;

    rules_config_load();

    int count = rules_storage_load(g_rules, MAX_RULES);
    if (count > 0) {
        g_rule_count = count;
        LOG_INFO_T("RulesEngine", "Init", "Loaded", "loaded %d rules from storage", count);
    }

    pthread_mutex_unlock(&g_engine_lock);

    /* ====== 新增：注册规则引擎配置步骤 ====== */
    /* 注意：注册函数需要 wizard_state_t 参数，由主进程调用时传入 */
    LOG_DEBUG_T("RulesEngine", "Init", "ConfigStep", "rules config step available");

    LOG_INFO_T("RulesEngine", "Init", "OK", "rules engine initialized");
    return 0;
}
/* ============================================================
 * 添加规则（含AI守卫检查）
 * ============================================================ */

int rules_engine_add(const rule_t *rule) {
    LOG_INFO_T("RulesEngine", "Add", "Enter", "name=%s", rule ? rule->name : "(null)");

    if (!rule) {
        LOG_ERROR_T("RulesEngine", "Add", "Invalid", "rule is NULL");
        return -1;
    }

    /* AI守卫检查：检测逻辑性误差或递进问题 */
    if (g_config.require_ai_guard) {
        char guard_result[256];
        if (rules_ai_guard_check(rule, guard_result, sizeof(guard_result)) != 0) {
            LOG_WARN_T("RulesEngine", "Add", "AIGuardFail", "AI guard rejected: %s", guard_result);
            return -1;
        }
    }

    pthread_mutex_lock(&g_engine_lock);

    if (g_rule_count >= MAX_RULES) {
        pthread_mutex_unlock(&g_engine_lock);
        LOG_ERROR_T("RulesEngine", "Add", "Overflow", "max rules reached (%d)", MAX_RULES);
        return -1;
    }

    /* 检查是否已存在同名规则 */
    for (int i = 0; i < g_rule_count; i++) {
        if (strcmp(g_rules[i].name, rule->name) == 0) {
            /* 更新现有规则 */
            g_rules[i] = *rule;
            pthread_mutex_unlock(&g_engine_lock);
            rules_storage_save(g_rules, g_rule_count);
            LOG_INFO_T("RulesEngine", "Add", "Updated", "rule '%s' updated", rule->name);
            return 0;
        }
    }

    g_rules[g_rule_count++] = *rule;
    pthread_mutex_unlock(&g_engine_lock);

    rules_storage_save(g_rules, g_rule_count);

    LOG_INFO_T("RulesEngine", "Add", "OK", "rule '%s' added (total=%d)", rule->name, g_rule_count);
    return 0;
}

/* ============================================================
 * 删除规则
 * ============================================================ */

int rules_engine_remove(const char *name) {
    LOG_INFO_T("RulesEngine", "Remove", "Enter", "name=%s", name ? name : "(null)");

    if (!name || !*name) {
        LOG_ERROR_T("RulesEngine", "Remove", "Invalid", "name is NULL or empty");
        return -1;
    }

    pthread_mutex_lock(&g_engine_lock);

    int found = 0;
    for (int i = 0; i < g_rule_count; i++) {
        if (strcmp(g_rules[i].name, name) == 0) {
            found = 1;
            for (int j = i; j < g_rule_count - 1; j++) {
                g_rules[j] = g_rules[j + 1];
            }
            g_rule_count--;
            break;
        }
    }

    pthread_mutex_unlock(&g_engine_lock);

    if (found) {
        rules_storage_save(g_rules, g_rule_count);
        LOG_INFO_T("RulesEngine", "Remove", "OK", "rule '%s' removed", name);
        return 0;
    }

    LOG_WARN_T("RulesEngine", "Remove", "NotFound", "rule '%s' not found", name);
    return -1;
}

/* ============================================================
 * 获取规则列表
 * ============================================================ */

int rules_engine_list(rule_t *out, int max_count) {
    LOG_DEBUG_T("RulesEngine", "List", "Enter", "max_count=%d", max_count);

    if (!out || max_count <= 0) return 0;

    pthread_mutex_lock(&g_engine_lock);

    int count = g_rule_count < max_count ? g_rule_count : max_count;
    for (int i = 0; i < count; i++) {
        out[i] = g_rules[i];
    }

    pthread_mutex_unlock(&g_engine_lock);

    LOG_DEBUG_T("RulesEngine", "List", "OK", "returned %d rules", count);
    return count;
}

/* ============================================================
 * 检查并执行规则（主循环）
 * ============================================================ */

int rules_engine_check_and_execute(void) {
    LOG_DEBUG_T("RulesEngine", "CheckExec", "Enter", "checking %d rules", g_rule_count);

    if (!g_config.enabled) {
        LOG_DEBUG_T("RulesEngine", "CheckExec", "Disabled", "rules engine disabled");
        return 0;
    }

    pthread_mutex_lock(&g_engine_lock);

    int executed = 0;
    for (int i = 0; i < g_rule_count; i++) {
        rule_t *rule = &g_rules[i];

        if (!rule->enabled) continue;

        /* 检查条件是否满足 */
        int condition_met = 0;
        if (rules_parser_evaluate(rule->condition, &condition_met) != 0) {
            LOG_WARN_T("RulesEngine", "CheckExec", "EvalFail", "rule '%s' condition eval failed", rule->name);
            continue;
        }

        if (condition_met) {
            LOG_INFO_T("RulesEngine", "CheckExec", "Triggered", "rule '%s' triggered", rule->name);
            /* 执行动作 */
            int ret = rules_executor_run(rule->actions, rule->action_count);
            if (ret == 0) {
                executed++;
                rule->last_triggered = time(NULL);
                rule->trigger_count++;
                LOG_DEBUG_T("RulesEngine", "CheckExec", "ActionOK", "rule '%s' executed", rule->name);
            } else {
                LOG_WARN_T("RulesEngine", "CheckExec", "ActionFail", "rule '%s' action failed", rule->name);
            }
        }
    }

    pthread_mutex_unlock(&g_engine_lock);

    if (executed > 0) {
        rules_storage_save(g_rules, g_rule_count);
    }

    LOG_DEBUG_T("RulesEngine", "CheckExec", "Done", "executed %d rules", executed);
    return executed;
}

/* ============================================================
 * 获取配置
 * ============================================================ */

const rule_config_t* rules_engine_get_config(void) {
    return &g_config;
}