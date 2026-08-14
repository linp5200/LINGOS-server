/**
 * @file    tui_controls.h
 * @brief   TUI 控件声明（按钮、列表、输入框）
 * @version LN-B-3.8.0.0
 */

#ifndef TUI_TUI_CONTROLS_H
#define TUI_TUI_CONTROLS_H

#include <notcurses/notcurses.h>
#include "../wizard/wizard_core.h"

/* ============================================================
 * 控件类型
 * ============================================================ */

typedef enum {
    CTL_LABEL,
    CTL_BUTTON,
    CTL_LIST_ITEM,
    CTL_INPUT,
    CTL_CHECKBOX,
    CTL_SEPARATOR
} tui_ctl_type_t;

/* ============================================================
 * 控件结构
 * ============================================================ */

typedef struct tui_control {
    tui_ctl_type_t type;
    char *label;
    int x;
    int y;
    int width;
    int height;
    int focused;
    int selected;
    void *data;
    void (*on_click)(struct tui_control *ctl, void *user_data);
    void *user_data;
    struct tui_control *next;
    struct tui_control *prev;
} tui_control_t;

/* ============================================================
 * 控件列表
 * ============================================================ */

typedef struct tui_control_list {
    tui_control_t *head;
    tui_control_t *tail;
    tui_control_t *focused;
    int count;
} tui_control_list_t;

/* ============================================================
 * 控件 API
 * ============================================================ */

/**
 * @brief 创建控件列表
 * @return 控件列表指针
 */
tui_control_list_t* tui_controls_create_list(void);

/**
 * @brief 销毁控件列表
 * @param list 控件列表指针
 */
void tui_controls_destroy_list(tui_control_list_t *list);

/**
 * @brief 添加按钮
 * @param list 控件列表
 * @param label 标签
 * @param x X坐标
 * @param y Y坐标
 * @param width 宽度
 * @param on_click 点击回调
 * @param user_data 用户数据
 * @return 控件指针
 */
tui_control_t* tui_controls_add_button(tui_control_list_t *list,
                                       const char *label,
                                       int x, int y, int width,
                                       void (*on_click)(tui_control_t *, void *),
                                       void *user_data);

/**
 * @brief 添加列表项（选择项）
 * @param list 控件列表
 * @param label 标签
 * @param x X坐标
 * @param y Y坐标
 * @param data 关联数据
 * @return 控件指针
 */
tui_control_t* tui_controls_add_list_item(tui_control_list_t *list,
                                          const char *label,
                                          int x, int y,
                                          void *data);

/**
 * @brief 添加输入框
 * @param list 控件列表
 * @param label 标签
 * @param x X坐标
 * @param y Y坐标
 * @param width 宽度
 * @param buffer 输入缓冲区
 * @param buffer_size 缓冲区大小
 * @return 控件指针
 */
tui_control_t* tui_controls_add_input(tui_control_list_t *list,
                                      const char *label,
                                      int x, int y, int width,
                                      char *buffer, int buffer_size);

/**
 * @brief 添加复选框
 * @param list 控件列表
 * @param label 标签
 * @param x X坐标
 * @param y Y坐标
 * @param selected 初始状态
 * @param on_click 点击回调
 * @param user_data 用户数据
 * @return 控件指针
 */
tui_control_t* tui_controls_add_checkbox(tui_control_list_t *list,
                                         const char *label,
                                         int x, int y,
                                         int selected,
                                         void (*on_click)(tui_control_t *, void *),
                                         void *user_data);

/**
 * @brief 添加分隔符
 * @param list 控件列表
 * @param x X坐标
 * @param y Y坐标
 * @param width 宽度
 * @return 控件指针
 */
tui_control_t* tui_controls_add_separator(tui_control_list_t *list,
                                          int x, int y, int width);

/**
 * @brief 聚焦下一个控件
 * @param list 控件列表
 * @return 新聚焦控件
 */
tui_control_t* tui_controls_focus_next(tui_control_list_t *list);

/**
 * @brief 聚焦上一个控件
 * @param list 控件列表
 * @return 新聚焦控件
 */
tui_control_t* tui_controls_focus_prev(tui_control_list_t *list);

/**
 * @brief 绘制控件列表
 * @param list 控件列表
 * @param plane Notcurses 平面
 */
void tui_controls_render(tui_control_list_t *list, struct ncplane *plane);

/**
 * @brief 处理点击事件
 * @param list 控件列表
 * @param x 点击X坐标
 * @param y 点击Y坐标
 * @return 被点击的控件，NULL 表示未点击
 */
tui_control_t* tui_controls_handle_click(tui_control_list_t *list, int x, int y);

/**
 * @brief 处理键盘事件
 * @param list 控件列表
 * @param key 键码
 * @return 1 表示事件已处理，0 未处理
 */
int tui_controls_handle_key(tui_control_list_t *list, int key);

#endif /* TUI_TUI_CONTROLS_H */