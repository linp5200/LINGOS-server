/**
 * @file    update_rollback.c
 * @brief   回滚版本管理
 * @version LN-B-5.0.0.0
 * @par     核心协议：防弹编程（回滚失败时保留现场）
 * @changes 补充头文件；安全字符串替换；双文支持
 */

#include "update_rollback.h"
#include "../common/safe_string.h"
#include "../common/data_path.h"
#include "../common/lang.h"
#include "../lib/log_extra.h"
#include "../core/version.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <errno.h>

#define ROLLBACK_DIR "/LINGOS/backups/versions"

/* ============================================================
 * 获取回滚目录
 * ============================================================ */

static const char* get_rollback_dir(void) {
    static char path[512];
    if (path[0] == '\0') {
        const char *root = lingos_data_root();
        safe_snprintf(path, sizeof(path), "%s%s", root, ROLLBACK_DIR);
    }
    return path;
}

/* ============================================================
 * 安全执行命令（fork+execvp）
 * ============================================================ */
static int safe_exec(const char *cmd, char *const argv[]) {
    pid_t pid = fork();
    if (pid == -1) return -1;
    if (pid == 0) {
        execvp(cmd, argv);
        _exit(1);
    } else {
        int status;
        waitpid(pid, &status, 0);
        return (WIFEXITED(status) && WEXITSTATUS(status) == 0) ? 0 : -1;
    }
}

/* ============================================================
 * 创建版本备份（用于回滚）
 * ============================================================ */

int update_rollback_create(const char *version) {
    LOG_INFO_T("UpdateRollback", "Create", "Enter", "version=%s", version ? version : "(null)");

    if (!version || !*version) {
        LOG_ERROR_T("UpdateRollback", "Create", "Invalid", "version is NULL");
        return -1;
    }

    const char *dir = get_rollback_dir();
    mkdir(dir, 0755);

    char backup_path[512];
    safe_snprintf(backup_path, sizeof(backup_path), "%s/%s", dir, version);

    if (access(backup_path, F_OK) == 0) {
        LOG_DEBUG_T("UpdateRollback", "Create", "Exists", "backup already exists for %s", version);
        return 0;
    }

    char *mkdir_argv[] = {"/bin/mkdir", "-p", backup_path, NULL};
    if (safe_exec("/bin/mkdir", mkdir_argv) != 0) {
        LOG_ERROR_T("UpdateRollback", "Create", "MkdirFail", "mkdir failed");
        return -1;
    }

    char *cp_argv[] = {"/bin/cp", "/LINGOS/version", backup_path, NULL};
    if (safe_exec("/bin/cp", cp_argv) != 0) {
        LOG_ERROR_T("UpdateRollback", "Create", "CopyFail", "cp version failed");
        return -1;
    }

    LOG_INFO_T("UpdateRollback", "Create", "OK", "backup created for version %s", version);
    return 0;
}

/* ============================================================
 * 列出可回滚的版本
 * ============================================================ */

int update_rollback_list(char *out, size_t out_len) {
    LOG_DEBUG_T("UpdateRollback", "List", "Enter", "listing rollback versions");

    if (!out || out_len == 0) return -1;

    const char *dir = get_rollback_dir();
    DIR *d = opendir(dir);
    if (!d) {
        safe_snprintf(out, out_len, tr("No rollback versions available", "无可用的回滚版本"));
        return 0;
    }

    size_t pos = 0;
    struct dirent *entry;
    while ((entry = readdir(d)) != NULL && pos < out_len - 1) {
        if (entry->d_name[0] == '.') continue;
        pos += safe_snprintf(out + pos, out_len - pos, "%s\n", entry->d_name);
    }
    closedir(d);

    LOG_DEBUG_T("UpdateRollback", "List", "OK", "listed versions");
    return 0;
}

/* ============================================================
 * 执行回滚
 * ============================================================ */

int update_rollback_apply(const char *version) {
    LOG_INFO_T("UpdateRollback", "Apply", "Enter", "version=%s", version ? version : "(null)");

    if (!version || !*version) {
        LOG_ERROR_T("UpdateRollback", "Apply", "Invalid", "version is NULL");
        return -1;
    }

    const char *dir = get_rollback_dir();
    char backup_path[512];
    safe_snprintf(backup_path, sizeof(backup_path), "%s/%s", dir, version);

    if (access(backup_path, F_OK) != 0) {
        LOG_ERROR_T("UpdateRollback", "Apply", "NotFound", "backup for %s not found", version);
        return -1;
    }

    char version_file[512];
    safe_snprintf(version_file, sizeof(version_file), "%s/version", backup_path);
    if (access(version_file, F_OK) == 0) {
        char *cp_argv[] = {"/bin/cp", version_file, "/LINGOS/version", NULL};
        if (safe_exec("/bin/cp", cp_argv) != 0) {
            LOG_ERROR_T("UpdateRollback", "Apply", "RestoreFail", "failed to restore version");
            return -1;
        }
    }

    LOG_INFO_T("UpdateRollback", "Apply", "OK", "rolled back to version %s", version);
    return 0;
}