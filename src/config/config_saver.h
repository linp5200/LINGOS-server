/**
 * @file    src/config/config_saver.h
 * @brief   配置保存（三级降级：重试 → 重建 → 脚本）
 * @version LN-0.4.3
 * @par     核心协议：C1, C-C
 */

#ifndef CONFIG_SAVER_H
#define CONFIG_SAVER_H

#include "config_core.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 保存配置（三级降级）
 * @param cfg 配置结构
 * @return 0 成功，-1 失败
 */
int config_saver_save(const wizard_config_t *cfg);

/**
 * @brief 生成修复脚本
 * @param cfg 配置结构
 * @param script_path 输出脚本路径
 * @param script_path_size 缓冲区大小
 * @return 0 成功，-1 失败
 */
int config_saver_generate_script(const wizard_config_t *cfg, char *script_path, size_t script_path_size);

#ifdef __cplusplus
}
#endif

#endif /* CONFIG_SAVER_H */