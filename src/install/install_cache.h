/**
 * @file    src/install/install_cache.h
 * @brief   离线缓存管理
 * @version LN-0.4.3
 * @par     核心协议：C1, CM
 */

#ifndef INSTALL_CACHE_H
#define INSTALL_CACHE_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 检查包是否在缓存中
 * @param pkg_name 包名称
 * @param type 类型 ("system" 或 "python" 或 "model")
 * @return 1 在缓存中，0 不在
 */
int install_cache_has(const char *pkg_name, const char *type);

/**
 * @brief 添加包到缓存
 * @param pkg_name 包名称
 * @param type 类型 ("system" 或 "python" 或 "model")
 * @return 0 成功，-1 失败
 */
int install_cache_add(const char *pkg_name, const char *type);

/**
 * @brief 从缓存移除包
 * @param pkg_name 包名称
 * @param type 类型 ("system" 或 "python" 或 "model")
 * @return 0 成功，-1 失败
 */
int install_cache_remove(const char *pkg_name, const char *type);

/**
 * @brief 清空所有缓存
 */
void install_cache_clear_all(void);

/**
 * @brief 加载缓存到内存
 * @return 0 成功，-1 失败
 */
int install_cache_load(void);

/**
 * @brief 保存缓存到磁盘
 * @return 0 成功，-1 失败
 */
int install_cache_save(void);

#ifdef __cplusplus
}
#endif

#endif /* INSTALL_CACHE_H */