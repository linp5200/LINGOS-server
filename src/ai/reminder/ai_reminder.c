/**
 * @file    ai_reminder.c
 * @brief   AI 主动任务提醒实现
 * @version LN-B-4.2.0.0
 */

#include "ai_reminder.h"
#include "ai_reminder_store.h"
#include "ai_reminder_scheduler.h"
#include "data_path.h"
#include "safe_string.h"
#include "log_extra.h"
#include "uart.h"
#include "lang.h"
#include <sys/stat.h>     /* 提供 mkdir() */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <errno.h>
#include <pthread.h>

#define REMINDER_DIR "/data/reminders"
#define REMINDER_INDEX_FILE "/data/reminders/index.json"

static int g_initialized = 0;
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;

/* ============================================================
 * 内部辅助：获取提醒目录
 * ============================================================ */

static const char* get_reminder_dir(void) {
    static char path[512];
    if (path[0] == '\0') {
        const char *root = lingos_data_root();
        safe_snprintf(path, sizeof(path), "%s%s", root, REMINDER_DIR);
    }
    return path;
}

static const char* get_index_path(void) {
    static char path[512];
    if (path[0] == '\0') {
        const char *root = lingos_data_root();
        safe_snprintf(path, sizeof(path), "%s%s", root, REMINDER_INDEX_FILE);
    }
    return path;
}

/* ============================================================
 * 内部辅助：确保目录存在
 * ============================================================ */

static int ensure_reminder_dir(void) {
    const char *dir = get_reminder_dir();
    if (access(dir, F_OK) != 0) {
        if (mkdir(dir, 0755) != 0) {
            LOG_ERROR_T("Reminder", "EnsureDir", "Fail", "cannot create %s: %s",
                        dir, strerror(errno));
            return -1;
        }
    }
    return 0;
}

/* ============================================================
 * 内部辅助：生成提醒 ID
 * ============================================================ */

static char* generate_reminder_id(void) {
    time_t now = time(NULL);
    struct tm *tm = localtime(&now);
    static int counter = 0;
    counter++;

    char *buf = malloc(64);
    if (!buf) return NULL;
    strftime(buf, 64, "reminder_%Y%m%d_%H%M%S", tm);
    char suffix[16];
    safe_snprintf(suffix, sizeof(suffix), "_%03d", counter % 1000);
    strcat(buf, suffix);
    return buf;
}

/* ============================================================
 * 公共 API 实现
 * ============================================================ */

int reminder_init(void) {
    LOG_INFO_T("Reminder", "Init", "Enter", "initializing reminder system");

    if (g_initialized) {
        LOG_DEBUG_T("Reminder", "Init", "Already", "already initialized");
        return 0;
    }

    if (ensure_reminder_dir() != 0) {
        LOG_ERROR_T("Reminder", "Init", "DirFail", "failed to create reminder directory");
        return -1;
    }

    /* 加载存储索引 */
    if (reminder_store_load() != 0) {
        LOG_WARN_T("Reminder", "Init", "LoadFail", "failed to load index, starting fresh");
        reminder_store_clear();
    }

    /* 启动调度器 */
    if (reminder_scheduler_start() != 0) {
        LOG_WARN_T("Reminder", "Init", "SchedulerFail", "scheduler start failed, continuing");
    }

    g_initialized = 1;
    LOG_INFO_T("Reminder", "Init", "OK", "reminder system ready");
    return 0;
}

