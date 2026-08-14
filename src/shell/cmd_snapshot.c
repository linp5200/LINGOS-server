/**
 * @file    cmd_snapshot.c
 * @brief   快照管理命令：create, list, restore, delete, diff
 * @version LN-B-4.2.0.0
 * @path    src/shell/cmd_snapshot.c
 * @note    根据约定，所有 Shell 命令源文件均置于 src/shell/ 目录下
 */

#include "snapshot.h"
#include "lang.h"
#include "safe_string.h"
#include "uart.h"
#include "log_extra.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* ============================================================
 * 内部辅助
 * ============================================================ */

static void cmd_snapshot_list(void) {
    LOG_DEBUG_T("CmdSnapshot", "List", "Enter", "listing snapshots");

    snapshot_info_t infos[32];
    int count = snapshot_list(infos, 32);

    if (count == 0) {
        uart_puts(tr("No snapshots found.\n", "未找到快照。\n"));
        uart_puts(tr("  Create a snapshot: snapshot create <name>\n",
                     "  创建快照：snapshot create <名称>\n"));
        return;
    }

    uart_puts(tr("\n=== Snapshots ===\n", "\n=== 快照列表 ===\n"));
    char buf[256];

    for (int i = 0; i < count; i++) {
        char time_str[32];
        struct tm *tm = localtime(&infos[i].created_at);
        strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S", tm);

        safe_snprintf(buf, sizeof(buf),
                      "  %d. %s\n"
                      "     ID: %s\n"
                      "     Name: %s\n"
                      "     Created: %s\n"
                      "     Size: %lld KB\n"
                      "     Version: %s\n",
                      i + 1,
                      infos[i].description[0] ? infos[i].description : "(no description)",
                      infos[i].id,
                      infos[i].name[0] ? infos[i].name : "(unnamed)",
                      time_str,
                      (long long)(infos[i].size_bytes / 1024),
                      infos[i].version[0] ? infos[i].version : "unknown");
        uart_puts(buf);
    }

    safe_snprintf(buf, sizeof(buf), tr("\nTotal: %d snapshots\n", "\n总计：%d 个快照\n"), count);
    uart_puts(buf);
}

static void cmd_snapshot_create(const char *name, const char *desc) {
    LOG_INFO_T("CmdSnapshot", "Create", "Enter", "name='%s', desc='%s'",
               name ? name : "(null)", desc ? desc : "(null)");

    if (!name || !*name) {
        uart_puts(tr("Usage: snapshot create <name> [description]\n",
                     "用法：snapshot create <名称> [描述]\n"));
        return;
    }

    uart_puts(tr("Creating snapshot...\n", "正在创建快照...\n"));

    char id[64];
    int ret = snapshot_create(name, desc, id, sizeof(id));

    if (ret == 0) {
        uart_puts(tr("Snapshot created successfully.\n", "快照创建成功。\n"));
        uart_puts(tr("  ID: ", "  ID: "));
        uart_puts(id);
        uart_puts("\n");
        LOG_INFO_T("CmdSnapshot", "Create", "OK", "created snapshot %s", id);
    } else {
        uart_puts(tr("Failed to create snapshot.\n", "创建快照失败。\n"));
        LOG_ERROR_T("CmdSnapshot", "Create", "Fail", "snapshot_create returned %d", ret);
    }
}

static void cmd_snapshot_restore(const char *id) {
    LOG_INFO_T("CmdSnapshot", "Restore", "Enter", "id='%s'", id ? id : "(null)");

    if (!id || !*id) {
        uart_puts(tr("Usage: snapshot restore <id>\n", "用法：snapshot restore <ID>\n"));
        uart_puts(tr("  Use 'snapshot list' to see available snapshot IDs.\n",
                     "  使用 'snapshot list' 查看可用快照 ID。\n"));
        return;
    }

    int ret = snapshot_restore(id);
    if (ret == 0) {
        LOG_INFO_T("CmdSnapshot", "Restore", "OK", "restored snapshot %s", id);
    } else {
        uart_puts(tr("Failed to restore snapshot.\n", "恢复快照失败。\n"));
        LOG_ERROR_T("CmdSnapshot", "Restore", "Fail", "snapshot_restore returned %d", ret);
    }
}

