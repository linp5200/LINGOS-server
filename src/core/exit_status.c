/**
 * @file    src/core/exit_status.c
 * @brief   退出状态管理实现（检测异常关闭）
 * @version LN-0.4.3
 */

#include "exit_status.h"
#include "../common/data_path.h"
#include "../common/safe_string.h"
#include "../lib/log_extra.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <errno.h>
#include <time.h>

#define EXIT_STATUS_PATH "/state/exit_status.json"
#define MAX_CRASH_COUNT 3
#define CRASH_WINDOW_SECONDS 300  /* 5 分钟内连续崩溃 */

static exit_status_t g_status;
static int g_initialized = 0;

/* ============================================================
 * 内部辅助：获取状态文件路径
 * ============================================================ */
static const char* get_status_path(void) {
    static char path[512];
    if (path[0] == '\0') {
        const char *root = lingos_data_root();
        safe_snprintf(path, sizeof(path), "%s%s", root, EXIT_STATUS_PATH);
    }
    return path;
}

/* ============================================================
 * 内部辅助：确保状态目录存在
 * ============================================================ */
static void ensure_status_dir(void) {
    const char *root = lingos_data_root();
    char dir[512];
    safe_snprintf(dir, sizeof(dir), "%s/state", root);
    if (access(dir, F_OK) != 0) {
        mkdir(dir, 0755);
    }
}

/* ============================================================
 * 内部辅助：读取状态文件
 * ============================================================ */
static int load_status(exit_status_t *status) {
    if (!status) return -1;

    const char *path = get_status_path();
    FILE *fp = fopen(path, "r");
    if (!fp) {
        LOG_DEBUG_T("ExitStatus", "Load", "NotFound", "status file not found");
        return -1;
    }

    char line[256];
    while (fgets(line, sizeof(line), fp)) {
        if (line[0] == '#' || line[0] == '\n') continue;
        char key[64];
        long long val;
        if (sscanf(line, "%63[^=]=%lld", key, &val) == 2) {
            if (strcmp(key, "last_start_time") == 0)
                status->last_start_time = (time_t)val;
            else if (strcmp(key, "last_exit_time") == 0)
                status->last_exit_time = (time_t)val;
            else if (strcmp(key, "last_exit_code") == 0)
                status->last_exit_code = (int)val;
            else if (strcmp(key, "is_clean_exit") == 0)
                status->is_clean_exit = (int)val;
            else if (strcmp(key, "crash_count") == 0)
                status->crash_count = (int)val;
            else if (strcmp(key, "first_crash_time") == 0)
                status->first_crash_time = (time_t)val;
        } else if (sscanf(line, "%63[^=]=%127[^\n]", key, status->last_exit_reason) == 2) {
            /* 处理字符串字段 */
            if (strcmp(key, "last_exit_reason") != 0) {
                /* 忽略其他字符串字段 */
            }
        }
    }
    fclose(fp);
    LOG_DEBUG_T("ExitStatus", "Load", "OK", "status loaded: clean=%d, count=%d",
                status->is_clean_exit, status->crash_count);
    return 0;
}

/* ============================================================
 * 内部辅助：保存状态文件
 * ============================================================ */
static void save_status(const exit_status_t *status) {
    if (!status) return;

    const char *path = get_status_path();
    ensure_status_dir();

    FILE *fp = fopen(path, "w");
    if (!fp) {
        LOG_WARN_T("ExitStatus", "Save", "OpenFail", "cannot write %s", path);
        return;
    }

    fprintf(fp, "# LING OS Exit Status (auto-generated)\n");
    fprintf(fp, "last_start_time=%lld\n", (long long)status->last_start_time);
    fprintf(fp, "last_exit_time=%lld\n", (long long)status->last_exit_time);
    fprintf(fp, "last_exit_code=%d\n", status->last_exit_code);
    fprintf(fp, "is_clean_exit=%d\n", status->is_clean_exit);
    fprintf(fp, "last_exit_reason=%s\n", status->last_exit_reason);
    fprintf(fp, "crash_count=%d\n", status->crash_count);
    fprintf(fp, "first_crash_time=%lld\n", (long long)status->first_crash_time);

    fclose(fp);
    LOG_DEBUG_T("ExitStatus", "Save", "OK", "status saved");
}

/* ============================================================
 * 核心 API 实现
 * ============================================================ */
