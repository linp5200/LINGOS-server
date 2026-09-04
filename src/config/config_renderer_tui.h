/**
 * @file    src/config/config_renderer_tui.h
 * @brief   TUI 渲染器头文件
 * @version LN-0.4.3
 * @par     核心协议：C1, C-C
 */

#ifndef CONFIG_RENDERER_TUI_H
#define CONFIG_RENDERER_TUI_H

#include "config_renderer.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 检测 TUI 是否可用
 * @param force 是否强制使用（跳过检测）
 * @return 1 可用，0 不可用
 */
int renderer_tui_available(int force);

/**
 * @brief 运行 TUI 渲染测试（多元素验证）
 * @param details 输出详细测试结果（可为 NULL）
 * @return 0 全部通过，1 轻微偏差，2 严重偏差，-1 失败
 */
int renderer_tui_test(char *details, size_t details_size);

/**
 * @brief 创建 TUI 渲染器实现
 * @param ctx 渲染器上下文
 * @return 0 成功，-1 失败
 */
int renderer_tui_impl_create(renderer_ctx_t *ctx);

#ifdef __cplusplus
}
#endif

#endif /* CONFIG_RENDERER_TUI_H */