/**
 * @file    src/config/config_renderer_cli.h
 * @brief   CLI 渲染器头文件
 * @version LN-0.4.3
 * @par     核心协议：C1, C-C
 */

#ifndef CONFIG_RENDERER_CLI_H
#define CONFIG_RENDERER_CLI_H

#include "config_renderer.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 创建 CLI 渲染器实现
 * @param ctx 渲染器上下文
 * @return 0 成功，-1 失败
 */
int renderer_cli_impl_create(renderer_ctx_t *ctx);

#ifdef __cplusplus
}
#endif

#endif /* CONFIG_RENDERER_CLI_H */