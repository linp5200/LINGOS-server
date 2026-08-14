/**
 * @file    rules_cmds.c
 * @brief   rule 子命令实现
 * @version LN-B-4.3.0.0
 * @par     核心协议：防御性编程
 */

#include "rules_cmds.h"
#include "../rules/rules_engine.h"
#include "../rules/rules_parser.h"
#include "../rules/rules_ai_guard.h"
#include "../common/lang.h"
#include "../drivers/uart.h"
#include "../lib/log_extra.h"
#include "../common/safe_string.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* ============================================================
 * 列出规则
 * ============================================================ */

static void cmd_rules_list(void) {
    rule_t rules[32];
    int count = rules_engine_list(rules, 32);

    uart_puts(tr("\n=== Rules ===\n", "\n=== 规则列表 ===\n"));

    if (count == 0) {
        uart_puts(tr("No rules defined.\n", "没有定义规则。\n"));
        uart_puts(tr("  rule add <name> - Create a new rule\n",
                     "  rule add <名称> - 创建新规则\n"));
        return;
    }

    for (int i = 0; i < count; i++) {
        rule_t *r = &rules[i];
        char time_buf[32];
        struct tm *tm = localtime(&r->created_at);
        strftime(time_buf, sizeof(time_buf), "%Y-%m-%d", tm);

        char buf[256];
        safe_snprintf(buf, sizeof(buf),
                      "  %d. %s [%s] %s\n"
                      "     IF: %s\n"
                      "     THEN: %d actions\n"
                      "     Triggered: %d times, Last: %s\n",
                      i + 1,
                      r->name,
                      r->enabled ? "ON" : "OFF",
                      r->is_custom ? "[CUSTOM]" : "[SIMPLE]",
                      r->condition,
                      r->action_count,
                      r->trigger_count,
                      time_buf);
        uart_puts(buf);
        uart_puts("\n");
    }
}

/* ============================================================
 * 添加规则（交互式）
 * ============================================================ */

static void cmd_rules_add(const char *name) {
    LOG_INFO_T("RulesCmd", "Add", "Enter", "name=%s", name ? name : "(null)");

    if (!name || !*name) {
        uart_puts(tr("Usage: rule add <name>\n", "用法：rule add <名称>\n"));
        return;
    }

    rule_t rule;
    memset(&rule, 0, sizeof(rule));
    safe_strncpy(rule.name, name, sizeof(rule.name));

    uart_puts(tr("\nSelect condition type:\n", "\n选择条件类型：\n"));
    const char **templates = rules_parser_get_condition_templates();
    for (int i = 0; templates[i]; i++) {
        char buf[64];
        safe_snprintf(buf, sizeof(buf), "  %d. %s\n", i + 1, templates[i]);
        uart_puts(buf);
    }
    uart_puts(tr("  c. Custom expression (developer)\n", "  c. 自定义表达式（开发者）\n"));
    uart_puts(tr("Enter choice: ", "输入选项："));
    fflush(stdout);

    char input[16];
    if (!fgets(input, sizeof(input), stdin)) return;
    input[strcspn(input, "\n")] = '\0';

    if (input[0] == 'c' || input[0] == 'C') {
        rule.is_custom = 1;
        uart_puts(tr("Enter condition expression: ", "输入条件表达式："));
        if (!fgets(rule.condition, sizeof(rule.condition), stdin)) return;
        rule.condition[strcspn(rule.condition, "\n")] = '\0';
    } else {
        int idx = atoi(input) - 1;
        if (templates[idx]) {
            rules_parser_generate_condition(templates[idx], rule.condition, sizeof(rule.condition));
        } else {
            uart_puts(tr("Invalid choice.\n", "无效选项。\n"));
            return;
        }
    }

    /* 选择动作 */
    uart_puts(tr("\nSelect actions (comma separated, e.g. 1,3):\n",
                 "\n选择动作（逗号分隔，如 1,3）：\n"));
    const char **actions = rules_parser_get_action_templates();
    for (int i = 0; actions[i]; i++) {
        char buf[64];
        safe_snprintf(buf, sizeof(buf), "  %d. %s\n", i + 1, actions[i]);
        uart_puts(buf);
    }
    uart_puts(tr("  c. Custom action\n", "  c. 自定义动作\n"));
    uart_puts(tr("Enter choices: ", "输入选择："));

    char action_input[256];
    if (!fgets(action_input, sizeof(action_input), stdin)) return;
    action_input[strcspn(action_input, "\n")] = '\0';

    char *token = strtok(action_input, ",");
    int action_idx = 0;
    while (token && action_idx < RULE_MAX_ACTIONS) {
        while (*token == ' ') token++;
        if (token[0] == 'c' || token[0] == 'C') {
            uart_puts(tr("Enter custom action: ", "输入自定义动作："));
            if (fgets(rule.actions[action_idx], sizeof(rule.actions[action_idx]), stdin)) {
                rule.actions[action_idx][strcspn(rule.actions[action_idx], "\n")] = '\0';
                action_idx++;
                rule.action_count++;
            }
        } else {
            int idx = atoi(token) - 1;
            if (actions[idx]) {
                safe_strncpy(rule.actions[action_idx], actions[idx], sizeof(rule.actions[action_idx]));
                action_idx++;
                rule.action_count++;
            }
        }
        token = strtok(NULL, ",");
    }

    rule.enabled = 1;
    rule.created_at = time(NULL);

    /* AI 守卫检查 */
    char guard_reason[256];
    if (rules_ai_guard_check(&rule, guard_reason, sizeof(guard_reason)) != 0) {
        uart_puts(tr("\n[WARN] AI Guard rejected: ", "\n[警告] AI 守卫拒绝："));
        uart_puts(guard_reason);
        uart_puts("\n");
        uart_puts(tr("Add anyway? (y/N): ", "仍然添加？(y/N): "));
        char confirm[8];
        if (!fgets(confirm, sizeof(confirm), stdin)) return;
        if (confirm[0] != 'y' && confirm[0] != 'Y') {
            uart_puts(tr("Cancelled.\n", "已取消。\n"));
            return;
        }
    }

    if (rules_engine_add(&rule) == 0) {
        uart_puts(tr("Rule added successfully.\n", "规则添加成功。\n"));
    } else {
        uart_puts(tr("Failed to add rule.\n", "添加规则失败。\n"));
    }
}

