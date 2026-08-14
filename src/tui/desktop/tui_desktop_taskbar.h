/**
 * @file    tui_desktop_taskbar.h
 * @brief   TUI 桌面任务栏头文件
 * @version LN-B-4.2.0.0
 */

#ifndef TUI_DESKTOP_TUI_DESKTOP_TASKBAR_H
#define TUI_DESKTOP_TUI_DESKTOP_TASKBAR_H

#include "tui_desktop.h"

/**
 * @brief 初始化任务栏
 */
void tui_desktop_taskbar_init(void);

/**
 * @brief 渲染任务栏
 * @param state 桌面状态
 */
void tui_desktop_taskbar_render(tui_desktop_state_t *state);

/**
 * @brief 处理任务栏鼠标事件
 * @param event_type 事件类型（0=释放, 1=按下）
 * @param mouse_x, mouse_y 鼠标坐标
 * @return 1 事件已处理，0 未处理
 */
int tui_desktop_taskbar_handle_mouse(int event_type, int mouse_x, int mouse_y);

/**
 * @brief 设置任务栏可见性
 * @param visible 1=显示，0=隐藏
 */
void tui_desktop_taskbar_set_visible(int visible);

/**
 * @brief 检查任务栏是否可见
 * @return 1=可见，0=隐藏
 */
int tui_desktop_taskbar_is_visible(void);

/**
 * @brief 更新任务栏状态（时间、窗口数等）
 */
void tui_desktop_taskbar_update(void);

/**
 * @brief 清理任务栏资源
 */
void tui_desktop_taskbar_cleanup(void);

#endif /* TUI_DESKTOP_TUI_DESKTOP_TASKBAR_H */