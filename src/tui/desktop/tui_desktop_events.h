/**
 * @file    tui_desktop_events.h
 * @brief   TUI 桌面事件处理头文件
 * @version LN-B-5.0.0.0
 */

#ifndef TUI_DESKTOP_TUI_DESKTOP_EVENTS_H
#define TUI_DESKTOP_TUI_DESKTOP_EVENTS_H

#include <notcurses/notcurses.h>
#include "tui_desktop.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 初始化事件系统
 */
void tui_desktop_events_init(void);

/**
 * @brief 处理键盘事件
 * @param state 桌面状态
 * @param key 按键值
 * @return 1 事件已处理，0 未处理，-1 错误
 */
int tui_desktop_events_handle_key(tui_desktop_state_t *state, int key);

/**
 * @brief 处理鼠标事件
 * @param state 桌面状态
 * @param key 按键值（NCKEY_MOUSE）
 * @param input ncinput 结构
 * @return 1 事件已处理，0 未处理，-1 错误
 */
int tui_desktop_events_handle_mouse(tui_desktop_state_t *state, int key, ncinput *input);

/**
 * @brief 更新鼠标位置
 * @param x, y 坐标
 */
void tui_desktop_events_update_mouse_position(int x, int y);

/**
 * @brief 获取鼠标 X 坐标
 */
int tui_desktop_events_get_mouse_x(void);

/**
 * @brief 获取鼠标 Y 坐标
 */
int tui_desktop_events_get_mouse_y(void);

/**
 * @brief 检查是否正在拖拽
 */
int tui_desktop_events_is_dragging(void);

/**
 * @brief 清理事件系统
 */
void tui_desktop_events_cleanup(void);

#ifdef __cplusplus
}
#endif

#endif /* TUI_DESKTOP_TUI_DESKTOP_EVENTS_H */