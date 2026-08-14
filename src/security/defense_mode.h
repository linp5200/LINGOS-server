/**
 * @file    defense_mode.h
 * @brief   防御模式枚举与等级管理
 * @version LN-B-5.0.0.0
 */

#ifndef DEFENSE_MODE_H
#define DEFENSE_MODE_H

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================
 * 防御模式枚举（等级从低到高）
 * ============================================================ */

typedef enum {
    DEFENSE_MODE_NONE = 0,      /* 无模式（所有功能可用） */
    DEFENSE_MODE_SHADOW = 1,    /* 影子模式（最低） */
    DEFENSE_MODE_DARK = 2,      /* 暗影模式（中等） */
    DEFENSE_MODE_ABSOLUTE = 3   /* 绝对保护（最高） */
} defense_mode_t;

/* ============================================================
 * API
 * ============================================================ */

/**
 * @brief 获取当前防御模式
 * @return 当前模式枚举值
 */
defense_mode_t defense_mode_get(void);

/**
 * @brief 设置防御模式（带等级覆盖检查）
 * @param mode 目标模式
 * @return 0 成功，-1 失败（尝试从高级降级到低级会失败）
 */
int defense_mode_set(defense_mode_t mode);

/**
 * @brief 检查模式 A 是否比模式 B 等级更高
 * @param a 模式 A
 * @param b 模式 B
 * @return 1 如果 A > B，0 否则
 */
int defense_mode_is_higher(defense_mode_t a, defense_mode_t b);

/**
 * @brief 应用当前防御模式（从 security.json 加载并生效）
 * @return 0 成功，-1 失败
 */
int defense_mode_apply_current(void);

/**
 * @brief 获取模式名称字符串
 * @param mode 模式枚举
 * @return 名称字符串
 */
const char* defense_mode_name(defense_mode_t mode);

/**
 * @brief 获取模式等级数字（1-3）
 * @param mode 模式枚举
 * @return 等级数字
 */
int defense_mode_level(defense_mode_t mode);

int defense_mode_apply_current(void);

#ifdef __cplusplus
}
#endif

#endif /* DEFENSE_MODE_H */