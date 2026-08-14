/**
 * @file    error_logger.c
 * @brief   统一错误日志记录（写入 /LINGOS/Debug/error_*.log）
 * @version LN-B-5.0.0.0
 * @changes 安全字符串替换；双文支持
 */

#include "error_logger.h"
#include "../common/data_path.h"
#include "../common/safe_string.h"
#include "../common/lang.h"
#include "../lib/log_extra.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>
#include <errno.h>

#define ERROR_LOG_DIR "/Debug"
#define ERROR_LOG_PREFIX "error_"

static const char* get_error_log_path(void) {
    static char path[512];
    static char last_date[11] = {0};
    time_t now = time(NULL);
    struct tm *tm = localtime(&now);
    char date_str[11];
    strftime(date_str, sizeof(date_str), "%Y%m%d", tm);

    if (strcmp(date_str, last_date) != 0) {
        const char *root = lingos_data_root();
        safe_snprintf(path, sizeof(path), "%s%s/%s%s.log", root, ERROR_LOG_DIR, ERROR_LOG_PREFIX, date_str);
        safe_strncpy(last_date, date_str, sizeof(last_date));
    }
    return path;
}

static void ensure_debug_dir(void) {
    const char *root = lingos_data_root();
    char dir[512];
    safe_snprintf(dir, sizeof(dir), "%s%s", root, ERROR_LOG_DIR);
    if (access(dir, F_OK) != 0) {
        if (mkdir(dir, 0755) != 0 && errno != EEXIST) {
            LOG_ERROR_T("ErrorLogger", "Mkdir", "Fail", "cannot create %s", dir);
        }
    }
}

void log_error_to_file_va(const char *tag, const char *fmt, va_list args) {
    if (!tag || !fmt) return;
    ensure_debug_dir();
    const char *path = get_error_log_path();
    FILE *fp = fopen(path, "a");
    if (!fp) return;

    time_t now = time(NULL);
    struct tm *tm = localtime(&now);
    char time_buf[32];
    strftime(time_buf, sizeof(time_buf), "%Y-%m-%d %H:%M:%S", tm);
    fprintf(fp, "[%s] [%s] ", time_buf, tag);
    vfprintf(fp, fmt, args);
    fprintf(fp, "\n");
    fclose(fp);

    /* 同时记录到主日志系统 */
    char msg[2048];
    vsnprintf(msg, sizeof(msg), fmt, args);
    LOG_ERROR_T("ErrorLogger", "Write", tag, "%s", msg);
}

void log_error_to_file(const char *tag, const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    log_error_to_file_va(tag, fmt, args);
    va_end(args);
}