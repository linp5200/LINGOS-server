/**
 * @file    tui_desktop_icons.h
 * @brief   TUI 桌面图标管理头文件
 * @version LN-B-5.0.0.0
 * @changes 添加 tui_desktop_icons_cleanup 声明
 */

#ifndef TUI_DESKTOP_TUI_DESKTOP_ICONS_H
#define TUI_DESKTOP_TUI_DESKTOP_ICONS_H

#include "tui_desktop.h"

/* 图标结构（前向声明） */
typedef struct tui_icon tui_icon_t;

/**
 * @brief 初始化桌面图标
 */
void tui_desktop_icons_init(void);

/**
 * @brief 渲染所有图标
 * @param state 桌面状态
 */
void tui_desktop_icons_render(tui_desktop_state_t *state);

/**
 * @brief 命中检测（判断鼠标位置是否在图标上）
 * @param mouse_x, mouse_y 鼠标坐标
 * @return 命中的图标指针，未命中返回 NULL
 */
tui_icon_t* tui_desktop_icons_hit_test(int mouse_x, int mouse_y);

/**
 * @brief 处理鼠标事件（点击、悬停）
 * @param event_type 事件类型（0=释放, 1=按下）
 * @param mouse_x, mouse_y 鼠标坐标
 * @return 1 事件已处理，0 未处理
 */
int tui_desktop_icons_handle_mouse(int event_type, int mouse_x, int mouse_y);

/**
 * @brief 获取图标数量
 * @return 图标总数
 */
int tui_desktop_icons_count(void);

/**
 * @brief 动态添加图标
 * @param label 图标标签
 * @param icon_char 图标字符（如 "📁"）
 * @param cmd 点击执行的命令
 * @param x, y 位置坐标
 */
void tui_desktop_icons_add(const char *label, const char *icon_char, const char *cmd, int x, int y);

/**
 * @brief 删除图标
 * @param label 图标标签
 */
void tui_desktop_icons_remove(const char *label);

/**
 * @brief 清理所有图标（释放内存）
 */
void tui_desktop_icons_cleanup(void);

#endif /* TUI_DESKTOP_TUI_DESKTOP_ICONS_H */