int reminder_add(const char *content, time_t trigger_time,
                 int repeat, int repeat_interval, const char *session_id,
                 char *out_id, size_t out_id_len) {
    LOG_INFO_T("Reminder", "Add", "Enter", "content='%.50s...', trigger_time=%ld",
               content ? content : "(null)", (long)trigger_time);

    if (!content || !*content) {
        LOG_ERROR_T("Reminder", "Add", "Invalid", "content is empty");
        return -1;
    }

    if (trigger_time <= time(NULL)) {
        LOG_WARN_T("Reminder", "Add", "PastTime", "trigger time is in the past");
        /* 允许添加过去时间，但会立即触发 */
    }

    if (!g_initialized) {
        if (reminder_init() != 0) {
            LOG_ERROR_T("Reminder", "Add", "InitFail", "reminder system not initialized");
            return -1;
        }
    }

    pthread_mutex_lock(&g_lock);

    char *id = generate_reminder_id();
    if (!id) {
        pthread_mutex_unlock(&g_lock);
        LOG_ERROR_T("Reminder", "Add", "IdFail", "failed to generate ID");
        return -1;
    }

    reminder_t reminder;
    memset(&reminder, 0, sizeof(reminder));
    safe_strncpy(reminder.id, id, sizeof(reminder.id));
    safe_strncpy(reminder.content, content, sizeof(reminder.content));
    reminder.trigger_time = trigger_time;
    reminder.created_at = time(NULL);
    reminder.triggered_at = 0;
    reminder.status = REMINDER_STATUS_PENDING;
    reminder.repeat = repeat;
    reminder.repeat_interval = repeat_interval;
    if (session_id) {
        safe_strncpy(reminder.session_id, session_id, sizeof(reminder.session_id));
    }

    /* 存储到文件 */
    int ret = reminder_store_save(&reminder);
    free(id);

    if (ret == 0) {
        /* 添加到调度器 */
        reminder_scheduler_add(&reminder);

        if (out_id && out_id_len > 0) {
            safe_strncpy(out_id, reminder.id, out_id_len);
        }
        LOG_INFO_T("Reminder", "Add", "OK", "reminder %s added", reminder.id);
    } else {
        LOG_ERROR_T("Reminder", "Add", "SaveFail", "failed to save reminder");
    }

    pthread_mutex_unlock(&g_lock);
    return ret;
}

int reminder_delete(const char *id) {
    LOG_INFO_T("Reminder", "Delete", "Enter", "id='%s'", id ? id : "(null)");

    if (!id || !*id) {
        LOG_ERROR_T("Reminder", "Delete", "Invalid", "id is NULL or empty");
        return -1;
    }

    if (!g_initialized) {
        LOG_WARN_T("Reminder", "Delete", "NotInit", "reminder system not initialized");
        return -1;
    }

    pthread_mutex_lock(&g_lock);

    /* 从调度器移除 */
    reminder_scheduler_remove(id);

    /* 从存储删除 */
    int ret = reminder_store_delete(id);

    pthread_mutex_unlock(&g_lock);

    if (ret == 0) {
        LOG_INFO_T("Reminder", "Delete", "OK", "reminder %s deleted", id);
    } else {
        LOG_ERROR_T("Reminder", "Delete", "Fail", "failed to delete reminder %s", id);
    }
    return ret;
}

int reminder_list(reminder_t *out, int max_count, int include_triggered) {
    LOG_DEBUG_T("Reminder", "List", "Enter", "max_count=%d, include_triggered=%d",
                max_count, include_triggered);

    if (!out || max_count <= 0) {
        LOG_ERROR_T("Reminder", "List", "Invalid", "out=%p, max_count=%d", (void*)out, max_count);
        return -1;
    }

    if (!g_initialized) {
        if (reminder_init() != 0) {
            LOG_ERROR_T("Reminder", "List", "InitFail", "reminder system not initialized");
            return -1;
        }
    }

    pthread_mutex_lock(&g_lock);
    int count = reminder_store_list(out, max_count, include_triggered);
    pthread_mutex_unlock(&g_lock);

    LOG_DEBUG_T("Reminder", "List", "OK", "returned %d reminders", count);
    return count;
}

int reminder_get(const char *id, reminder_t *out) {
    LOG_DEBUG_T("Reminder", "Get", "Enter", "id='%s'", id ? id : "(null)");

    if (!id || !*id || !out) {
        LOG_ERROR_T("Reminder", "Get", "Invalid", "id=%p, out=%p", (void*)id, (void*)out);
        return -1;
    }

    if (!g_initialized) {
        if (reminder_init() != 0) {
            LOG_ERROR_T("Reminder", "Get", "InitFail", "reminder system not initialized");
            return -1;
        }
    }

    pthread_mutex_lock(&g_lock);
    int ret = reminder_store_get(id, out);
    pthread_mutex_unlock(&g_lock);

    return ret;
}

