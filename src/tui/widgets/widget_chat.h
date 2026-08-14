/**
 * @file    widget_chat.h
 * @brief   TUI 桌面 Nook 聊天小部件
 * @version LN-B-4.2.0.0
 */

#ifndef TUI_WIDGETS_WIDGET_CHAT_H
#define TUI_WIDGETS_WIDGET_CHAT_H

/**
 * @brief 创建聊天小部件（在聚焦窗口中启动 Nook 对话）
 */
void widget_chat_create(void);

/**
 * @brief 发送消息到 Nook
 * @param msg 消息内容
 */
void widget_chat_input(const char *msg);

/**
 * @brief 处理键盘按键（用于输入框）
 * @param key 键码
 */
void widget_chat_keypress(int key);

/**
 * @brief 销毁聊天小部件
 */
void widget_chat_destroy(void);

#endif /* TUI_WIDGETS_WIDGET_CHAT_H */