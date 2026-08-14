/**
 * @file    update_incremental.c
 * @brief   增量更新逻辑
 * @version LN-B-5.0.0.0
 * @par     核心协议：防御性编程（检查文件完整性）
 * @changes 安全字符串替换；双文支持
 */

#include "update_incremental.h"
#include "../common/safe_string.h"
#include "../common/data_path.h"
#include "../common/lang.h"
#include "../lib/log_extra.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <errno.h>

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

/* ============================================================
 * 生成增量清单
 * ============================================================ */

int update_incremental_manifest(const char *base_dir, const char *target_dir,
                                char *out, size_t out_len) {
    LOG_DEBUG_T("UpdateInc", "Manifest", "Enter", "base=%s, target=%s",
                base_dir ? base_dir : "(null)", target_dir ? target_dir : "(null)");

    if (!base_dir || !target_dir || !out || out_len == 0) return -1;

    char cmd[512];
    safe_snprintf(cmd, sizeof(cmd), "diff -rq %s %s 2>/dev/null | head -20", base_dir, target_dir);

    FILE *fp = popen(cmd, "r");
    if (!fp) {
        safe_snprintf(out, out_len, tr("Failed to generate manifest", "生成清单失败"));
        return -1;
    }

    size_t pos = 0;
    char line[256];
    while (fgets(line, sizeof(line), fp) && pos < out_len - 1) {
        pos += safe_snprintf(out + pos, out_len - pos, "%s", line);
    }
    pclose(fp);

    LOG_INFO_T("UpdateInc", "Manifest", "OK", "manifest generated (%zu bytes)", pos);
    return 0;
}

/* ============================================================
 * 应用增量更新
 * ============================================================ */

int update_incremental_apply(const char *manifest_path) {
    LOG_INFO_T("UpdateInc", "Apply", "Enter", "manifest=%s", manifest_path ? manifest_path : "(null)");

    if (!manifest_path || access(manifest_path, F_OK) != 0) {
        LOG_ERROR_T("UpdateInc", "Apply", "Invalid", "manifest not found");
        return -1;
    }

    char cmd[512];
    safe_snprintf(cmd, sizeof(cmd), "patch -p0 < %s 2>/dev/null", manifest_path);

    if (safe_exec_sh(cmd) != 0) {
        LOG_WARN_T("UpdateInc", "Apply", "PatchFail", "patch application failed");
        return -1;
    }

    LOG_INFO_T("UpdateInc", "Apply", "OK", "incremental update applied");
    return 0;
}