/* ============================================================
 * 删除规则
 * ============================================================ */

static void cmd_rules_remove(const char *name) {
    LOG_INFO_T("RulesCmd", "Remove", "Enter", "name=%s", name ? name : "(null)");

    if (!name || !*name) {
        uart_puts(tr("Usage: rule remove <name>\n", "用法：rule remove <名称>\n"));
        return;
    }

    if (rules_engine_remove(name) == 0) {
        uart_puts(tr("Rule removed.\n", "规则已删除。\n"));
    } else {
        uart_puts(tr("Rule not found.\n", "规则未找到。\n"));
    }
}

/* ============================================================
 * 启用/禁用规则
 * ============================================================ */

static void cmd_rules_toggle(const char *name) {
    LOG_INFO_T("RulesCmd", "Toggle", "Enter", "name=%s", name ? name : "(null)");

    if (!name || !*name) {
        uart_puts(tr("Usage: rule toggle <name>\n", "用法：rule toggle <名称>\n"));
        return;
    }

    rule_t rules[32];
    int count = rules_engine_list(rules, 32);
    for (int i = 0; i < count; i++) {
        if (strcmp(rules[i].name, name) == 0) {
            rules[i].enabled = !rules[i].enabled;
            rules_engine_add(&rules[i]);
            uart_puts(tr("Rule ", "规则 "));
            uart_puts(name);
            uart_puts(rules[i].enabled ? tr(" enabled.\n", " 已启用。\n") : tr(" disabled.\n", " 已禁用。\n"));
            return;
        }
    }

    uart_puts(tr("Rule not found.\n", "规则未找到。\n"));
}

/* ============================================================
 * 显示帮助
 * ============================================================ */

static void cmd_rules_help(void) {
    uart_puts(tr("\nRule Commands:\n", "\n规则命令：\n"));
    uart_puts(tr("  rule list              - List all rules\n", "  rule list              - 列出所有规则\n"));
    uart_puts(tr("  rule add <name>        - Create a new rule\n", "  rule add <名称>        - 创建新规则\n"));
    uart_puts(tr("  rule remove <name>     - Delete a rule\n", "  rule remove <名称>     - 删除规则\n"));
    uart_puts(tr("  rule toggle <name>     - Enable/disable a rule\n", "  rule toggle <名称>     - 启用/禁用规则\n"));
    uart_puts(tr("  rule help              - Show this help\n", "  rule help              - 显示帮助\n"));
}

/* ============================================================
 * 主分发函数
 * ============================================================ */

void rules_dispatch(const char *args) {
    LOG_DEBUG_T("RulesCmd", "Dispatch", "Enter", "args=%s", args ? args : "(null)");

    if (!args || !*args) {
        cmd_rules_help();
        return;
    }

    char cmd_buf[256];
    safe_strncpy(cmd_buf, args, sizeof(cmd_buf));
    cmd_buf[sizeof(cmd_buf) - 1] = '\0';

    char *saveptr;
    char *subcmd = strtok_r(cmd_buf, " ", &saveptr);
    char *arg = strtok_r(NULL, "", &saveptr);

    if (!subcmd) {
        cmd_rules_help();
        return;
    }

    if (strcmp(subcmd, "list") == 0) {
        cmd_rules_list();
    } else if (strcmp(subcmd, "add") == 0) {
        cmd_rules_add(arg);
    } else if (strcmp(subcmd, "remove") == 0) {
        cmd_rules_remove(arg);
    } else if (strcmp(subcmd, "toggle") == 0) {
        cmd_rules_toggle(arg);
    } else if (strcmp(subcmd, "help") == 0 || strcmp(subcmd, "--help") == 0) {
        cmd_rules_help();
    } else {
        uart_puts(tr("Unknown rule subcommand.\n", "未知的规则子命令。\n"));
        uart_puts(tr("Available: list, add, remove, toggle, help\n",
                     "可用：list, add, remove, toggle, help\n"));
    }
}