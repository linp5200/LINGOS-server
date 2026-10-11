/**
 * @file    src/config/config_renderer_clack.h
 * @brief   Clack 风格配置渲染器头文件（先生 2026-10-01 方案 A 定稿）
 * @version LN-0.8.2
 * @par     核心协议：C1, C-C
 */

#ifndef CONFIG_RENDERER_CLACK_H
#define CONFIG_RENDERER_CLACK_H

#include "config_renderer.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 创建 Clack 渲染器实现
 * @param ctx 渲染器上下文
 * @return 0 成功，-1 失败
 */
int renderer_clack_impl_create(renderer_ctx_t *ctx);

/**
 * @brief 检测 Clack 是否可用（真终端）
 * @return 1 可用，0 不可用
 */
int renderer_clack_available(void);

#ifdef __cplusplus
}
#endif

#endif /* CONFIG_RENDERER_CLACK_H */
