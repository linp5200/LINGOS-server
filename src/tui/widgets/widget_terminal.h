/**
 * @file    widget_terminal.h
 * @brief   TUI 桌面终端小部件（内嵌 Shell）
 * @version LN-B-4.2.0.0
 */

#ifndef TUI_WIDGETS_WIDGET_TERMINAL_H
#define TUI_WIDGETS_WIDGET_TERMINAL_H

#include <stddef.h>

/**
 * @brief 创建终端小部件（在聚焦窗口中启动 Shell）
 */
void widget_terminal_create(void);

/**
 * @brief 更新终端（读取子进程输出并刷新显示）
 */
void widget_terminal_update(void);

/**
 * @brief 向终端输入命令
 * @param cmd 命令字符串
 */
void widget_terminal_input(const char *cmd);

/**
 * @brief 销毁终端小部件
 */
void widget_terminal_destroy(void);

#endif /* TUI_WIDGETS_WIDGET_TERMINAL_H */