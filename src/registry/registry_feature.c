/**
 * @file    registry_feature.c
 * @brief   功能特性注册与开关控制
 * @version LN-B-5.0.0.0
 */

#include "registry.h"
#include "../common/safe_string.h"
#include "../lib/log_extra.h"
#include "../lib/cJSON/cJSON.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>

static pthread_mutex_t g_feature_lock = PTHREAD_MUTEX_INITIALIZER;

/**
 * @brief 注册一个功能特性
 * @param id 特性 ID，如 "feature:shadow_mode"
 * @param name 显示名称
 * @param default_state 默认启用状态
 * @param description 描述
 * @return 0 成功，-1 失败
 */
int registry_feature_register(const char *id, const char *name, int default_state, const char *description) {
    LOG_INFO_T("RegistryFeature", "Register", "Enter", "id='%s'", id ? id : "(null)");

    if (!id || !*id) {
        LOG_ERROR_T("RegistryFeature", "Register", "Invalid", "id is NULL");
        return -1;
    }

    registry_entry_t entry;
    memset(&entry, 0, sizeof(entry));
    safe_strncpy(entry.id, id, sizeof(entry.id));
    entry.type = REG_TYPE_FEATURE;
    safe_strncpy(entry.name, name ? name : id, sizeof(entry.name));
    safe_strncpy(entry.version, "1.0.0", sizeof(entry.version));
    entry.status = default_state ? REG_STATUS_ACTIVE : REG_STATUS_INACTIVE;

    /* 存储描述到 metadata (JSON) */
    cJSON *meta = cJSON_CreateObject();
    cJSON_AddStringToObject(meta, "description", description ? description : "");
    cJSON_AddBoolToObject(meta, "default", default_state);
    entry.metadata = (void*)meta;

    int ret = registry_register(&entry);
    if (ret != 0) cJSON_Delete(meta);
    return ret;
}

/**
 * @brief 获取功能特性状态
 * @param id 特性 ID
 * @return 1 启用，0 禁用，-1 未找到
 */
int registry_feature_is_enabled(const char *id) {
    const registry_entry_t *entry = registry_get(id);
    if (!entry) return -1;
    return (entry->status == REG_STATUS_ACTIVE) ? 1 : 0;
}

/**
 * @brief 设置功能特性状态
 * @param id 特性 ID
 * @param enabled 启用/禁用
 * @return 0 成功，-1 失败
 */
int registry_feature_set(const char *id, int enabled) {
    LOG_INFO_T("RegistryFeature", "Set", "Enter", "id='%s', enabled=%d", id ? id : "(null)", enabled);

    const registry_entry_t *old = registry_get(id);
    if (!old) {
        LOG_WARN_T("RegistryFeature", "Set", "NotFound", "feature '%s' not found", id);
        return -1;
    }

    registry_entry_t updated = *old;
    updated.status = enabled ? REG_STATUS_ACTIVE : REG_STATUS_INACTIVE;
    updated.updated_at = time(NULL);

    return registry_update(id, &updated);
}

/**
 * @brief 列出所有功能特性
 */
int registry_feature_list(registry_entry_t **out, int max_count) {
    return registry_list(REG_TYPE_FEATURE, out, max_count);
}