static void cmd_snapshot_delete(const char *id) {
    LOG_INFO_T("CmdSnapshot", "Delete", "Enter", "id='%s'", id ? id : "(null)");

    if (!id || !*id) {
        uart_puts(tr("Usage: snapshot delete <id>\n", "用法：snapshot delete <ID>\n"));
        return;
    }

    int ret = snapshot_delete(id);
    if (ret == 0) {
        uart_puts(tr("Snapshot deleted.\n", "快照已删除。\n"));
        LOG_INFO_T("CmdSnapshot", "Delete", "OK", "deleted snapshot %s", id);
    } else {
        uart_puts(tr("Failed to delete snapshot.\n", "删除快照失败。\n"));
        LOG_ERROR_T("CmdSnapshot", "Delete", "Fail", "snapshot_delete returned %d", ret);
    }
}

static void cmd_snapshot_diff(const char *id) {
    LOG_INFO_T("CmdSnapshot", "Diff", "Enter", "id='%s'", id ? id : "(null)");

    if (!id || !*id) {
        uart_puts(tr("Usage: snapshot diff <id>\n", "用法：snapshot diff <ID>\n"));
        return;
    }

    uart_puts(tr("Comparing snapshot with current system...\n", "正在比较快照与当前系统...\n"));

    char buf[4096];
    int ret = snapshot_diff(id, buf, sizeof(buf));

    if (ret == 0) {
        log_draw_box(tr("Diff Result", "差异结果"), buf, COLOR_CYAN, COLOR_DIM, COLOR_WHITE);
        LOG_INFO_T("CmdSnapshot", "Diff", "OK", "diff for snapshot %s", id);
    } else {
        uart_puts(tr("Failed to compare snapshot.\n", "比较快照失败。\n"));
        LOG_ERROR_T("CmdSnapshot", "Diff", "Fail", "snapshot_diff returned %d", ret);
    }
}

static void cmd_snapshot_help(void) {
    uart_puts(tr("\nSnapshot Commands:\n", "\n快照命令：\n"));
    uart_puts(tr("  snapshot create <name> [desc]  - Create a snapshot\n",
                 "  snapshot create <名称> [描述]  - 创建快照\n"));
    uart_puts(tr("  snapshot list                  - List all snapshots\n",
                 "  snapshot list                  - 列出所有快照\n"));
    uart_puts(tr("  snapshot restore <id>          - Restore a snapshot\n",
                 "  snapshot restore <ID>          - 恢复快照\n"));
    uart_puts(tr("  snapshot delete <id>           - Delete a snapshot\n",
                 "  snapshot delete <ID>           - 删除快照\n"));
    uart_puts(tr("  snapshot diff <id>             - Show differences\n",
                 "  snapshot diff <ID>             - 显示差异\n"));
}

/* ============================================================
 * 公共 API
 * ============================================================ */

void snapshot_dispatch(const char *args) {
    LOG_DEBUG_T("CmdSnapshot", "Dispatch", "Enter", "args='%s'", args ? args : "(null)");

    if (!args || !*args) {
        cmd_snapshot_help();
        return;
    }

    char cmd_buf[256];
    safe_strncpy(cmd_buf, args, sizeof(cmd_buf));
    cmd_buf[sizeof(cmd_buf) - 1] = '\0';

    char *saveptr;
    char *subcmd = strtok_r(cmd_buf, " ", &saveptr);
    char *arg1 = strtok_r(NULL, " ", &saveptr);
    char *arg2 = strtok_r(NULL, "", &saveptr);

    if (!subcmd) {
        cmd_snapshot_help();
        return;
    }

    if (strcmp(subcmd, "list") == 0) {
        cmd_snapshot_list();
    } else if (strcmp(subcmd, "create") == 0) {
        cmd_snapshot_create(arg1, arg2);
    } else if (strcmp(subcmd, "restore") == 0) {
        cmd_snapshot_restore(arg1);
    } else if (strcmp(subcmd, "delete") == 0) {
        cmd_snapshot_delete(arg1);
    } else if (strcmp(subcmd, "diff") == 0) {
        cmd_snapshot_diff(arg1);
    } else if (strcmp(subcmd, "help") == 0 || strcmp(subcmd, "--help") == 0) {
        cmd_snapshot_help();
    } else {
        uart_puts(tr("Unknown snapshot command: ", "未知快照命令："));
        uart_puts(subcmd);
        uart_puts("\n");
        uart_puts(tr("Available: create, list, restore, delete, diff, help\n",
                     "可用命令：create, list, restore, delete, diff, help\n"));
    }
}