int exit_status_init(exit_status_t *status) {
    LOG_INFO_T("ExitStatus", "Init", "Enter", "status=%p", (void*)status);

    if (!status) {
        status = &g_status;
    }

    memset(status, 0, sizeof(exit_status_t));

    /* 加载历史状态 */
    int loaded = load_status(status);

    if (loaded == 0) {
        /* 检查上次是否正常退出 */
        int abnormal = 0;
        if (!status->is_clean_exit) {
            time_t now = time(NULL);
            /* 如果上次退出时间在最近 30 秒内，且非正常退出，认为异常 */
            if (status->last_exit_time > 0 &&
                (now - status->last_exit_time) < 30) {
                abnormal = 1;
                LOG_WARN_T("ExitStatus", "Init", "Abnormal", "previous exit was abnormal");
            } else {
                /* 如果上次退出时间较早，可能是手动清理，重置状态 */
                LOG_DEBUG_T("ExitStatus", "Init", "Old", "old exit status, clearing");
                memset(status, 0, sizeof(exit_status_t));
            }
        }

        /* 更新 crash_count */
        if (abnormal) {
            status->crash_count++;
            if (status->crash_count == 1) {
                status->first_crash_time = time(NULL);
            }
            status->is_clean_exit = 0;
        } else {
            /* 正常启动，重置崩溃计数 */
            status->crash_count = 0;
            status->first_crash_time = 0;
            status->is_clean_exit = 1;
        }
    } else {
        /* 首次启动或文件不存在，设置为正常状态 */
        status->is_clean_exit = 1;
        status->crash_count = 0;
        status->first_crash_time = 0;
    }

    /* 更新启动时间 */
    status->last_start_time = time(NULL);
    safe_strncpy(status->last_exit_reason, "Running", sizeof(status->last_exit_reason));

    save_status(status);

    g_initialized = 1;

    LOG_INFO_T("ExitStatus", "Init", "OK", "crash_count=%d, clean=%d",
               status->crash_count, status->is_clean_exit);
    return 0;
}

int exit_status_check_abnormal(exit_status_t *status) {
    if (!status) {
        status = &g_status;
    }
    if (!g_initialized) {
        exit_status_init(status);
    }

    return (!status->is_clean_exit) ? 1 : 0;
}

void exit_status_mark_clean(int exit_code, const char *reason) {
    LOG_INFO_T("ExitStatus", "MarkClean", "Enter", "exit_code=%d, reason='%s'",
               exit_code, reason ? reason : "(null)");

    exit_status_t *status = &g_status;
    status->last_exit_time = time(NULL);
    status->last_exit_code = exit_code;
    status->is_clean_exit = 1;
    status->crash_count = 0;
    status->first_crash_time = 0;
    if (reason) {
        safe_strncpy(status->last_exit_reason, reason, sizeof(status->last_exit_reason));
    } else {
        safe_strncpy(status->last_exit_reason, "Normal exit", sizeof(status->last_exit_reason));
    }

    save_status(status);
    LOG_INFO_T("ExitStatus", "MarkClean", "OK", "marked clean");
}

void exit_status_mark_abnormal(int signal, const char *reason) {
    LOG_WARN_T("ExitStatus", "MarkAbnormal", "Enter", "signal=%d, reason='%s'",
               signal, reason ? reason : "(null)");

    exit_status_t *status = &g_status;
    status->last_exit_time = time(NULL);
    status->last_exit_code = 128 + signal;
    status->is_clean_exit = 0;
    if (reason) {
        safe_snprintf(status->last_exit_reason, sizeof(status->last_exit_reason),
                      "Signal %d: %s", signal, reason);
    } else {
        safe_snprintf(status->last_exit_reason, sizeof(status->last_exit_reason),
                      "Signal %d", signal);
    }

    save_status(status);
    LOG_WARN_T("ExitStatus", "MarkAbnormal", "OK", "marked abnormal");
}

void exit_status_clear_abnormal(void) {
    LOG_INFO_T("ExitStatus", "ClearAbnormal", "Enter", "clearing abnormal flag");

    exit_status_t *status = &g_status;
    status->is_clean_exit = 1;
    status->crash_count = 0;
    status->first_crash_time = 0;
    safe_strncpy(status->last_exit_reason, "Cleared by user", sizeof(status->last_exit_reason));

    save_status(status);
    LOG_INFO_T("ExitStatus", "ClearAbnormal", "OK", "abnormal flag cleared");
}

const exit_status_t* exit_status_get(void) {
    if (!g_initialized) {
        exit_status_init(NULL);
    }
    return &g_status;
}

void exit_status_format_message(const exit_status_t *status,
                                const char *lang,
                                char *buf, size_t size) {
    if (!status || !buf || size == 0) return;

    int is_zh = (lang && strcmp(lang, "zh") == 0);

    if (status->is_clean_exit) {
        if (is_zh) {
            safe_snprintf(buf, size, "系统上次正常退出");
        } else {
            safe_snprintf(buf, size, "System was shut down normally");
        }
        return;
    }

    char time_str[32] = {0};
    if (status->last_exit_time > 0) {
        struct tm *tm = localtime(&status->last_exit_time);
        strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S", tm);
    } else {
        safe_strncpy(time_str, "Unknown", sizeof(time_str));
    }

    if (is_zh) {
        safe_snprintf(buf, size,
                      "系统上次异常退出\n"
                      "退出码: %d\n"
                      "原因: %s\n"
                      "时间: %s\n"
                      "连续异常次数: %d",
                      status->last_exit_code,
                      status->last_exit_reason[0] ? status->last_exit_reason : "未知",
                      time_str,
                      status->crash_count);
    } else {
        safe_snprintf(buf, size,
                      "System was shut down abnormally\n"
                      "Exit code: %d\n"
                      "Reason: %s\n"
                      "Time: %s\n"
                      "Consecutive abnormal exits: %d",
                      status->last_exit_code,
                      status->last_exit_reason[0] ? status->last_exit_reason : "Unknown",
                      time_str,
                      status->crash_count);
    }
}