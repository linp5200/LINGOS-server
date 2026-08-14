/**
 * @file    error_report.c
 * @brief   统一错误报告系统（实现部分）
 * @version LN-B-4.3.0.0
 * @par     核心协议：C-C（攻击性/契约式使用 abort()）
 */

#include "error_report.h"
#include "error_codes.h"
#include "log_extra.h"
#include "safe_string.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <time.h>
#include <sys/types.h>
#include <sys/stat.h>

/* ============================================================
 * 内部辅助：生成错误地址
 * ============================================================ */

static uint64_t g_error_sequence = 0;

static void generate_error_address(char *buf, size_t size, uint32_t hex_id) {
    time_t now = time(NULL);
    uint32_t timestamp = (uint32_t)(now & 0xFFFFFFFF);
    snprintf(buf, size, "0x%08x/%08x", timestamp, hex_id);
}

/* ============================================================
 * 内部辅助：检查是否在开发者模式
 * ============================================================ */

static int is_developer_mode(void) {
    const char *env = getenv("LINGOS_DEV_MODE");
    return (env && strcmp(env, "1") == 0) ? 1 : 0;
}

/* ============================================================
 * 核心错误报告函数（攻击性/契约式，使用 abort()）
 * ============================================================ */

void _report_error(const char *func, const char *file, int line, const char *fmt, ...) {
    char msg_buf[512];
    va_list args;
    va_start(args, fmt);
    vsnprintf(msg_buf, sizeof(msg_buf), fmt, args);
    va_end(args);

    /* 获取错误编码（简化：默认未知） */
    uint32_t hex_id = 0x00000000;
    /* 尝试从 msg_buf 中提取错误符号，这里简化 */
    const error_code_t *ec = error_code_find("ERR_UNKNOWN_001");
    if (ec) hex_id = ec->hex_id;

    char address[64];
    generate_error_address(address, sizeof(address), hex_id);

    /* 输出到终端（红色） */
    fprintf(stderr, "\033[31mError\033[0m [%s] %s (%s:%d)\n", func, msg_buf, file, line);
    fprintf(stderr, "  Crash Locator: %s\n", address);

    /* 输出到日志 */
    LOG_ERROR_T("ErrorReport", "Fatal", "Crash", "%s (func=%s file=%s:%d)", msg_buf, func, file, line);

    /* 如果开发者模式，输出额外信息 */
    if (is_developer_mode()) {
        fprintf(stderr, "  [DEVELOPER MODE] abort() called, generating core dump...\n");
    }

    /* 将错误代码写入环境变量，供监督者读取 */
    setenv("LINGOS_CRASH_CODE", "ERR_UNKNOWN_001", 1);

    /* 攻击性：终止进程 */
    abort();
}

/* ============================================================
 * 警告报告函数（防御性/容错，不终止）
 * ============================================================ */

void _report_warn(const char *func, const char *file, int line, const char *fmt, ...) {
    char msg_buf[512];
    va_list args;
    va_start(args, fmt);
    vsnprintf(msg_buf, sizeof(msg_buf), fmt, args);
    va_end(args);

    fprintf(stderr, "\033[33mWarn\033[0m [%s] %s (%s:%d)\n", func, msg_buf, file, line);
    LOG_WARN_T("ErrorReport", "Warn", "Msg", "%s (func=%s)", msg_buf, func);
}

/* ============================================================
 * 提示报告函数（跛脚式/防弹，不终止）
 * ============================================================ */

void _report_note(const char *func, const char *file, int line, const char *fmt, ...) {
    char msg_buf[512];
    va_list args;
    va_start(args, fmt);
    vsnprintf(msg_buf, sizeof(msg_buf), fmt, args);
    va_end(args);

    fprintf(stderr, "\033[36mNote\033[0m [%s] %s (%s:%d)\n", func, msg_buf, file, line);
    LOG_DEBUG_T("ErrorReport", "Note", "Msg", "%s (func=%s)", msg_buf, func);
}