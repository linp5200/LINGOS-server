/**
 * @file    src/config/config_renderer_raw.h
 * @brief   RAW 渲染器头文件
 * @version LN-B-5.1.2.6-rc
 * @par     核心协议：C1, C-C
 */

#ifndef CONFIG_RENDERER_RAW_H
#define CONFIG_RENDERER_RAW_H

#include "config_renderer.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 创建 RAW 渲染器实现
 * @param ctx 渲染器上下文
 * @return 0 成功，-1 失败
 */
int renderer_raw_impl_create(renderer_ctx_t *ctx);

#ifdef __cplusplus
}
#endif

#endif /* CONFIG_RENDERER_RAW_H */