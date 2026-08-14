/**
 * @file    error_report.h
 * @brief   统一错误报告宏定义
 * @version LN-B-4.3.0.0
 */

#ifndef ERROR_REPORT_H
#define ERROR_REPORT_H

#include <stdarg.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================
 * 错误报告函数声明
 * ============================================================ */

/**
 * @brief 致命错误报告（终止进程）
 * @param func 函数名（自动填充 __func__）
 * @param file 文件名（自动填充 __FILE__）
 * @param line 行号（自动填充 __LINE__）
 * @param fmt 格式化字符串
 * @param ... 可变参数
 */
void _report_error(const char *func, const char *file, int line, const char *fmt, ...);

/**
 * @brief 警告报告（不终止）
 */
void _report_warn(const char *func, const char *file, int line, const char *fmt, ...);

/**
 * @brief 提示报告（不终止）
 */
void _report_note(const char *func, const char *file, int line, const char *fmt, ...);

/* ============================================================
 * 用户宏（自动填充函数名/文件名/行号）
 * ============================================================ */

#define REPORT_ERROR(fmt, ...) \
    _report_error(__func__, __FILE__, __LINE__, fmt, ##__VA_ARGS__)

#define REPORT_WARN(fmt, ...) \
    _report_warn(__func__, __FILE__, __LINE__, fmt, ##__VA_ARGS__)

#define REPORT_NOTE(fmt, ...) \
    _report_note(__func__, __FILE__, __LINE__, fmt, ##__VA_ARGS__)

#ifdef __cplusplus
}
#endif

#endif /* ERROR_REPORT_H */