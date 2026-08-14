/**
 * @file    tui_desktop_render.h
 * @brief   TUI 桌面渲染函数头文件
 * @version LN-B-4.2.0.0
 */

#ifndef TUI_DESKTOP_TUI_DESKTOP_RENDER_H
#define TUI_DESKTOP_TUI_DESKTOP_RENDER_H

#include "tui_desktop.h"

/**
 * @brief 渲染桌面背景
 * @param state 桌面状态
 */
void tui_desktop_render_background(tui_desktop_state_t *state);

/**
 * @brief 渲染状态栏
 * @param state 桌面状态
 */
void tui_desktop_render_statusbar(tui_desktop_state_t *state);

/**
 * @brief 渲染任务栏
 * @param state 桌面状态
 */
void tui_desktop_render_taskbar(tui_desktop_state_t *state);

/**
 * @brief 渲染桌面图标
 * @param state 桌面状态
 */
void tui_desktop_render_icons(tui_desktop_state_t *state);

/**
 * @brief 渲染所有窗口
 * @param state 桌面状态
 */
void tui_desktop_render_windows(tui_desktop_state_t *state);

#endif /* TUI_DESKTOP_TUI_DESKTOP_RENDER_H */