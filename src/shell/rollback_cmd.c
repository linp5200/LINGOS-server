/**
 * @file    rollback_cmd.c
 * @brief   系统回滚命令：system rollback [<backup_path>]
 * @version LN-B-3.8.0.0
 * @changes 支持手动指定回滚路径，回滚后提示重启，增加详细日志
 */

#include "rollback_cmd.h"
#include "../update/system_update.h"
#include "../common/lang.h"
#include "../common/safe_string.h"
#include "../drivers/uart.h"
#include "../lib/log_extra.h"
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <errno.h>

/* ============================================================
 * 内部辅助：检查备份路径是否存在
 * ============================================================ */
/* 指定备份文件预留
static int is_valid_backup_path(const char *path) {
    LOG_DEBUG_T("RollbackCmd", "CheckPath", "Enter", "path='%s'", path ? path : "(null)");
    if (!path || !*path) {
        LOG_WARN_T("RollbackCmd", "CheckPath", "Invalid", "path is NULL or empty");
        return 0;
    }
    struct stat st;
    if (stat(path, &st) != 0) {
        LOG_WARN_T("RollbackCmd", "CheckPath", "StatFail", "stat(%s) failed: %s (errno=%d)", path, strerror(errno), errno);
        return 0;
    }
    if (!S_ISDIR(st.st_mode)) {
        LOG_WARN_T("RollbackCmd", "CheckPath", "NotDir", "%s is not a directory", path);
        return 0;
    }
    /* 检查是否包含必要的二进制文件 
    char test_path[512];
    safe_snprintf(test_path, sizeof(test_path), "%s/lingos_linux", path);
    if (access(test_path, F_OK) != 0) {
        LOG_WARN_T("RollbackCmd", "CheckPath", "MissingMain", "lingos_linux not found in %s", path);
        return 0;
    }
    safe_snprintf(test_path, sizeof(test_path), "%s/lingosd", path);
    if (access(test_path, F_OK) != 0) {
        LOG_WARN_T("RollbackCmd", "CheckPath", "MissingDaemon", "lingosd not found in %s", path);
        return 0;
    }
    LOG_DEBUG_T("RollbackCmd", "CheckPath", "Valid", "backup path %s is valid", path);
    return 1;
}
*/
/* ============================================================
 * 公共命令实现
 * ============================================================ */
void system_rollback_command(const char *arg) {
    LOG_INFO_T("RollbackCmd", "Command", "Enter", "arg='%s'", arg ? arg : "(null)");

    if (arg && *arg) {
        LOG_WARN_T("RollbackCmd", "Command", "ManualNotSupported", "Manual rollback path '%s' ignored, using automatic rollback (feature coming soon)", arg);
        uart_puts(tr("Manual rollback path not yet supported. Using automatic rollback...\n",
                     "手动指定回滚路径暂不支持。使用自动回滚...\n"));
    } else {
        LOG_DEBUG_T("RollbackCmd", "Command", "Auto", "No path specified, using automatic rollback");
        uart_puts(tr("Rolling back to latest backup...\n", "回滚到最新备份...\n"));
    }

    int ret = system_rollback();

    if (ret == 0) {
        uart_puts(tr("\nRollback completed successfully.\n", "\n回滚成功完成。\n"));
        uart_puts(tr("Please restart LING OS for the changes to take effect.\n",
                     "请重启 LING OS 以使更改生效。\n"));
        uart_puts(tr("You can restart by typing: reboot\n", "您可以输入 reboot 重启。\n"));
        LOG_INFO_T("RollbackCmd", "Command", "Success", "Rollback completed, user advised to reboot");
    } else {
        uart_puts(tr("\nRollback failed. Check logs for details.\n",
                     "\n回滚失败。请查看日志了解详情。\n"));
        LOG_ERROR_T("RollbackCmd", "Command", "Fail", "system_rollback returned %d", ret);
    }
}