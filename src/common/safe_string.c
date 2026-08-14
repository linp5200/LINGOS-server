/**
 * @file    safe_string.c
 * @brief   安全字符串操作实现
 * @version LN-B-3.8.0.0
 */

#include "safe_string.h"
#include "../drivers/uart.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdarg.h>

int safe_snprintf(char *buf, size_t size, const char *fmt, ...) {
    if (!buf || size == 0) return -1;
    if (!fmt) {
        buf[0] = '\0';
        return 0;
    }

    va_list args;
    va_start(args, fmt);
    int ret = vsnprintf(buf, size, fmt, args);
    va_end(args);

    if (ret < 0 || ret >= (int)size) {
        /* 截断，强制终止 */
        buf[size - 1] = '\0';
        return ret < 0 ? 0 : ret;
    }
    return ret;
}

char* safe_strncpy(char *dest, const char *src, size_t size) {
    if (!dest || size == 0) return dest;
    if (!src) {
        dest[0] = '\0';
        return dest;
    }

    size_t i;
    for (i = 0; i < size - 1 && src[i] != '\0'; i++) {
        dest[i] = src[i];
    }
    dest[i] = '\0';
    return dest;
}

size_t safe_strlcat(char *dest, const char *src, size_t size) {
    if (!dest || size == 0) return 0;
    if (!src) return strlen(dest);

    size_t dest_len = strlen(dest);
    size_t src_len = strlen(src);
    size_t total = dest_len + src_len;

    if (dest_len >= size) return dest_len;

    size_t copy_len = size - dest_len - 1;
    if (copy_len > src_len) copy_len = src_len;

    for (size_t i = 0; i < copy_len; i++) {
        dest[dest_len + i] = src[i];
    }
    dest[dest_len + copy_len] = '\0';

    return total;
}

int safe_str_is_empty(const char *str) {
    return (str == NULL || str[0] == '\0');
}

char* safe_strdup(const char *str) {
    if (!str) return NULL;
    size_t len = strlen(str) + 1;
    char *buf = malloc(len);
    if (!buf) return NULL;
    memcpy(buf, str, len);
    return buf;
}

/* ============================================================
 * 安全退格（UTF-8 感知）
 * ============================================================ */
int safe_backspace_echo(char *buf, int *len) {
    if (!buf || !len || *len <= 0) return 0;

    int n = *len;
    /* 定位最后一个 UTF-8 字符的起点（跳过尾随字节 0x80-0xBF） */
    int start = n - 1;
    while (start > 0 && ((unsigned char)buf[start] & 0xC0) == 0x80) {
        start--;
    }
    int char_bytes = n - start;

    /* 显示宽度：3 字节 UTF-8（中文/全角）= 2 列；其余 = 1 列 */
    unsigned char lead = (unsigned char)buf[start];
    int width = (lead >= 0xE0) ? 2 : 1;

    /* 终端回退：\b 空格 \b 按宽度重复 */
    for (int i = 0; i < width; i++) {
        uart_puts("\b \b");
    }

    /* 移除字符字节 */
    buf[start] = '\0';
    *len = start;
    return char_bytes;
}