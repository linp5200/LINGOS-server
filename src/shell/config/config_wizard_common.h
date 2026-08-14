/**
 * @file    config_wizard_common.h
 * @brief   配置向导公共函数声明
 * @version LN-B-4.2.0.0
 */

#ifndef SHELL_CONFIG_CONFIG_WIZARD_COMMON_H
#define SHELL_CONFIG_CONFIG_WIZARD_COMMON_H

#include "../../wizard/wizard_core.h"
#include <stddef.h>

/* ============================================================
 * 颜色宏（与 log_extra.h 保持一致）
 * ============================================================ */

#ifndef COLOR_RESET
#define COLOR_RESET   "\033[0m"
#endif
#ifndef COLOR_RED
#define COLOR_RED     "\033[31m"
#endif
#ifndef COLOR_GREEN
#define COLOR_GREEN   "\033[32m"
#endif
#ifndef COLOR_YELLOW
#define COLOR_YELLOW  "\033[33m"
#endif
#ifndef COLOR_CYAN
#define COLOR_CYAN    "\033[36m"
#endif
#ifndef COLOR_DIM
#define COLOR_DIM     "\033[2m"
#endif
#ifndef COLOR_BOLD
#define COLOR_BOLD    "\033[1m"
#endif

/* ============================================================
 * 键盘键码常量
 * ============================================================ */

#define KEY_UP      1000
#define KEY_DOWN    1001
#define KEY_LEFT    1002
#define KEY_RIGHT   1003
#define KEY_ENTER   10
#define KEY_ESC     27
#define KEY_SPACE   32
#define KEY_TAB     9

/* ============================================================
 * 公共函数声明
 * ============================================================ */

/**
 * @brief 绘制选项列表（支持键盘导航）
 * @param ctx 向导上下文
 */
void wizard_draw_options(wizard_context_t *ctx);

/**
 * @brief 绘制带输入框的选项
 * @param ctx 向导上下文
 * @param prompt 提示信息
 * @param value 当前值
 * @param error_msg 错误信息（NULL 表示无错误）
 */
void wizard_draw_input(wizard_context_t *ctx, const char *prompt,
                       const char *value, const char *error_msg);

/**
 * @brief 读取键盘输入（支持方向键）
 * @return 键码（KEY_UP, KEY_DOWN, KEY_ENTER, KEY_ESC 等）
 */
int wizard_read_key(void);

/**
 * @brief 读取字符串输入（支持退格）
 * @param buf 缓冲区
 * @param buf_size 缓冲区大小
 */
void wizard_read_string(char *buf, size_t buf_size);

/**
 * @brief 读取密码（不回显）
 * @param buf 缓冲区
 * @param buf_size 缓冲区大小
 */
void wizard_read_password(char *buf, size_t buf_size);

/**
 * @brief 验证 DeepSeek API Key
 * @param key API Key
 * @param error_msg 错误信息输出
 * @param msg_len 错误信息缓冲区大小
 * @return 1 有效，0 无效
 */
int wizard_validate_deepseek_api_key(const char *key, char *error_msg, size_t msg_len);

/**
 * @brief 验证 URL
 * @param url URL
 * @param error_msg 错误信息输出
 * @param msg_len 错误信息缓冲区大小
 * @return 1 有效，0 无效
 */
int wizard_validate_url(const char *url, char *error_msg, size_t msg_len);

/**
 * @brief 验证模型名称
 * @param model 模型名称
 * @param error_msg 错误信息输出
 * @param msg_len 错误信息缓冲区大小
 * @return 1 有效，0 无效
 */
int wizard_validate_model_name(const char *model, char *error_msg, size_t msg_len);

/**
 * @brief 获取选项标签（支持多语言）
 * @param ctx 向导上下文
 * @param opt 选项指针
 * @return 标签字符串
 */
const char* wizard_opt_label(wizard_context_t *ctx, wizard_option_t *opt);

/**
 * @brief 获取选项描述（支持多语言）
 * @param ctx 向导上下文
 * @param opt 选项指针
 * @return 描述字符串
 */
const char* wizard_opt_desc(wizard_context_t *ctx, wizard_option_t *opt);

/**
 * @brief 确认对话框
 * @param message_en 英文提示
 * @param message_zh 中文提示
 * @return 1 确认，0 取消
 */
int wizard_confirm(const char *message_en, const char *message_zh);

#endif /* SHELL_CONFIG_CONFIG_WIZARD_COMMON_H */