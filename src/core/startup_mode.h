/**
 * @file    startup_mode.h
 * @brief   启动模式管理（Shell / TUI Desktop）
 * @version LN-B-4.2.0.0
 */

#ifndef CORE_STARTUP_MODE_H
#define CORE_STARTUP_MODE_H

#include <stdint.h>

/* ============================================================
 * 启动模式枚举
 * ============================================================ */

typedef enum {
    STARTUP_MODE_SHELL = 0,   /* 默认进入 Shell CLI */
    STARTUP_MODE_TUI = 1      /* 默认进入 TUI 桌面 */
} startup_mode_t;

/* ============================================================
 * 公共 API
 * ============================================================ */

/**
 * @brief 获取当前启动模式
 * @return 启动模式枚举值（默认 STARTUP_MODE_SHELL）
 */
startup_mode_t startup_mode_get(void);

/**
 * @brief 设置启动模式（写入配置文件）
 * @param mode 启动模式
 * @return 0 成功，-1 失败
 */
int startup_mode_set(startup_mode_t mode);

/**
 * @brief 显示当前启动模式到终端
 */
void startup_mode_show(void);

/**
 * @brief 获取启动模式名称字符串
 * @param mode 启动模式
 * @return 名称字符串（"shell" 或 "tui"）
 */
const char* startup_mode_name(startup_mode_t mode);

#endif /* CORE_STARTUP_MODE_H */