int reminder_trigger(const char *id) {
    LOG_INFO_T("Reminder", "Trigger", "Enter", "id='%s'", id ? id : "(null)");

    if (!id || !*id) {
        LOG_ERROR_T("Reminder", "Trigger", "Invalid", "id is NULL or empty");
        return -1;
    }

    if (!g_initialized) {
        LOG_WARN_T("Reminder", "Trigger", "NotInit", "reminder system not initialized");
        return -1;
    }

    reminder_t reminder;
    if (reminder_get(id, &reminder) != 0) {
        LOG_ERROR_T("Reminder", "Trigger", "NotFound", "reminder %s not found", id);
        return -1;
    }

    if (reminder.status != REMINDER_STATUS_PENDING) {
        LOG_WARN_T("Reminder", "Trigger", "WrongStatus", "reminder %s status is %d",
                   id, reminder.status);
        return -1;
    }

    /* 更新状态 */
    reminder.status = REMINDER_STATUS_TRIGGERED;
    reminder.triggered_at = time(NULL);

    pthread_mutex_lock(&g_lock);
    int ret = reminder_store_save(&reminder);
    pthread_mutex_unlock(&g_lock);

    if (ret != 0) {
        LOG_ERROR_T("Reminder", "Trigger", "SaveFail", "failed to update reminder %s", id);
        return -1;
    }

    /* 输出提醒到终端 */
    uart_puts(COLOR_BOLD COLOR_YELLOW);
    uart_puts("\n┌─────────────────────────────────────────────────────────────┐\n");
    uart_puts("│  ⏰ REMINDER                                             │\n");
    uart_puts("├─────────────────────────────────────────────────────────────┤\n");
    uart_puts("│  ");
    uart_puts(reminder.content);
    uart_puts("\n");
    uart_puts("└─────────────────────────────────────────────────────────────┘\n");
    uart_puts(COLOR_RESET);

    LOG_INFO_T("Reminder", "Trigger", "OK", "reminder %s triggered: %s", id, reminder.content);

    /* 检查是否需要重复 */
    if (reminder.repeat > 0) {
        reminder_t new_reminder;
        memcpy(&new_reminder, &reminder, sizeof(reminder));
        new_reminder.trigger_time = time(NULL) + reminder.repeat_interval;
        new_reminder.created_at = time(NULL);
        new_reminder.triggered_at = 0;
        new_reminder.status = REMINDER_STATUS_PENDING;
        new_reminder.repeat = reminder.repeat - 1;

        char new_id[64];
        if (reminder_add(new_reminder.content, new_reminder.trigger_time,
                         new_reminder.repeat, new_reminder.repeat_interval,
                         new_reminder.session_id, new_id, sizeof(new_id)) == 0) {
            LOG_INFO_T("Reminder", "Trigger", "Repeat", "repeating reminder: %s", new_id);
        }
    }

    return 0;
}

int reminder_count(int include_triggered) {
    if (!g_initialized) {
        if (reminder_init() != 0) {
            return 0;
        }
    }

    pthread_mutex_lock(&g_lock);
    int count = reminder_store_count(include_triggered);
    pthread_mutex_unlock(&g_lock);
    return count;
}

void reminder_cleanup(void) {
    LOG_INFO_T("Reminder", "Cleanup", "Enter", "cleaning up reminder system");

    reminder_scheduler_stop();

    pthread_mutex_lock(&g_lock);
    reminder_store_clear();
    g_initialized = 0;
    pthread_mutex_unlock(&g_lock);

    LOG_INFO_T("Reminder", "Cleanup", "OK", "reminder system cleaned up");
}

const char* reminder_status_name(reminder_status_t status) {
    switch (status) {
        case REMINDER_STATUS_PENDING:   return "pending";
        case REMINDER_STATUS_TRIGGERED: return "triggered";
        case REMINDER_STATUS_CANCELLED: return "cancelled";
        case REMINDER_STATUS_EXPIRED:   return "expired";
        default:                        return "unknown";
    }
}