/**
 * @file    web_update.c
 * @brief   Web UI 组件更新处理
 * @version LN-B-5.0.0.0
 * @changes 安全字符串替换；双文支持
 */

#include "web_update.h"
#include "../lib/log_extra.h"
#include "../common/data_path.h"
#include "../common/safe_string.h"
#include "../common/lang.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <errno.h>
#include <time.h>

/* ============================================================
 * 安全执行命令（fork+execvp）
 * ============================================================ */
static int safe_exec_sh(const char *cmd_str) {
    pid_t pid = fork();
    if (pid == -1) return -1;
    if (pid == 0) {
        execl("/bin/sh", "sh", "-c", cmd_str, (char *)NULL);
        _exit(1);
    } else {
        int status;
        waitpid(pid, &status, 0);
        return (WIFEXITED(status) && WEXITSTATUS(status) == 0) ? 0 : -1;
    }
}

static int mkdir_p(const char *path) {
    char tmp[512];
    char *p = NULL;
    size_t len;
    safe_snprintf(tmp, sizeof(tmp), "%s", path);
    len = strlen(tmp);
    if (tmp[len - 1] == '/') tmp[len - 1] = '\0';
    for (p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            if (mkdir(tmp, 0755) != 0 && errno != EEXIST) {
                return -1;
            }
            *p = '/';
        }
    }
    if (mkdir(tmp, 0755) != 0 && errno != EEXIST) {
        return -1;
    }
    return 0;
}

/* ============================================================
 * 安装 Web 组件
 * ============================================================ */

int install_web_component(const char *extract_dir) {
    if (!extract_dir) {
        LOG_ERROR_T("WebUpdate", "Install", "NoDir", "extract_dir is NULL");
        return -1;
    }

    char src_web[512];
    safe_snprintf(src_web, sizeof(src_web), "%s/web", extract_dir);
    if (access(src_web, F_OK) != 0) {
        LOG_DEBUG_T("WebUpdate", "Install", "NoWebDir", "no web/ directory in update package");
        return 0;
    }

    const char *root = lingos_data_root();
    char dst_web[512];
    safe_snprintf(dst_web, sizeof(dst_web), "%s/web", root);

    char backup[512];
    safe_snprintf(backup, sizeof(backup), "%s.backup_%d", dst_web, (int)time(NULL));
    if (access(dst_web, F_OK) == 0) {
        if (rename(dst_web, backup) != 0) {
            LOG_ERROR_T("WebUpdate", "Install", "BackupFail", "cannot rename old web to %s", backup);
            return -1;
        }
        LOG_INFO_T("WebUpdate", "Install", "Backup", "old web backed up to %s", backup);
    }

    if (mkdir_p(dst_web) != 0) {
        LOG_ERROR_T("WebUpdate", "Install", "MkdirFail", "cannot create %s", dst_web);
        if (access(backup, F_OK) == 0) {
            rename(backup, dst_web);
        }
        return -1;
    }

    char cmd[1024];
    safe_snprintf(cmd, sizeof(cmd), "cp -r '%s'/* '%s/' 2>/dev/null", src_web, dst_web);
    if (safe_exec_sh(cmd) != 0) {
        LOG_ERROR_T("WebUpdate", "Install", "CopyFail", "cp -r failed");
        if (access(backup, F_OK) == 0) {
            rename(backup, dst_web);
        }
        return -1;
    }

    LOG_INFO_T("WebUpdate", "Install", "OK", "web UI updated from %s", src_web);
    return 0;
}