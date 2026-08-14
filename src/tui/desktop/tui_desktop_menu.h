/**
 * @file    tui_desktop_menu.h
 * @brief   TUI 桌面菜单系统头文件
 * @version LN-B-5.0.0.0
 */

#ifndef TUI_DESKTOP_TUI_DESKTOP_MENU_H
#define TUI_DESKTOP_TUI_DESKTOP_MENU_H

#include <notcurses/notcurses.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 初始化菜单系统
 */
void tui_desktop_menu_init(void);

/**
 * @brief 渲染顶部菜单栏
 * @param plane 菜单平面
 */
void tui_desktop_menu_render(struct ncplane *plane);

/**
 * @brief 显示顶部菜单（下拉）
 * @param index 菜单索引
 */
void tui_desktop_menu_show_top(int index);

/**
 * @brief 隐藏顶部菜单
 */
void tui_desktop_menu_hide_top(void);

/**
 * @brief 显示右键上下文菜单
 * @param x, y 鼠标坐标
 */
void tui_desktop_menu_show_context(int x, int y);

/**
 * @brief 处理菜单相关按键
 * @param key 按键值
 * @return 1 事件已处理，0 未处理
 */
int tui_desktop_menu_handle_key(int key);

/**
 * @brief 清理菜单资源
 */
void tui_desktop_menu_cleanup(void);

#ifdef __cplusplus
}
#endif

#endif /* TUI_DESKTOP_TUI_DESKTOP_MENU_H */