/**
 * @file    dependency_check.c
 * @brief   系统依赖检查实现
 * @version LN-B-5.1.2.6-rc
 * @changes 移除 check_system_library 定义，改用 env_detect 中的实现
 */

#include "dependency_check.h"
#include "../common/safe_string.h"
#include "../common/data_path.h"
#include "../lib/log_extra.h"
#include "../fs/env_detect.h"   /* 新增：声明 check_system_library */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/statvfs.h>

int check_python_module(const char *module_name) {
    if (!module_name) return 0;
    char path[512];
    const char *root = lingos_data_root();
    safe_snprintf(path, sizeof(path), "%s/bin/%s.py", root, module_name);
    return (access(path, F_OK) == 0);
}

int check_system_command(const char *cmd) {
    if (!cmd) return 0;
    char buf[256];
    safe_snprintf(buf, sizeof(buf), "command -v %s > /dev/null 2>&1", cmd);
    return (system(buf) == 0);
}

/* 原 check_system_library 函数已删除，因为已在 ../fs/env_detect.c 中实现 */

int check_disk_space(int required_mb) {
    const char *root = lingos_data_root();
    struct statvfs stat;
    if (statvfs(root, &stat) != 0) return 0;
    unsigned long long free_space = (unsigned long long)stat.f_bsize * stat.f_bavail;
    unsigned long long free_mb = free_space / (1024 * 1024);
    return (free_mb >= (unsigned long long)required_mb);
}

int check_notcurses_available(void) {
    /* 检查库是否存在（现在使用外部 check_system_library） */
    if (check_system_library("libnotcurses.so")) return 1;
    /* 备用：检查头文件 */
    if (access("/usr/include/notcurses/notcurses.h", F_OK) == 0) return 1;
    return 0;
}

int check_required_dependencies(dep_check_result_t *result) {
    if (!result) return -1;
    memset(result, 0, sizeof(*result));
    char *details = result->details;
    char *suggestions = result->suggestions;
    int d_off = 0, s_off = 0;

    LOG_INFO_T("DependencyCheck", "Start", "Begin", "checking all dependencies");

    /* ===== 1. 检查 Python 模块 ===== */
    const char *modules[] = {
        "ai_server", "skill_handlers", "syscall_client", 
        "config_helpers", "authorization_service", "sub_ai_worker",
        "sub_ai_scheduler", "repair_engine"
    };
    for (int i = 0; i < 8; i++) {
        if (!check_python_module(modules[i])) {
            d_off += safe_snprintf(details + d_off, 256 - d_off,
                "❌ Python module: %s.py (missing)\n", modules[i]);
            result->python_missing++;
        }
    }

    /* ===== 2. 检查系统命令 ===== */
    const char *cmds[] = {"python3", "curl", "tar", "gzip", "sha256sum", "ldconfig"};
    for (int i = 0; i < 6; i++) {
        if (!check_system_command(cmds[i])) {
            d_off += safe_snprintf(details + d_off, 256 - d_off,
                "❌ Command: %s (missing)\n", cmds[i]);
            result->cmd_missing++;
        }
    }

    /* ===== 3. 检查动态库（使用外部 check_system_library） ===== */
    const char *libs[] = {"libcurl.so", "libmicrohttpd.so", "libnotcurses.so"};
    for (int i = 0; i < 3; i++) {
        if (!check_system_library(libs[i])) {
            d_off += safe_snprintf(details + d_off, 256 - d_off,
                "❌ Library: %s (missing)\n", libs[i]);
            result->lib_missing++;
        }
    }

    /* ===== 4. 检查磁盘空间 ===== */
    if (!check_disk_space(100)) {
        d_off += safe_snprintf(details + d_off, 256 - d_off,
            "❌ Disk space: < 100MB available\n");
        result->lib_missing++;  /* 复用作为警告 */
    }

    /* ===== 5. 生成修复建议 ===== */
    if (result->python_missing > 0) {
        s_off += safe_snprintf(suggestions + s_off, 256 - s_off,
            "Fix Python modules: make install_python_script\n");
    }
    if (result->cmd_missing > 0) {
        s_off += safe_snprintf(suggestions + s_off, 256 - s_off,
            "Fix commands: apt install curl tar gzip coreutils\n");
    }
    if (result->lib_missing > 0) {
        s_off += safe_snprintf(suggestions + s_off, 256 - s_off,
            "Fix libraries: apt install libcurl4 libmicrohttpd-dev libnotcurses-dev\n");
    }
    if (result->python_missing + result->cmd_missing + result->lib_missing == 0) {
        s_off += safe_snprintf(suggestions + s_off, 64, "All dependencies satisfied ✓");
    }

    LOG_INFO_T("DependencyCheck", "Complete", "OK", 
               "missing: Python=%d, cmd=%d, lib=%d",
               result->python_missing, result->cmd_missing, result->lib_missing);
    return (result->python_missing + result->cmd_missing + result->lib_missing > 0) ? -1 : 0;
}