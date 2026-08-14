/**
 * @file    src/health/check_cache.h
 * @brief   自检缓存管理（避免重复检查）
 * @version LN-B-5.1.2.6-rc
 * @par     核心协议：C1, CM
 */

#ifndef HEALTH_CHECK_CACHE_H
#define HEALTH_CHECK_CACHE_H

#include "check_manager.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 初始化缓存
 * @return 0 成功，-1 失败
 */
int check_cache_init(void);

/**
 * @brief 保存检查结果到缓存
 * @param summary 汇总结果
 * @return 0 成功，-1 失败
 */
int check_cache_save(const check_summary_t *summary);

/**
 * @brief 加载缓存
 * @param summary 输出汇总结果（调用者分配）
 * @return 0 成功（有缓存），-1 失败（无缓存或过期）
 */
int check_cache_load(check_summary_t *summary);

/**
 * @brief 设置单个检查项的结果（用于检查函数内部存储）
 * @param id 检查项 ID
 * @param message 结果消息
 * @param result 结果状态
 * @return 0 成功，-1 失败
 */
int check_cache_set(const char *id, const char *message, check_result_t result);

/**
 * @brief 获取单个检查项的结果
 * @param id 检查项 ID
 * @param message 输出消息缓冲区（至少256字节）
 * @param msg_size 缓冲区大小
 * @return 0 成功，-1 失败
 */
int check_cache_get(const char *id, char *message, size_t msg_size);

/**
 * @brief 检查缓存是否有效（未过期）
 * @return 1 有效，0 无效
 */
int check_cache_is_valid(void);

/**
 * @brief 使缓存无效（强制重新检查）
 */
void check_cache_invalidate(void);

#ifdef __cplusplus
}
#endif

#endif /* HEALTH_CHECK_CACHE_H */