/**
 * @file    src/config/config_validator.c
 * @brief   配置验证函数实现
 * @version LN-B-5.1.2.6-rc
 * @par     核心协议：C1, C-C
 */

#include "config_validator.h"
#include "../common/safe_string.h"
#include "../common/lang.h"
#include <string.h>
#include <ctype.h>

/* ============================================================
 * 验证 API Key（必须以 sk- 开头）
 * ============================================================ */
int config_validate_api_key(const char *value, char *err_msg, size_t err_size) {
    if (!value || !*value) {
        if (err_msg) safe_snprintf(err_msg, err_size,
            tr("API Key cannot be empty", "API Key 不能为空"));
        return -1;
    }
    if (strlen(value) < 3 || strncmp(value, "sk-", 3) != 0) {
        if (err_msg) safe_snprintf(err_msg, err_size,
            tr("API Key must start with 'sk-'", "API Key 必须以 'sk-' 开头"));
        return -1;
    }
    return 0;
}

/* ============================================================
 * 验证 URL（必须以 http:// 或 https:// 开头）
 * ============================================================ */
int config_validate_url(const char *value, char *err_msg, size_t err_size) {
    if (!value || !*value) {
        if (err_msg) safe_snprintf(err_msg, err_size,
            tr("URL cannot be empty", "URL 不能为空"));
        return -1;
    }
    if (strncmp(value, "http://", 7) != 0 && strncmp(value, "https://", 8) != 0) {
        if (err_msg) safe_snprintf(err_msg, err_size,
            tr("URL must start with http:// or https://", "URL 必须以 http:// 或 https:// 开头"));
        return -1;
    }
    return 0;
}

/* ============================================================
 * 验证非空
 * ============================================================ */
int config_validate_nonempty(const char *value, char *err_msg, size_t err_size) {
    if (!value || !*value) {
        if (err_msg) safe_snprintf(err_msg, err_size,
            tr("Value cannot be empty", "值不能为空"));
        return -1;
    }
    return 0;
}

/* ============================================================
 * 通用验证分发
 * ============================================================ */
int config_validate(const char *value, const char *rule, char *err_msg, size_t err_size) {
    if (!value) {
        if (err_msg) safe_snprintf(err_msg, err_size,
            tr("Invalid value", "无效值"));
        return -1;
    }

    if (!rule || !*rule) {
        // 无规则，默认非空
        return config_validate_nonempty(value, err_msg, err_size);
    }

    if (strcmp(rule, "api_key") == 0) {
        return config_validate_api_key(value, err_msg, err_size);
    }
    if (strcmp(rule, "url") == 0) {
        return config_validate_url(value, err_msg, err_size);
    }
    if (strcmp(rule, "nonempty") == 0) {
        return config_validate_nonempty(value, err_msg, err_size);
    }

    // 未知规则，默认非空
    return config_validate_nonempty(value, err_msg, err_size);
}