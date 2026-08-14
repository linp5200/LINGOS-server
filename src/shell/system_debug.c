/**
 * @file    system_debug.c
 * @brief   系统调试更新命令（system debug update）- 删除 /LINGOS 所有数据
 * @version 2.0.0.0
 */

#include "system_debug.h"
#include "../common/lang.h"
#include "uart.h"
#include "log_extra.h"
#include "../security/audit.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

#define LINGOS_ROOT "/LINGOS"

static int rm_rf(const char *path) {
    char cmd[1024];
    snprintf(cmd, sizeof(cmd), "rm -rf '%s'", path);
    return system(cmd);
}

void system_debug_update(void) {
    uart_puts(tr("\n!!! DANGER: This will delete ALL data in /LINGOS !!!\n",
                 "\n!!! 危险：这将删除 /LINGOS 目录下的所有数据 !!!\n"));
    uart_puts(tr("All user memories, skills, configurations, and logs will be permanently lost.\n",
                 "所有用户记忆、技能、配置和日志将永久丢失。\n"));
    uart_puts(tr("This operation is IRREVERSIBLE.\n", "此操作不可逆。\n"));
    uart_puts(tr("Type 'YES' to confirm: ", "输入 'YES' 确认: "));

    char confirm[16] = {0};
    int i = 0;
    while (1) {
        char c = uart_getc();
        if (c == '\r' || c == '\n') break;
        if (c == '\b' || c == 127) {
            if (i > 0) { i--; uart_puts("\b \b"); }
        } else if (i < 15) {
            confirm[i++] = c;
            uart_putc(c);
        }
    }
    uart_puts("\n");
    confirm[i] = '\0';

    if (strcmp(confirm, "YES") != 0) {
        uart_puts(tr("Operation cancelled.\n", "操作已取消。\n"));
        LOG_WARN_T("SystemDebug", "Update", "Cancelled", "user did not confirm");
        return;
    }

    uart_puts(tr("Deleting /LINGOS...\n", "正在删除 /LINGOS...\n"));
    LOG_INFO_T("SystemDebug", "Update", "Start", "deleting entire /LINGOS directory");

    audit_log("system", "debug_update", "system_debug_update", "{}", "deleting /LINGOS", 0, "critical", 1);

    int ret = rm_rf(LINGOS_ROOT);
    if (ret != 0) {
        uart_puts(tr("Failed to delete some files. You may need to manually remove /LINGOS.\n",
                     "删除部分文件失败。您可能需要手动删除 /LINGOS。\n"));
        LOG_ERROR_T("SystemDebug", "Update", "Fail", "rm -rf returned %d", ret);
        return;
    }

    uart_puts(tr("Successfully removed /LINGOS.\n", "已成功删除 /LINGOS。\n"));
    uart_puts(tr("Please restart LING OS to re-initialize the system.\n",
                 "请重启 LING OS 以重新初始化系统。\n"));
    LOG_INFO_T("SystemDebug", "Update", "Success", "/LINGOS removed, restart required");
}