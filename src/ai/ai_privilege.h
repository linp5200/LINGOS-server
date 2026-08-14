/**
 * @file    src/ai/ai_privilege.h
 * @brief   AI 权限管理接口
 * @version LN-B-5.1.2.6-rc
 * @par     核心协议：C1, C3, AI-CTL
 * @changes 新增 AI 权限分级接口
 */

#ifndef AI_PRIVILEGE_H
#define AI_PRIVILEGE_H

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================
 * 权限等级定义
 * ============================================================ */
#define PRIVILEGE_LEVEL_L0    0   /* 只读 - 自动授权 */
#define PRIVILEGE_LEVEL_L1    1   /* 常规 - 自动授权（有时间限制） */
#define PRIVILEGE_LEVEL_L2    2   /* 高影响 - 默认需确认 */
#define PRIVILEGE_LEVEL_L3    3   /* 危险 - 确认 + 可配置二次验证 */

/* ============================================================
 * 权限请求结果
 * ============================================================ */
#define PRIVILEGE_RESULT_GRANTED     0   /* 已授权 */
#define PRIVILEGE_RESULT_DENIED      1   /* 已拒绝 */
#define PRIVILEGE_RESULT_NEED_AUTH   2   /* 需要用户确认 */
#define PRIVILEGE_RESULT_ERROR       -1  /* 内部错误 */

/* ============================================================
 * AI 权限 API
 * ============================================================ */

/**
 * @brief 初始化 AI 权限系统
 * @return 0 成功，-1 失败
 */
int ai_privilege_init(void);

/**
 * @brief 请求授权（AI 调用）
 * @param skill_name 技能名称
 * @param reason 请求原因（用于日志和用户显示）
 * @param out_level 输出授权的权限等级
 * @return PRIVILEGE_RESULT_* 常量
 */
int ai_privilege_request(const char *skill_name, const char *reason, int *out_level);

/**
 * @brief 授予权限（用户确认后）
 * @param skill_name 技能名称
 * @param level 权限等级
 * @param duration_sec 持续时间（秒），0 表示永久
 * @return 0 成功，-1 失败
 */
int ai_privilege_grant(const char *skill_name, int level, int duration_sec);

/**
 * @brief 撤销权限
 * @param skill_name 技能名称
 * @return 0 成功，-1 失败
 */
int ai_privilege_revoke(const char *skill_name);

/**
 * @brief 检查权限（核心检查函数）
 * @param skill_name 技能名称
 * @return 1 已授权，0 未授权
 */
int ai_privilege_check(const char *skill_name);

/**
 * @brief 获取技能所需权限等级
 * @param skill_name 技能名称
 * @return 权限等级（PRIVILEGE_LEVEL_L0-L3）
 */
int ai_privilege_get_level(const char *skill_name);

/**
 * @brief 检查技能是否为高风险（L2/L3）
 * @param skill_name 技能名称
 * @return 1 高风险，0 低风险
 */
int ai_privilege_is_high_risk(const char *skill_name);

/**
 * @brief 获取权限状态字符串
 * @param skill_name 技能名称
 * @return 状态字符串（静态），"unknown" 表示未知
 */
const char* ai_privilege_get_status(const char *skill_name);

/**
 * @brief 重新加载配置
 * @return 0 成功，-1 失败
 */
int ai_privilege_reload(void);

/**
 * @brief 获取缓存统计
 * @param cache_count 输出缓存条目数
 * @param rule_count 输出规则数
 */
void ai_privilege_get_stats(int *cache_count, int *rule_count);

#ifdef __cplusplus
}
#endif

#endif /* AI_PRIVILEGE_H */