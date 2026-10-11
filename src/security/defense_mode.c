/**
 * @file    defense_mode.c
 * @brief   防御模式状态管理
 * @version LN-B-5.0.0.0
 * @fix     允许绝对保护降级到暗影模式（用于自动关闭）
 */

#include "defense_mode.h"
#include "security_config.h"
#include "dark_mode.h"     /* 【P5 接线】模式切换副作用：敏感数据加密态（此前零调用者） */
#include "../lib/log_extra.h"
#include "../common/safe_string.h"
#include <pthread.h>
#include <string.h>

static defense_mode_t g_current_mode = DEFENSE_MODE_NONE;
static pthread_mutex_t g_mode_lock = PTHREAD_MUTEX_INITIALIZER;
static int g_force_downgrade = 0;  /* 强制降级标志（用于绝对保护自动关闭） */

const char* defense_mode_name(defense_mode_t mode) {
    switch (mode) {
        case DEFENSE_MODE_NONE:     return "none";
        case DEFENSE_MODE_SHADOW:   return "shadow";
        case DEFENSE_MODE_DARK:     return "dark";
        case DEFENSE_MODE_ABSOLUTE: return "absolute";
        default:                    return "unknown";
    }
}

int defense_mode_level(defense_mode_t mode) {
    switch (mode) {
        case DEFENSE_MODE_NONE:     return 0;
        case DEFENSE_MODE_SHADOW:   return 1;
        case DEFENSE_MODE_DARK:     return 2;
        case DEFENSE_MODE_ABSOLUTE: return 3;
        default:                    return 0;
    }
}

int defense_mode_is_higher(defense_mode_t a, defense_mode_t b) {
    return defense_mode_level(a) > defense_mode_level(b);
}

defense_mode_t defense_mode_get(void) {
    pthread_mutex_lock(&g_mode_lock);
    defense_mode_t mode = g_current_mode;
    pthread_mutex_unlock(&g_mode_lock);
    return mode;
}

int defense_mode_set(defense_mode_t mode) {
    LOG_INFO_T("DefenseMode", "Set", "Enter", "mode=%d (%s)", mode, defense_mode_name(mode));

    pthread_mutex_lock(&g_mode_lock);

    /* 检查等级覆盖规则 */
    if (defense_mode_is_higher(g_current_mode, mode) &&
        g_current_mode != DEFENSE_MODE_NONE) {
        /* 检查是否为强制降级（绝对保护自动关闭） */
        if (g_force_downgrade) {
            LOG_INFO_T("DefenseMode", "Set", "ForceDowngrade",
                       "forcing downgrade from %s to %s (auto-close)",
                       defense_mode_name(g_current_mode), defense_mode_name(mode));
            g_force_downgrade = 0;
        } else {
            pthread_mutex_unlock(&g_mode_lock);
            LOG_WARN_T("DefenseMode", "Set", "Blocked",
                       "cannot downgrade from %s to %s without force flag",
                       defense_mode_name(g_current_mode), defense_mode_name(mode));
            return -1;
        }
    }

    defense_mode_t previous = g_current_mode;
    g_current_mode = mode;
    pthread_mutex_unlock(&g_mode_lock);

    /* 【P5 接线】模式切换副作用（dark_mode.c 此前零调用者）：
     *   进入 dark/absolute → 敏感数据目录打加密标记 + 生成锁定内存密钥；
     *   从 dark/absolute 降级 → 清除标记与密钥。 */
    if (mode == DEFENSE_MODE_DARK || mode == DEFENSE_MODE_ABSOLUTE) {
        if (dark_mode_encrypt_sensitive_data() != 0) {
            LOG_WARN_T("DefenseMode", "Set", "DarkMarkFail", "sensitive data marking failed");
        }
    } else if (previous == DEFENSE_MODE_DARK || previous == DEFENSE_MODE_ABSOLUTE) {
        if (dark_mode_decrypt_data() != 0) {
            LOG_WARN_T("DefenseMode", "Set", "DarkUnmarkFail", "sensitive data unmarking skipped");
        }
    }

    /* 同步到 security.json */
    switch (mode) {
        case DEFENSE_MODE_SHADOW:
            security_config_set_shadow_enabled(1);
            security_config_set_dark_enabled(0);
            security_config_set_absolute_enabled(0);
            break;
        case DEFENSE_MODE_DARK:
            security_config_set_shadow_enabled(0);
            security_config_set_dark_enabled(1);
            security_config_set_absolute_enabled(0);
            break;
        case DEFENSE_MODE_ABSOLUTE:
            security_config_set_shadow_enabled(0);
            security_config_set_dark_enabled(0);
            security_config_set_absolute_enabled(1);
            break;
        default:
            security_config_set_shadow_enabled(0);
            security_config_set_dark_enabled(0);
            security_config_set_absolute_enabled(0);
            break;
    }

    LOG_INFO_T("DefenseMode", "Set", "OK", "mode switched to %s", defense_mode_name(mode));
    return 0;
}

/**
 * @brief 强制设置防御模式（绕过降级检查，仅用于绝对保护自动关闭）
 * @param mode 目标模式
 * @return 0 成功，-1 失败
 */
int defense_mode_set_force(defense_mode_t mode) {
    g_force_downgrade = 1;
    int ret = defense_mode_set(mode);
    /* 【P5 修复】标志未被消费（同等级/升级路径）也必须清除，
     *   否则残留会让「下一次未经授权的降级」被静默放行。 */
    g_force_downgrade = 0;
    return ret;
}

int defense_mode_apply_current(void) {
    LOG_INFO_T("DefenseMode", "ApplyCurrent", "Enter", "applying from security config");

    const security_config_t *cfg = security_config_get();
    if (!cfg) {
        LOG_ERROR_T("DefenseMode", "ApplyCurrent", "NoConfig", "security config not loaded");
        return -1;
    }

    defense_mode_t target = DEFENSE_MODE_NONE;

    if (cfg->absolute_enabled) {
        target = DEFENSE_MODE_ABSOLUTE;
    } else if (cfg->dark_enabled) {
        target = DEFENSE_MODE_DARK;
    } else if (cfg->shadow_enabled) {
        target = DEFENSE_MODE_SHADOW;
    }

    pthread_mutex_lock(&g_mode_lock);
    g_current_mode = target;
    pthread_mutex_unlock(&g_mode_lock);

    /* 【P5 接线】启动即按持久化配置进入暗影/绝对保护时，同样落地加密标记 */
    if (target == DEFENSE_MODE_DARK || target == DEFENSE_MODE_ABSOLUTE) {
        if (dark_mode_encrypt_sensitive_data() != 0) {
            LOG_WARN_T("DefenseMode", "ApplyCurrent", "DarkMarkFail", "sensitive data marking failed");
        }
    }

    LOG_INFO_T("DefenseMode", "ApplyCurrent", "OK", "applied mode: %s", defense_mode_name(target));
    return 0;
}