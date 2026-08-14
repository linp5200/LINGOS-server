/**
 * @file    safe_string.h
 * @brief   安全字符串操作（防止缓冲区溢出）
 * @version LN-B-3.8.0.0
 */

#ifndef COMMON_SAFE_STRING_H
#define COMMON_SAFE_STRING_H

#include <stddef.h>

/**
 * @brief 安全 snprintf（自动截断，返回所需大小）
 * @param buf 目标缓冲区
 * @param size 缓冲区大小
 * @param fmt 格式化字符串
 * @param ... 可变参数
 * @return 所需缓冲区大小（不含终止符），若 >= size 则截断
 */
int safe_snprintf(char *buf, size_t size, const char *fmt, ...);

/**
 * @brief 安全 strncpy（保证终止符）
 * @param dest 目标缓冲区
 * @param src 源字符串
 * @param size 缓冲区大小
 * @return dest
 */
char* safe_strncpy(char *dest, const char *src, size_t size);

/**
 * @brief 安全字符串拼接（保证终止符）
 * @param dest 目标缓冲区
 * @param src 源字符串
 * @param size 缓冲区大小
 * @return 拼接后的长度
 */
size_t safe_strlcat(char *dest, const char *src, size_t size);

/**
 * @brief 检查字符串是否为 NULL 或空
 * @param str 字符串
 * @return 1 如果 NULL 或空，0 否则
 */
int safe_str_is_empty(const char *str);

/**
 * @brief 安全复制字符串到动态分配的内存
 * @param str 源字符串
 * @return 新分配的字符串，失败返回 NULL
 */
char* safe_strdup(const char *str);

/**
 * @brief 安全退格（UTF-8 感知）：删除缓冲末尾一个完整字符，
 *        并在终端回退对应显示宽度（中文/全角=2 列，ASCII=1 列）
 * @param buf 输入缓冲（以 \0 结尾）
 * @param len 当前输入长度（字节数，函数内更新）
 * @return 实际删除的字节数（0 表示缓冲为空，无操作）
 */
int safe_backspace_echo(char *buf, int *len);

#endif /* COMMON_SAFE_STRING_H */