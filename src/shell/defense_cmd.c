/**
 * @file    defense_cmd.c
 * @brief   system defense 命令实现
 * @version LN-B-5.0.0.0
 */

#include "../security/defense_mode.h"
#include "../security/security_config.h"
#include "../common/lang.h"
#include "../common/safe_string.h"
#include "../drivers/uart.h"
#include "../lib/log_extra.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

/* ============================================================
 * 显示防御状态
 * ============================================================ */
static void cmd_defense_status(void) {
    defense_mode_t mode = defense_mode_get();
    const security_config_t *cfg = security_config_get();

    uart_puts(tr("\n=== Defense System Status ===\n", "\n=== 防御系统状态 ===\n"));

    char buf[128];

    /* 当前模式 */
    safe_snprintf(buf, sizeof(buf),
                  tr("Current mode: %s\n", "当前模式：%s\n"),
                  defense_mode_name(mode));
    uart_puts(buf);

    /* 各模式状态 */
    safe_snprintf(buf, sizeof(buf),
                  tr("  Shadow mode: %s\n", "  影子模式：%s\n"),
                  cfg && cfg->shadow_enabled ? tr("enabled", "启用") : tr("disabled", "禁用"));
    uart_puts(buf);

    safe_snprintf(buf, sizeof(buf),
                  tr("  Dark mode: %s\n", "  暗影模式：%s\n"),
                  cfg && cfg->dark_enabled ? tr("enabled", "启用") : tr("disabled", "禁用"));
    uart_puts(buf);

    safe_snprintf(buf, sizeof(buf),
                  tr("  Absolute protect: %s\n", "  绝对保护：%s\n"),
                  cfg && cfg->absolute_enabled ? tr("enabled", "启用") : tr("disabled", "禁用"));
    uart_puts(buf);

    /* 输入模式 */
    safe_snprintf(buf, sizeof(buf),
                  tr("Input mode: %s\n", "输入模式：%s\n"),
                  cfg ? cfg->input_mode : "unknown");
    uart_puts(buf);
}

/* ============================================================
 * 切换防御模式
 * ============================================================ */
static void cmd_defense_set(const char *mode_name, int enabled) {
    defense_mode_t target;

    if (strcmp(mode_name, "shadow") == 0) {
        target = enabled ? DEFENSE_MODE_SHADOW : DEFENSE_MODE_NONE;
    } else if (strcmp(mode_name, "dark") == 0) {
        target = enabled ? DEFENSE_MODE_DARK : DEFENSE_MODE_NONE;
    } else if (strcmp(mode_name, "absolute") == 0) {
        if (enabled) {
            uart_puts(tr(
                "Activating absolute protect. This will block all external inputs.\n"
                "Type 'y' to confirm: ",
                "正在激活绝对保护。这将阻断所有外部输入。\n"
                "输入 'y' 确认："
            ));
            char c = uart_getc();
            uart_putc(c);
            uart_puts("\n");
            if (c != 'y' && c != 'Y') {
                uart_puts(tr("Operation cancelled.\n", "操作已取消。\n"));
                return;
            }
        }
        target = enabled ? DEFENSE_MODE_ABSOLUTE : DEFENSE_MODE_NONE;
    } else {
        uart_puts(tr("Unknown defense mode. Available: shadow, dark, absolute\n",
                     "未知防御模式。可用：shadow, dark, absolute\n"));
        return;
    }

    int ret = defense_mode_set(target);
    if (ret == 0) {
        char buf[128];
        safe_snprintf(buf, sizeof(buf),
                      tr("Defense mode set to: %s\n", "防御模式已切换至：%s\n"),
                      enabled ? mode_name : "none");
        uart_puts(buf);
    } else {
        uart_puts(tr("Failed to set defense mode. Cannot downgrade from higher mode.\n",
                     "切换防御模式失败。无法从高级模式降级。\n"));
    }
}

/* ============================================================
 * 主分发函数
 * ============================================================ */
void defense_dispatch(const char *args) {
    LOG_DEBUG_T("DefenseCmd", "Dispatch", "Enter", "args='%s'", args ? args : "(null)");

    if (!args || !*args || strcmp(args, "status") == 0) {
        cmd_defense_status();
        return;
    }

    char cmd_buf[128];
    safe_strncpy(cmd_buf, args, sizeof(cmd_buf));
    cmd_buf[sizeof(cmd_buf) - 1] = '\0';

    char *saveptr;
    char *subcmd = strtok_r(cmd_buf, " ", &saveptr);
    char *mode = strtok_r(NULL, " ", &saveptr);

    if (!subcmd) {
        cmd_defense_status();
        return;
    }

    if (strcmp(subcmd, "shadow") == 0) {
        if (!mode) {
            uart_puts(tr("Usage: system defense shadow on|off\n", "用法：system defense shadow on|off\n"));
            return;
        }
        if (strcmp(mode, "on") == 0) cmd_defense_set("shadow", 1);
        else if (strcmp(mode, "off") == 0) cmd_defense_set("shadow", 0);
        else uart_puts(tr("Invalid value. Use 'on' or 'off'.\n", "无效值。请使用 'on' 或 'off'。\n"));
        return;
    }

    if (strcmp(subcmd, "dark") == 0) {
        if (!mode) {
            uart_puts(tr("Usage: system defense dark on|off\n", "用法：system defense dark on|off\n"));
            return;
        }
        if (strcmp(mode, "on") == 0) cmd_defense_set("dark", 1);
        else if (strcmp(mode, "off") == 0) cmd_defense_set("dark", 0);
        else uart_puts(tr("Invalid value. Use 'on' or 'off'.\n", "无效值。请使用 'on' 或 'off'。\n"));
        return;
    }

    if (strcmp(subcmd, "absolute") == 0) {
        if (!mode) {
            uart_puts(tr("Usage: system defense absolute on|off\n", "用法：system defense absolute on|off\n"));
            return;
        }
        if (strcmp(mode, "on") == 0) cmd_defense_set("absolute", 1);
        else if (strcmp(mode, "off") == 0) cmd_defense_set("absolute", 0);
        else uart_puts(tr("Invalid value. Use 'on' or 'off'.\n", "无效值。请使用 'on' 或 'off'。\n"));
        return;
    }

    uart_puts(tr("Unknown defense subcommand.\n", "未知的 defense 子命令。\n"));
    uart_puts(tr("Available: status, shadow on|off, dark on|off, absolute on|off\n",
                 "可用：status, shadow on|off, dark on|off, absolute on|off\n"));
}