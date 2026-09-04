/**
 * @file    src/config/wizard_step_defs.h
 * @brief   向导步骤定义加载器（动态模块 → JSON → 内置）
 * @version LN-0.4.3
 * @par     核心协议：C1, C-C
 */

#ifndef CONFIG_WIZARD_STEP_DEFS_H
#define CONFIG_WIZARD_STEP_DEFS_H

#include "wizard_engine.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 从动态模块加载步骤定义
 * @param path 模块路径（如 /LINGOS/plugins/wizard/step_module.so）
 * @param steps 输出步骤数组（调用者需释放）
 * @param count 输出步骤数量
 * @return 0 成功，-1 失败
 */
int wizard_step_defs_load_module(const char *path, wizard_step_def_t **steps, int *count);

/**
 * @brief 从 JSON 文件加载步骤定义
 * @param path JSON 文件路径
 * @param steps 输出步骤数组（调用者需释放）
 * @param count 输出步骤数量
 * @return 0 成功，-1 失败
 */
int wizard_step_defs_load_json(const char *path, wizard_step_def_t **steps, int *count);

/**
 * @brief 获取内置（硬编码）步骤定义
 * @param steps 输出步骤数组（指向静态数据，无需释放）
 * @param count 输出步骤数量
 * @return 0 成功，-1 失败
 */
int wizard_step_defs_get_builtin(wizard_step_def_t **steps, int *count);

/**
 * @brief 释放动态加载的步骤定义
 * @param steps 步骤数组
 * @param count 步骤数量
 */
void wizard_step_defs_free(wizard_step_def_t *steps, int count);

/**
 * @brief 根据步骤 ID 查找步骤
 * @param steps 步骤数组
 * @param count 步骤数量
 * @param id 步骤 ID
 * @return 步骤指针，未找到返回 NULL
 */
wizard_step_def_t* wizard_step_defs_find(wizard_step_def_t *steps, int count, const char *id);

#ifdef __cplusplus
}
#endif

#endif /* CONFIG_WIZARD_STEP_DEFS_H */