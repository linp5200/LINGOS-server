/**
 * @file    update_dev_mode.c
 * @brief   调试版本号（LINGOS_DEV_MODE=1 启用）
 * @version LN-B-5.0.0.0
 * @par     核心协议：契约式编程（仅开发者模式启用）
 * @changes 修正 LOG_DEBUG_T 参数类型
 */

#include "update_dev_mode.h"
#include "core/version.h"
#include "common/error_report.h"
#include "common/safe_string.h"
#include "common/lang.h"
#include "lib/log_extra.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* ============================================================
 * 检查是否为开发者模式
 * ============================================================ */

int update_dev_mode_is_enabled(void) {
    const char *env = getenv("LINGOS_DEV_MODE");
    int result = (env && strcmp(env, "1") == 0) ? 1 : 0;
    char buf[8];
    snprintf(buf, sizeof(buf), "%d", result);
    LOG_DEBUG_T("UpdateDev", "IsEnabled", "result=%s", buf);
    return result;
}

/* ============================================================
 * 设置调试版本号
 * ============================================================ */

int update_dev_mode_set_version(const char *version) {
    LOG_INFO_T("UpdateDev", "SetVersion", "Enter", "version=%s", version ? version : "(null)");

    if (!update_dev_mode_is_enabled()) {
        LOG_WARN_T("UpdateDev", "SetVersion", "Disabled", tr("developer mode not enabled", "开发者模式未启用"));
        return -1;
    }

    if (!version || !*version) {
        LOG_ERROR_T("UpdateDev", "SetVersion", "Invalid", "version is NULL");
        return -1;
    }

    if (version_set(version) != 0) {
        LOG_ERROR_T("UpdateDev", "SetVersion", "Fail", "version_set failed");
        return -1;
    }

    LOG_INFO_T("UpdateDev", "SetVersion", "OK", "version set to %s (dev mode)", version);
    return 0;
}

/* ============================================================
 * 获取开发者模式剩余时间
 * ============================================================ */

long update_dev_mode_get_remaining(void) {
    if (!update_dev_mode_is_enabled()) {
        return 0;
    }

    static time_t start_time = 0;
    if (start_time == 0) {
        start_time = time(NULL);
    }

    long elapsed = time(NULL) - start_time;
    long remaining = 86400 - elapsed;
    if (remaining < 0) remaining = 0;

    char buf[32];
    snprintf(buf, sizeof(buf), "%ld", remaining);
    LOG_DEBUG_T("UpdateDev", "GetRemaining", "remaining=%s", buf);
    return remaining;
}