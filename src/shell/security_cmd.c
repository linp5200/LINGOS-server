/**
 * @file    security_cmd.c
 * @brief   system security 命令实现
 * @version LN-B-5.0.0.0
 */

#include "../security/security_config.h"
#include "../security/execution_gate.h"
#include "../common/lang.h"
#include "../common/safe_string.h"
#include "../drivers/uart.h"
#include "../lib/log_extra.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

/* ============================================================
 * 显示安全状态
 * ============================================================ */
static void cmd_security_status(void) {
    const security_config_t *cfg = security_config_get();
    const char *gate_mode = execution_gate_get_mode();

    uart_puts(tr("\n=== Security Status ===\n", "\n=== 安全状态 ===\n"));

    char buf[128];

    if (cfg) {
        safe_snprintf(buf, sizeof(buf),
                      tr("Input mode: %s\n", "输入模式：%s\n"),
                      cfg->input_mode);
        uart_puts(buf);
    }

    safe_snprintf(buf, sizeof(buf),
                  tr("Execution gate mode: %s\n", "执行门模式：%s\n"),
                  gate_mode);
    uart_puts(buf);

    /* 白名单数量 */
    char whitelist[4096];
    execution_gate_whitelist_list(whitelist, sizeof(whitelist));
    int count = 0;
    char *p = whitelist;
    while (*p) {
        if (*p == '\n') count++;
        p++;
    }
    safe_snprintf(buf, sizeof(buf),
                  tr("Whitelist entries: %d\n", "白名单条目：%d\n"),
                  count);
    uart_puts(buf);
}

/* ============================================================
 * 设置输入模式
 * ============================================================ */
static void cmd_security_input(const char *mode) {
    if (!mode) {
        uart_puts(tr("Usage: system security input strict|balanced|permissive\n",
                     "用法：system security input strict|balanced|permissive\n"));
        return;
    }

    if (strcmp(mode, "strict") != 0 &&
        strcmp(mode, "balanced") != 0 &&
        strcmp(mode, "permissive") != 0) {
        uart_puts(tr("Invalid input mode. Must be strict, balanced, or permissive.\n",
                     "无效的输入模式。必须是 strict、balanced 或 permissive。\n"));
        return;
    }

    int ret = security_config_set_input_mode(mode);
    if (ret == 0) {
        execution_gate_set_mode(mode);
        char buf[128];
        safe_snprintf(buf, sizeof(buf),
                      tr("Input mode set to: %s\n", "输入模式已设置为：%s\n"),
                      mode);
        uart_puts(buf);
    } else {
        uart_puts(tr("Failed to set input mode.\n", "设置输入模式失败。\n"));
    }
}

/* ============================================================
 * 主分发函数
 * ============================================================ */
void security_dispatch(const char *args) {
    LOG_DEBUG_T("SecurityCmd", "Dispatch", "Enter", "args='%s'", args ? args : "(null)");

    if (!args || !*args || strcmp(args, "status") == 0) {
        cmd_security_status();
        return;
    }

    char cmd_buf[128];
    safe_strncpy(cmd_buf, args, sizeof(cmd_buf));
    cmd_buf[sizeof(cmd_buf) - 1] = '\0';

    char *saveptr;
    char *subcmd = strtok_r(cmd_buf, " ", &saveptr);
    char *param = strtok_r(NULL, " ", &saveptr);

    if (!subcmd) {
        cmd_security_status();
        return;
    }

    if (strcmp(subcmd, "input") == 0) {
        cmd_security_input(param);
        return;
    }

    uart_puts(tr("Unknown security subcommand.\n", "未知的 security 子命令。\n"));
    uart_puts(tr("Available: status, input strict|balanced|permissive\n",
                 "可用：status, input strict|balanced|permissive\n"));
}