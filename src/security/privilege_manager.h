/**
 * @file    privilege_manager.h
 * @brief   权限管理（开发者模式）接口声明
 * @version LN-B-5.0.0.0
 */

#ifndef SECURITY_PRIVILEGE_MANAGER_H
#define SECURITY_PRIVILEGE_MANAGER_H

#include <stddef.h>   /* 添加此行，定义 size_t */

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 启用或禁用开发者模式
 * @param enable 1=启用，0=禁用
 * @return 0 成功，-1 失败
 */
int privilege_set_developer(int enable);

/**
 * @brief 检查并自动恢复（24小时超时回退）
 * @return 0 无需恢复，1 已恢复，-1 错误
 */
int privilege_check_and_auto_revert(void);

/**
 * @brief 获取开发者模式剩余秒数
 * @return 剩余秒数，0 表示未启用或已过期
 */
long privilege_get_remaining_seconds(void);

/**
 * @brief 检查系统是否被锁定（3次重启失败后）
 * @return 1 锁定，0 未锁定
 */
int privilege_is_locked(void);

/**
 * @brief 加载权限配置（从 /LINGOS/system/config/privilege.json）
 * @return 0 成功，-1 失败
 */
int privilege_config_load(void);

/**
 * @brief 保存权限配置
 * @return 0 成功，-1 失败
 */
int privilege_config_save(void);

/**
 * @brief 获取当前权限模式字符串（如 "default" 或 "developer"）
 * @param mode_buf 输出缓冲区
 * @param buf_len 缓冲区大小
 * @return 0 成功，-1 失败
 */
int privilege_get_mode(char *mode_buf, size_t buf_len);

#ifdef __cplusplus
}
#endif

#endif /* SECURITY_PRIVILEGE_MANAGER_H */