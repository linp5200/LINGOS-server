/**
 * @file    registry_cmd.c
 * @brief   registry 命令实现
 * @version LN-B-5.0.0.0
 */

#include "../registry/registry.h"
#include "../common/lang.h"
#include "../common/safe_string.h"
#include "../drivers/uart.h"
#include "../lib/log_extra.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#define MAX_ENTRY_DISPLAY 64

/* ============================================================
 * 列出注册项
 * ============================================================ */
static void cmd_registry_list(const char *type_filter) {
    registry_entry_t *entries[MAX_ENTRY_DISPLAY];
    int type = -1;

    if (type_filter) {
        if (strcmp(type_filter, "module") == 0) type = REG_TYPE_MODULE;
        else if (strcmp(type_filter, "component") == 0) type = REG_TYPE_COMPONENT;
        else if (strcmp(type_filter, "config") == 0) type = REG_TYPE_CONFIG;
        else if (strcmp(type_filter, "feature") == 0) type = REG_TYPE_FEATURE;
        else if (strcmp(type_filter, "skill") == 0) type = REG_TYPE_SKILL;
        else if (strcmp(type_filter, "plugin") == 0) type = REG_TYPE_PLUGIN;
        else if (strcmp(type_filter, "hook") == 0) type = REG_TYPE_HOOK;
        else if (strcmp(type_filter, "selfcheck") == 0) type = REG_TYPE_SELFCHECK;
        else {
            uart_puts(tr("Unknown registry type. Available: module, component, config, feature, skill, plugin, hook, selfcheck\n",
                         "未知的注册表类型。可用：module, component, config, feature, skill, plugin, hook, selfcheck\n"));
            return;
        }
    }

    int count = registry_list(type, entries, MAX_ENTRY_DISPLAY);

    if (count == 0) {
        uart_puts(tr("No entries found.\n", "未找到注册项。\n"));
        return;
    }

    uart_puts(tr("\n=== Registry Entries ===\n", "\n=== 注册表条目 ===\n"));

    char buf[256];
    for (int i = 0; i < count; i++) {
        const char *status_str = "unknown";
        switch (entries[i]->status) {
            case REG_STATUS_ACTIVE: status_str = tr("active", "活跃"); break;
            case REG_STATUS_INACTIVE: status_str = tr("inactive", "非活跃"); break;
            case REG_STATUS_DEPRECATED: status_str = tr("deprecated", "已弃用"); break;
            case REG_STATUS_ERROR: status_str = tr("error", "错误"); break;
            default: break;
        }

        safe_snprintf(buf, sizeof(buf),
                      "  %d. %s [%s] (%s) - %s\n",
                      i + 1,
                      entries[i]->id,
                      entries[i]->name,
                      status_str,
                      entries[i]->version);
        uart_puts(buf);
    }

    safe_snprintf(buf, sizeof(buf),
                  tr("\nTotal: %d entries\n", "\n总计：%d 个条目\n"),
                  count);
    uart_puts(buf);
}

/* ============================================================
 * 显示注册项详情
 * ============================================================ */
static void cmd_registry_show(const char *id) {
    if (!id || !*id) {
        uart_puts(tr("Usage: registry show <id>\n", "用法：registry show <ID>\n"));
        return;
    }

    const registry_entry_t *entry = registry_get(id);
    if (!entry) {
        uart_puts(tr("Entry not found.\n", "未找到该条目。\n"));
        return;
    }

    uart_puts(tr("\n=== Registry Entry ===\n", "\n=== 注册表条目详情 ===\n"));

    char buf[256];
    safe_snprintf(buf, sizeof(buf),
                  "  ID: %s\n"
                  "  Name: %s\n"
                  "  Version: %s\n"
                  "  Path: %s\n",
                  entry->id,
                  entry->name,
                  entry->version,
                  entry->path);
    uart_puts(buf);

    char time_buf[32];
    struct tm *tm = localtime(&entry->created_at);
    strftime(time_buf, sizeof(time_buf), "%Y-%m-%d %H:%M:%S", tm);
    safe_snprintf(buf, sizeof(buf),
                  "  Created: %s\n",
                  time_buf);
    uart_puts(buf);

    tm = localtime(&entry->updated_at);
    strftime(time_buf, sizeof(time_buf), "%Y-%m-%d %H:%M:%S", tm);
    safe_snprintf(buf, sizeof(buf),
                  "  Updated: %s\n",
                  time_buf);
    uart_puts(buf);
}

/* ============================================================
 * 热重载注册表
 * ============================================================ */
static void cmd_registry_reload(void) {
    uart_puts(tr("Reloading registry...\n", "正在重载注册表...\n"));
    int ret = registry_reload();
    if (ret == 0) {
        uart_puts(tr("Registry reloaded successfully.\n", "注册表重载成功。\n"));
    } else {
        uart_puts(tr("Failed to reload registry.\n", "重载注册表失败。\n"));
    }
}

/* ============================================================
 * 主分发函数
 * ============================================================ */
void registry_dispatch(const char *args) {
    LOG_DEBUG_T("RegistryCmd", "Dispatch", "Enter", "args='%s'", args ? args : "(null)");

    if (!args || !*args) {
        uart_puts(tr("Usage: registry <subcommand> [args]\n", "用法：registry <子命令> [参数]\n"));
        uart_puts(tr("Subcommands:\n", "子命令：\n"));
        uart_puts(tr("  list [type]    - List entries (optional type filter)\n",
                     "  list [类型]    - 列出条目（可选类型过滤）\n"));
        uart_puts(tr("  show <id>      - Show entry details\n",
                     "  show <ID>      - 显示条目详情\n"));
        uart_puts(tr("  reload         - Hot reload registry\n",
                     "  reload         - 热重载注册表\n"));
        return;
    }

    char cmd_buf[256];
    safe_strncpy(cmd_buf, args, sizeof(cmd_buf));
    cmd_buf[sizeof(cmd_buf) - 1] = '\0';

    char *saveptr;
    char *subcmd = strtok_r(cmd_buf, " ", &saveptr);
    char *param = strtok_r(NULL, "", &saveptr);

    if (!subcmd) {
        uart_puts(tr("Missing subcommand.\n", "缺少子命令。\n"));
        return;
    }

    if (strcmp(subcmd, "list") == 0) {
        cmd_registry_list(param);
        return;
    }

    if (strcmp(subcmd, "show") == 0) {
        if (param) {
            /* 去除前导空格 */
            while (*param == ' ') param++;
            cmd_registry_show(param);
        } else {
            uart_puts(tr("Usage: registry show <id>\n", "用法：registry show <ID>\n"));
        }
        return;
    }

    if (strcmp(subcmd, "reload") == 0) {
        cmd_registry_reload();
        return;
    }

    uart_puts(tr("Unknown registry subcommand.\n", "未知的 registry 子命令。\n"));
}