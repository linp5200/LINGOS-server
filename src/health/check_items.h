/**
 * @file    src/health/check_items.h
 * @brief   具体检查项声明（语言、依赖、硬件、配置、网络、权限、版本）
 * @version LN-0.4.3
 * @par     核心协议：C1, C-C
 * @changes 对齐 check_manager 接口模型：
 *          - 检查函数改为无参、返回 check_result_t（0/1/2/4）
 *          - 结果消息通过 check_cache_set() 存储
 *          - check_items_register_all() 返回 int
 */

#ifndef HEALTH_CHECK_ITEMS_H
#define HEALTH_CHECK_ITEMS_H

#include "check_manager.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 注册所有内置检查项到管理器
 * @return 0 成功，-1 失败
 */
int check_items_register_all(void);

/**
 * @brief 语言检测检查项（检测系统语言，预选语言）
 * @return CHECK_RESULT_PASS / WARN / FAIL
 */
int check_item_language(void);

/**
 * @brief 依赖检查项（检测系统库、Python 模块等）
 * @return CHECK_RESULT_PASS / WARN / FAIL
 */
int check_item_dependencies(void);

/**
 * @brief 硬件资源检查项（内存、磁盘、CPU）
 * @return CHECK_RESULT_PASS / WARN / FAIL
 */
int check_item_hardware(void);

/**
 * @brief 配置完整性检查项（配置文件是否存在、是否有效）
 * @return CHECK_RESULT_PASS / WARN / FAIL
 */
int check_item_config(void);

/**
 * @brief 网络检查项（DNS、镜像源可达性）
 * @return CHECK_RESULT_PASS / WARN / FAIL
 */
int check_item_network(void);

/**
 * @brief 权限检查项（目录可写）
 * @return CHECK_RESULT_PASS / WARN / FAIL
 */
int check_item_permissions(void);

/**
 * @brief 版本检查项（版本一致性）
 * @return CHECK_RESULT_PASS / WARN / FAIL
 */
int check_item_version(void);

#ifdef __cplusplus
}
#endif

#endif /* HEALTH_CHECK_ITEMS_H */
