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
        /* 【2026-09-19 修复】异常判定重写（修复模式此前永不触发）：
         *   · 旧实现：信号退出时 mark_abnormal 被 normal_exit 的 mark_clean 覆盖，
         *     且 init 将文件直接标 clean → 崩溃/断电全被吞掉 → repair_mode 死机制
         *   · 新语义：运行期写"脏标记"（is_clean_exit=0 + reason=Running，由
         *     exit_status_mark_running 在启动链判定后写入）；干净退出才置 1
         *   · 兼容旧版文件：reason=="Running" 视为未干净退出（旧版运行中遗留） */
        int abnormal = 0;
        if (!status->is_clean_exit ||
            strcmp(status->last_exit_reason, "Running") == 0) {
            abnormal = 1;
            LOG_WARN_T("ExitStatus", "Init", "Abnormal",
                       "previous exit was abnormal (reason='%s')", status->last_exit_reason);
        }

        if (abnormal) {
            status->crash_count++;
            if (status->crash_count > 999) status->crash_count = 999;
            if (status->crash_count == 1) {
                status->first_crash_time = time(NULL);
            }
            status->is_clean_exit = 0;
        } else {
            /* 上次正常退出，重置崩溃计数 */
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

    /* 更新启动时间（脏标记由 exit_status_mark_running() 在修复判定后写入） */
    status->last_start_time = time(NULL);

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

/* ============================================================
 * 【2026-09-19 新增】运行脏标记
 *   语义：进程启动链判定完成后即置"脏"——崩溃/断电/强杀不留痕的
 *   问题由此解决（文件保持 is_clean_exit=0 直到干净退出才置 1）。
 * ============================================================ */
void exit_status_mark_running(void) {
    if (!g_initialized) {
        exit_status_init(&g_status);
    }
    exit_status_t *status = &g_status;
    status->last_start_time = time(NULL);
    status->is_clean_exit = 0;   /* 脏：未完成干净退出前保持 0 */
    safe_strncpy(status->last_exit_reason, "Running", sizeof(status->last_exit_reason));
    save_status(status);
    LOG_DEBUG_T("ExitStatus", "MarkRunning", "OK", "marked running (dirty until clean exit)");
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

    /* 【2026-09-19】原因显示优化："Running"（未完成退出标记）→ 通俗描述 */
    char reason_disp[192];
    if (strcmp(status->last_exit_reason, "Running") == 0) {
        safe_strncpy(reason_disp,
                     is_zh ? "未完成正常退出（崩溃 / 断电 / 被强制结束）"
                           : "did not complete a clean shutdown (crash / power loss / killed)",
                     sizeof(reason_disp));
    } else if (status->last_exit_reason[0]) {
        safe_strncpy(reason_disp, status->last_exit_reason, sizeof(reason_disp));
    } else {
        safe_strncpy(reason_disp, is_zh ? "未知" : "Unknown", sizeof(reason_disp));
    }

    if (is_zh) {
        safe_snprintf(buf, size,
                      "系统上次异常退出\n"
                      "退出码: %d\n"
                      "原因: %s\n"
                      "时间: %s\n"
                      "连续异常次数: %d",
                      status->last_exit_code,
                      reason_disp,
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
                      reason_disp,
                      time_str,
                      status->crash_count);
    }
}