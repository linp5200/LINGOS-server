/**
 * @file    privilege_cmd.c
 * @brief   system privilege 命令实现
 * @version LN-B-5.0.0.0
 */

#include "../security/privilege_manager.h"
#include "../common/lang.h"
#include "../common/safe_string.h"
#include "../drivers/uart.h"
#include "../lib/log_extra.h"
#include "privilege_manager.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

/* ============================================================
 * 显示权限状态
 * ============================================================ */
static void cmd_privilege_status(void) {
    char mode[16];
    privilege_get_mode(mode, sizeof(mode));

    uart_puts(tr("\n=== Privilege Mode Status ===\n", "\n=== 权限模式状态 ===\n"));

    char buf[128];
    safe_snprintf(buf, sizeof(buf),
                  tr("Current mode: %s\n", "当前模式：%s\n"),
                  mode);
    uart_puts(buf);

    if (strcmp(mode, "developer") == 0) {
        long remaining = privilege_get_remaining_seconds();
        long hours = remaining / 3600;
        long mins = (remaining % 3600) / 60;
        safe_snprintf(buf, sizeof(buf),
                      tr("Auto-revert in: %ldh %ldm\n", "自动恢复时间：%ld小时 %ld分钟\n"),
                      hours, mins);
        uart_puts(buf);
    }

    if (privilege_is_locked()) {
        uart_puts(tr("  ⚠️ System is LOCKED\n", "  ⚠️ 系统已锁定\n"));
    }
}

/* ============================================================
 * 切换开发者模式
 * ============================================================ */
static void cmd_privilege_developer(const char *mode) {
    if (!mode) {
        uart_puts(tr("Usage: system privilege developer on|off\n", "用法：system privilege developer on|off\n"));
        return;
    }

    int enable;
    if (strcmp(mode, "on") == 0) {
        enable = 1;
    } else if (strcmp(mode, "off") == 0) {
        enable = 0;
    } else {
        uart_puts(tr("Invalid value. Use 'on' or 'off'.\n", "无效值。请使用 'on' 或 'off'。\n"));
        return;
    }

    /* 如果启用开发者模式，需要用户确认 */
    if (enable) {
        uart_puts(tr(
            "\n⚠️ WARNING: Developer mode gives all processes root privileges.\n"
            "This can be dangerous. Proceed? (y/N): ",
            "\n⚠️ 警告：开发者模式将以 root 权限运行所有进程。\n"
            "这可能很危险。继续？(y/N)："
        ));
        char c = uart_getc();
        uart_putc(c);
        uart_puts("\n");
        if (c != 'y' && c != 'Y') {
            uart_puts(tr("Operation cancelled.\n", "操作已取消。\n"));
            return;
        }
    }

    int ret = privilege_set_developer(enable);
    if (ret == 0) {
        if (enable) {
            uart_puts(tr("Developer mode enabled for 24 hours.\n", "开发者模式已启用，持续24小时。\n"));
        } else {
            uart_puts(tr("Developer mode disabled.\n", "开发者模式已禁用。\n"));
        }
    } else {
        uart_puts(tr("Failed to change privilege mode.\n", "切换权限模式失败。\n"));
    }
}

/* ============================================================
 * 主分发函数
 * ============================================================ */
void privilege_dispatch(const char *args) {
    LOG_DEBUG_T("PrivilegeCmd", "Dispatch", "Enter", "args='%s'", args ? args : "(null)");

    if (!args || !*args || strcmp(args, "status") == 0) {
        cmd_privilege_status();
        return;
    }

    char cmd_buf[128];
    safe_strncpy(cmd_buf, args, sizeof(cmd_buf));
    cmd_buf[sizeof(cmd_buf) - 1] = '\0';

    char *saveptr;
    char *subcmd = strtok_r(cmd_buf, " ", &saveptr);
    char *param = strtok_r(NULL, " ", &saveptr);

    if (!subcmd) {
        cmd_privilege_status();
        return;
    }

    if (strcmp(subcmd, "developer") == 0) {
        cmd_privilege_developer(param);
        return;
    }

    uart_puts(tr("Unknown privilege subcommand.\n", "未知的 privilege 子命令。\n"));
    uart_puts(tr("Available: status, developer on|off\n", "可用：status, developer on|off\n"));
}