/**
 * @file    shadow_mode.c
 * @brief   影子模式核心实现（非必要权限返回空数据）
 * @version LN-B-5.0.0.0
 */

#include "defense_mode.h"
#include "security_config.h"
#include "../common/safe_string.h"
#include "../lib/log_extra.h"
#include <string.h>
#include <pthread.h>

static pthread_mutex_t g_shadow_lock = PTHREAD_MUTEX_INITIALIZER;

/* ============================================================
 * 判断是否为必要权限（影子模式白名单）
 * ============================================================ */

static int is_required_permission(const char *perm) {
    if (!perm) return 0;

    /* 系统核心路径（始终允许） */
    const char *required[] = {
        "/LINGOS/system/config/ai_config.json",
        "/LINGOS/version",
        "/LINGOS/system/config/startup.conf",
        NULL
    };

    for (int i = 0; required[i]; i++) {
        if (strstr(perm, required[i]) != NULL) {
            return 1;
        }
    }

    /* 系统核心技能 */
    const char *required_skills[] = {
        "file_read:system_config",
        NULL
    };

    for (int i = 0; required_skills[i]; i++) {
        if (strstr(perm, required_skills[i]) != NULL) {
            return 1;
        }
    }

    return 0;
}

/* ============================================================
 * 影子模式检查：是否应返回空数据
 * ============================================================ */

int shadow_mode_should_return_empty(const char *app_id, const char *perm) {
    if (!app_id || !perm) return 0;

    /* 检查当前是否为影子模式 */
    defense_mode_t mode = defense_mode_get();
    if (mode != DEFENSE_MODE_SHADOW) {
        return 0;
    }

    /* 检查应用是否被排除 */
    const security_config_t *cfg = security_config_get();
    if (cfg) {
        for (int i = 0; i < cfg->shadow_excluded_count; i++) {
            if (strcmp(app_id, cfg->shadow_excluded_apps[i]) == 0) {
                return 0;  /* 排除应用不返回空 */
            }
        }
    }

    /* 必要权限不返回空 */
    if (is_required_permission(perm)) {
        return 0;
    }

    /* 检查权限是否在影子模式权限列表中 */
    if (cfg) {
        for (int i = 0; i < cfg->shadow_perm_count; i++) {
            if (strcmp(perm, cfg->shadow_permissions[i]) == 0) {
                LOG_DEBUG_T("ShadowMode", "Check", "ReturnEmpty",
                            "app='%s', perm='%s' -> returning empty", app_id, perm);
                return 1;
            }
        }
    }

    return 0;
}

/* ============================================================
 * 获取空数据（根据类型返回对应空值）
 * ============================================================ */

const char* shadow_mode_get_empty_data(const char *type) {
    if (!type) return "";

    if (strcmp(type, "string") == 0 || strcmp(type, "text") == 0) {
        return "";
    }

    if (strcmp(type, "list") == 0 || strcmp(type, "array") == 0) {
        return "[]";
    }

    if (strcmp(type, "object") == 0 || strcmp(type, "json") == 0) {
        return "{}";
    }

    if (strcmp(type, "number") == 0 || strcmp(type, "int") == 0) {
        return "0";
    }

    return "";
}