/**
 * @file    src/lib/log_extra.h
 * @brief   分级日志系统头文件
 * @version LN-0.4.3
 * @changes 新增全局级别管理、模块重置、字符串转换等函数声明
 */

#ifndef LOG_EXTRA_H
#define LOG_EXTRA_H

#include <stdio.h>
#include <stdint.h>
#include <unistd.h>
#include <time.h>
#include <stdarg.h>

#define LOG_LEVEL_ERROR 1
#define LOG_LEVEL_WARN  2
#define LOG_LEVEL_INFO  3
#define LOG_LEVEL_DEBUG 4

#ifndef CURRENT_LOG_LEVEL
#define CURRENT_LOG_LEVEL LOG_LEVEL_WARN
/* 此处修改会被.c覆盖*/
#endif

/* 颜色宏 */
#define COLOR_RESET   "\033[0m"
#define COLOR_RED     "\033[31m"
#define COLOR_GREEN   "\033[32m"
#define COLOR_YELLOW  "\033[33m"
#define COLOR_CYAN    "\033[36m"
#define COLOR_WHITE   "\033[37m"
#define COLOR_BOLD    "\033[1m"
#define COLOR_DIM     "\033[2m"

/* 函数声明 */
void log_system_init(void);
void log_set_level(int level);
int log_get_level(void);
void log_set_console_output(int enable);
void log_set_file_output(int enable);   /* 【2026-08-22 定稿】文件保存开关：1=开(默认DEBUG全量)，0=关(仅WARN+) */
int  log_get_file_output(void);
void log_set_module_level(const char *module, int level);
int log_get_module_level(const char *module);
void log_dump_module_levels(void);

/* 核心日志输出函数（6个参数） */
void log_output(int level, const char *module, const char *submodule,
                const char *step, const char *func, const char *fmt, ...);

void emergency_write(const char *fmt, ...);
void log_dump_all(void);

/* ---- UI 辅助函数 ---- */
int log_get_terminal_width(void);
void log_color_puts(const char *color, const char *prefix, const char *fmt, ...);
void log_draw_box(const char *title, const char *content,
                  const char *title_color, const char *border_color, const char *content_color);
void log_draw_status_bar(const char *version, int ai_status, const char *mode, int task_count);

/* ---- 进度条系统 ---- */
typedef enum {
    PROGRESS_IDLE = 0,
    PROGRESS_RUNNING,
    PROGRESS_DONE,
    PROGRESS_FAILED
} progress_status_t;

void log_draw_progress(int percent, const char *label, progress_status_t status);
int log_update_progress_async(const char *label_prefix, int max_retries);
int log_get_progress_width(void);
void log_set_progress_width(int width);
void log_write_progress_file(int percent, const char *label, progress_status_t status);
int log_run_with_progress(const char *cmd, const char *label, int timeout_sec);

/* ============================================================
 * 新增：全局日志级别管理（log level 指令支持）
 * ============================================================ */

/**
 * @brief 设置全局日志级别
 * @param level 日志级别 (LOG_LEVEL_ERROR/WARN/INFO/DEBUG)
 */
void log_set_global_level(int level);

/**
 * @brief 获取当前全局日志级别
 * @return 当前全局日志级别
 */
int log_get_global_level(void);

/**
 * @brief 获取默认日志级别
 * @return 默认日志级别 (LOG_LEVEL_DEBUG)
 */
int log_get_default_level(void);

/**
 * @brief 将字符串转换为日志级别
 * @param str 字符串 ("debug", "info", "warn", "error")
 * @return 日志级别，-1 表示无效
 */
int log_level_from_string(const char *str);

/**
 * @brief 将日志级别转换为字符串
 * @param level 日志级别
 * @return 字符串 ("debug"/"info"/"warn"/"error"/"unknown")
 */
const char* log_level_to_string(int level);

/**
 * @brief 检查模块是否存在（已设置自定义级别）
 * @param module 模块名称
 * @return 1 存在，0 不存在
 */
int log_module_exists(const char *module);

/**
 * @brief 重置特定模块的日志级别（恢复为全局级别）
 * @param module 模块名称
 * @return 0 成功，-1 失败（模块不存在）
 */
int log_reset_module_level(const char *module);

/**
 * @brief 重置所有模块的日志级别（恢复为全局级别）
 * @return 0 成功
 */
int log_reset_all_modules(void);

/* ---- 日志宏（自动注入 __func__） ---- */
#define LOG_ERROR_T(m, s, st, f, ...) \
    log_output(LOG_LEVEL_ERROR, m, s, st, __func__, f, ##__VA_ARGS__)

#define LOG_WARN_T(m, s, st, f, ...) \
    log_output(LOG_LEVEL_WARN, m, s, st, __func__, f, ##__VA_ARGS__)

#define LOG_INFO_T(m, s, st, f, ...) \
    log_output(LOG_LEVEL_INFO, m, s, st, __func__, f, ##__VA_ARGS__)

#define LOG_DEBUG_T(m, s, st, f, ...) \
    log_output(LOG_LEVEL_DEBUG, m, s, st, __func__, f, ##__VA_ARGS__)

#define LOG_EMERG_T(m, s, st, f, ...) \
    log_output(LOG_LEVEL_ERROR, m, s, st, __func__, f, ##__VA_ARGS__)

#endif /* LOG_EXTRA_H */