/**
 * @file    src/core/version.c
 * @brief   版本号读取/写入/确保（版本文件只读，仅由 system update 写入）
 * @version LN-B-5.1.2.6-rc
 * @changes 版本号更新至 LN-B-5.1.2.6-rc；
 *          version_ensure() 自动修复版本不匹配
 */

#include "version.h"
#include "data_path.h"
#include "log_extra.h"
#include "safe_string.h"
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>

/* ============================================================
 * 版本宏
 * ============================================================ */

#ifndef LINGOS_VERSION
#define LINGOS_VERSION "LN-B-5.1.2.6-rc"
#endif

/* ============================================================
 * 内部辅助：获取版本文件路径
 * ============================================================ */

static const char* get_version_file_path(void) {
    static char path[512];
    if (path[0] == '\0') {
        const char *root = lingos_data_root();
        safe_snprintf(path, sizeof(path), "%s/version", root);
    }
    return path;
}

/* ============================================================
 * 公共 API 实现
 * ============================================================ */

const char *version_get(void) {
    LOG_DEBUG_T("Version", "Get", "enter", "Reading version");
    static char cached_version[64] = {0};
    const char *path = get_version_file_path();

    FILE *fp = fopen(path, "r");
    if (!fp) {
        LOG_DEBUG_T("Version", "Get", "NoFile", "version file not found, using built-in: %s", LINGOS_VERSION);
        safe_strncpy(cached_version, LINGOS_VERSION, sizeof(cached_version));
        return cached_version;
    }

    if (fgets(cached_version, sizeof(cached_version), fp)) {
        char *nl = strchr(cached_version, '\n');
        if (nl) *nl = '\0';
        LOG_DEBUG_T("Version", "Get", "OK", "read version: %s", cached_version);
    } else {
        LOG_WARN_T("Version", "Get", "ReadFail", "fgets failed, using built-in: %s", LINGOS_VERSION);
        safe_strncpy(cached_version, LINGOS_VERSION, sizeof(cached_version));
    }
    fclose(fp);
    return cached_version;
}

int version_set(const char *new_version) {
    LOG_INFO_T("Version", "Set", "Enter", "new_version='%s'", new_version ? new_version : "(null)");

    if (!new_version || !*new_version) {
        LOG_ERROR_T("Version", "Set", "Invalid", "new_version is NULL or empty");
        return -1;
    }

    const char *path = get_version_file_path();
    FILE *fp = fopen(path, "w");
    if (!fp) {
        LOG_ERROR_T("Version", "Set", "OpenFail", "cannot open %s for writing: %s (errno=%d)",
                    path, strerror(errno), errno);
        return -1;
    }

    fprintf(fp, "%s\n", new_version);
    fclose(fp);
    LOG_INFO_T("Version", "Set", "OK", "version set to %s", new_version);
    return 0;
}

int version_ensure(void) {
    LOG_DEBUG_T("Version", "Ensure", "Enter", "Ensuring version consistency");

    const char *current = version_get();
    LOG_DEBUG_T("Version", "Ensure", "Current", "current='%s', built-in='%s'", current, LINGOS_VERSION);

    if (strcmp(current, LINGOS_VERSION) != 0) {
        LOG_WARN_T("Version", "Ensure", "Mismatch",
                   "version file=%s, built-in=%s. Auto-fixing...",
                   current, LINGOS_VERSION);

        /* 【新增】自动修复版本不匹配 */
        if (version_set(LINGOS_VERSION) == 0) {
            LOG_INFO_T("Version", "Ensure", "Fixed", "version updated to %s", LINGOS_VERSION);
            return 0;
        } else {
            LOG_ERROR_T("Version", "Ensure", "FixFail", "failed to update version file");
            return -1;
        }
    } else {
        LOG_DEBUG_T("Version", "Ensure", "OK", "version file and built-in match: %s", current);
        return 0;
    }
}