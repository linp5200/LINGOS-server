/**
 * @file    second_verify.h
 * @brief   敏感操作二次验证（密码 + Y/N 双重确认）
 * @version LN-B-4.2.0.0
 */

#ifndef SECURITY_VERIFY_SECOND_VERIFY_H
#define SECURITY_VERIFY_SECOND_VERIFY_H

#include <stdint.h>

/* ============================================================
 * 验证结果枚举
 * ============================================================ */

typedef enum {
    VERIFY_RESULT_DENIED = 0,      /* 拒绝 */
    VERIFY_RESULT_APPROVED = 1,    /* 批准 */
    VERIFY_RESULT_TIMEOUT = 2,     /* 超时 */
    VERIFY_RESULT_CANCELLED = 3    /* 用户取消 */
} verify_result_t;

/* ============================================================
 * 验证模式枚举
 * ============================================================ */

typedef enum {
    VERIFY_MODE_PASSWORD_ONLY = 0,   /* 仅密码（不推荐） */
    VERIFY_MODE_PASSWORD_AND_YN = 1  /* 密码 + Y/N 双重确认（默认） */
} verify_mode_t;

/* ============================================================
 * 公共 API
 * ============================================================ */

/**
 * @brief 初始化二次验证系统
 * @return 0 成功，-1 失败
 */
int second_verify_init(void);

/**
 * @brief 执行二次验证（密码 + Y/N 双重确认）
 * @param operation 操作名称（用于提示）
 * @param mode 验证模式
 * @param timeout_sec 超时秒数
 * @return 验证结果
 */
verify_result_t second_verify_check(const char *operation, verify_mode_t mode, int timeout_sec);

/**
 * @brief 简化版二次验证（使用默认模式和超时）
 * @param operation 操作名称
 * @return 验证结果
 */
verify_result_t second_verify_quick(const char *operation);

/**
 * @brief 检查 root 密码是否已设置
 * @return 1 已设置，0 未设置
 */
int second_verify_has_password(void);

/**
 * @brief 设置 root 密码
 * @param password 新密码（明文，调用后自动清除）
 * @return 0 成功，-1 失败
 */
int second_verify_set_password(const char *password);

/**
 * @brief 验证密码是否正确
 * @param password 待验证密码
 * @return 1 正确，0 错误，-1 错误
 */
int second_verify_check_password(const char *password);

/**
 * @brief 获取验证模式名称
 * @param mode 模式枚举
 * @return 名称字符串
 */
const char* second_verify_mode_name(verify_mode_t mode);

/**
 * @brief 获取验证结果名称
 * @param result 结果枚举
 * @return 名称字符串
 */
const char* second_verify_result_str(verify_result_t result);

#endif /* SECURITY_VERIFY_SECOND_VERIFY_H */