/**
 * @file    src/config/config_debug.h
 * @brief   配置调试模块声明
 * @version LN-B-5.1.2.6-rc
 * @par     核心协议：C1, C3
 * @changes 新增调试函数声明；定义 CONFIG_CUT_UP/DOWN 常量。
 */

#ifndef CONFIG_DEBUG_H
#define CONFIG_DEBUG_H

#include "config_renderer.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================
 * 常量定义
 * ============================================================ */
#define CONFIG_CUT_UP     1
#define CONFIG_CUT_DOWN   0

/* ============================================================
 * 调试函数
 * ============================================================ */

/**
 * @brief 执行指定渲染器的自检测试
 * @param mode 渲染器类型（RENDERER_TYPE_TUI/CLI/RAW）
 * @return 0 成功，-1 失败
 */
int config_debug_test(renderer_type_t mode);

/**
 * @brief 模拟渲染器升降级
 * @param mode 当前渲染器类型
 * @param direction CONFIG_CUT_DOWN 降级，CONFIG_CUT_UP 升级
 * @return 0 成功，-1 失败
 */
int config_debug_cut(renderer_type_t mode, int direction);

/**
 * @brief 获取可读的渲染器名称（带前缀）
 * @param mode 渲染器类型
 * @return 静态字符串（如 "TUI", "CLI", "RAW"）
 */
const char* config_debug_mode_name(renderer_type_t mode);

#ifdef __cplusplus
}
#endif

#endif /* CONFIG_DEBUG_H */