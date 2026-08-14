/**
 * @file    security_config.h
 * @brief   安全配置数据结构声明与 API
 * @version LN-B-5.0.0.0
 */

#ifndef SECURITY_CONFIG_H
#define SECURITY_CONFIG_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================
 * 安全配置结构
 * ============================================================ */

typedef struct {
    char version[16];               /* 配置文件版本 */
    char input_mode[16];            /* strict | balanced | permissive */

    /* 影子模式 */
    int shadow_enabled;
    char shadow_permissions[8][32];
    int shadow_perm_count;
    char shadow_excluded_apps[4][64];
    int shadow_excluded_count;

    /* 暗影模式 */
    int dark_enabled;
    int dark_simulate_hardware_disable;
    char dark_blocked_features[8][32];
    int dark_blocked_count;

    /* 绝对保护 */
    int absolute_enabled;
    char absolute_trigger[16];      /* auto | manual | both */
    int absolute_auto_close_interval;
    int absolute_block_all_external_input;
    int absolute_block_infected_internal_input;

    /* 行为监控 */
    int behavior_enabled;
    int behavior_window_size;
    int behavior_threshold;
    int behavior_auto_escalate;
} security_config_t;

/* ============================================================
 * API
 * ============================================================ */

/**
 * @brief 加载安全配置（若文件不存在则使用默认值）
 * @return 0 成功，-1 失败
 */
int security_config_load(void);

/**
 * @brief 保存当前安全配置到文件
 * @return 0 成功，-1 失败
 */
int security_config_save(void);

/**
 * @brief 重置为默认配置（覆盖当前内存配置，不自动保存）
 * @return 0 成功，-1 失败
 * @note 调用后需手动调用 security_config_save() 持久化
 */
int security_config_set_defaults(void);

/**
 * @brief 验证并修正配置（检查字段有效性）
 * @param cfg 配置结构指针
 * @return 0 有效，-1 有无效字段（已自动修正）
 */
int security_config_validate(security_config_t *cfg);

/**
 * @brief 获取当前配置（只读）
 * @return 配置结构指针
 */
const security_config_t* security_config_get(void);

/**
 * @brief 从 cJSON 对象加载配置（供 config_loader 热重载）
 * @param root cJSON 根对象指针
 * @return 0 成功，-1 失败
 */
int security_config_load_from_json(const void *root);

/**
 * @brief 热重载通知回调（供 config_loader 使用）
 */
void security_config_reload_notify(void);

/* 模式切换辅助 */
int security_config_set_input_mode(const char *mode);
int security_config_set_shadow_enabled(int enabled);
int security_config_set_dark_enabled(int enabled);
int security_config_set_absolute_enabled(int enabled);

#ifdef __cplusplus
}
#endif

#endif /* SECURITY_CONFIG_H */