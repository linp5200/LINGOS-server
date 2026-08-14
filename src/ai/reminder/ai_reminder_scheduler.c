/**
 * @file    ai_reminder_scheduler.c
 * @brief   提醒定时调度器
 * @version LN-B-4.2.0.0
 */

#include "ai_reminder.h"
#include "ai_reminder_store.h"
#include "log_extra.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <pthread.h>
#include <signal.h>

#define MAX_SCHEDULED 256
#define CHECK_INTERVAL_SEC 5

/* ============================================================
 * 全局状态
 * ============================================================ */

static reminder_t g_scheduled[MAX_SCHEDULED];
static int g_scheduled_count = 0;
static int g_scheduler_running = 0;
static int g_scheduler_stop = 0;
static pthread_t g_scheduler_thread;
static pthread_mutex_t g_sched_lock = PTHREAD_MUTEX_INITIALIZER;

/* ============================================================
 * 内部辅助：检查并触发到期提醒
 * ============================================================ */

static void check_and_trigger(void) {
    time_t now = time(NULL);

    pthread_mutex_lock(&g_sched_lock);

    for (int i = 0; i < g_scheduled_count; i++) {
        if (g_scheduled[i].status == REMINDER_STATUS_PENDING &&
            g_scheduled[i].trigger_time <= now) {
            LOG_DEBUG_T("ReminderSched", "Check", "Trigger", "reminder %s due", g_scheduled[i].id);

            /* 触发提醒 */
            if (reminder_trigger(g_scheduled[i].id) == 0) {
                /* 从调度列表中移除（trigger 函数会更新状态） */
                /* 如果是重复提醒，会重新添加 */
                /* 标记为已处理 */
                g_scheduled[i].status = REMINDER_STATUS_TRIGGERED;
            }
        }
    }

    /* 清理已触发的提醒（如果不是重复提醒） */
    int write_idx = 0;
    for (int i = 0; i < g_scheduled_count; i++) {
        /* 保留 pending 状态的提醒 */
        if (g_scheduled[i].status == REMINDER_STATUS_PENDING) {
            if (write_idx != i) {
                memcpy(&g_scheduled[write_idx], &g_scheduled[i], sizeof(reminder_t));
            }
            write_idx++;
        }
    }
    g_scheduled_count = write_idx;

    pthread_mutex_unlock(&g_sched_lock);
}

/* ============================================================
 * 调度器线程
 * ============================================================ */

static void* scheduler_thread(void *arg) {
    (void)arg;
    g_scheduler_running = 1;
    LOG_INFO_T("ReminderSched", "Thread", "Start", "scheduler thread started, interval=%ds",
               CHECK_INTERVAL_SEC);

    while (!g_scheduler_stop) {
        for (int i = 0; i < CHECK_INTERVAL_SEC && !g_scheduler_stop; i++) {
            sleep(1);
        }
        if (g_scheduler_stop) break;

        check_and_trigger();
    }

    g_scheduler_running = 0;
    LOG_INFO_T("ReminderSched", "Thread", "Stop", "scheduler thread stopped");
    return NULL;
}

/* ============================================================
 * 公共 API
 * ============================================================ */

int reminder_scheduler_start(void) {
    LOG_INFO_T("ReminderSched", "Start", "Enter", "starting reminder scheduler");

    if (g_scheduler_running) {
        LOG_WARN_T("ReminderSched", "Start", "Already", "scheduler already running");
        return 0;
    }

    /* 加载所有 pending 提醒到调度列表 */
    pthread_mutex_lock(&g_sched_lock);

    g_scheduled_count = 0;
    reminder_t temp[256];
    int count = reminder_store_list(temp, 256, 0);  /* 只加载 pending */
    for (int i = 0; i < count && i < MAX_SCHEDULED; i++) {
        if (temp[i].status == REMINDER_STATUS_PENDING) {
            memcpy(&g_scheduled[g_scheduled_count], &temp[i], sizeof(reminder_t));
            g_scheduled_count++;
            LOG_DEBUG_T("ReminderSched", "Start", "Load", "loaded reminder %s", temp[i].id);
        }
    }
    LOG_INFO_T("ReminderSched", "Start", "Loaded", "loaded %d pending reminders", g_scheduled_count);

    pthread_mutex_unlock(&g_sched_lock);

    g_scheduler_stop = 0;
    int ret = pthread_create(&g_scheduler_thread, NULL, scheduler_thread, NULL);
    if (ret != 0) {
        LOG_ERROR_T("ReminderSched", "Start", "ThreadFail", "pthread_create error %d", ret);
        return -1;
    }

    return 0;
}

void reminder_scheduler_stop(void) {
    LOG_INFO_T("ReminderSched", "Stop", "Enter", "stopping reminder scheduler");

    if (!g_scheduler_running) {
        LOG_WARN_T("ReminderSched", "Stop", "NotRunning", "scheduler not running");
        return;
    }

    g_scheduler_stop = 1;
    pthread_join(g_scheduler_thread, NULL);

    LOG_INFO_T("ReminderSched", "Stop", "OK", "scheduler stopped");
}

int reminder_scheduler_add(const reminder_t *reminder) {
    LOG_DEBUG_T("ReminderSched", "Add", "Enter", "reminder=%s", reminder ? reminder->id : "(null)");

    if (!reminder) {
        LOG_ERROR_T("ReminderSched", "Add", "Invalid", "reminder is NULL");
        return -1;
    }

    if (reminder->status != REMINDER_STATUS_PENDING) {
        LOG_DEBUG_T("ReminderSched", "Add", "Skip", "reminder %s not pending", reminder->id);
        return 0;
    }

    pthread_mutex_lock(&g_sched_lock);

    /* 检查是否已存在 */
    for (int i = 0; i < g_scheduled_count; i++) {
        if (strcmp(g_scheduled[i].id, reminder->id) == 0) {
            /* 更新内容 */
            memcpy(&g_scheduled[i], reminder, sizeof(reminder_t));
            pthread_mutex_unlock(&g_sched_lock);
            LOG_DEBUG_T("ReminderSched", "Add", "Updated", "reminder %s updated", reminder->id);
            return 0;
        }
    }

    if (g_scheduled_count >= MAX_SCHEDULED) {
        pthread_mutex_unlock(&g_sched_lock);
        LOG_WARN_T("ReminderSched", "Add", "Overflow", "max scheduled reached");
        return -1;
    }

    memcpy(&g_scheduled[g_scheduled_count], reminder, sizeof(reminder_t));
    g_scheduled_count++;

    pthread_mutex_unlock(&g_sched_lock);

    LOG_DEBUG_T("ReminderSched", "Add", "OK", "reminder %s added to scheduler", reminder->id);
    return 0;
}

int reminder_scheduler_remove(const char *id) {
    LOG_DEBUG_T("ReminderSched", "Remove", "Enter", "id='%s'", id ? id : "(null)");

    if (!id || !*id) {
        LOG_ERROR_T("ReminderSched", "Remove", "Invalid", "id is NULL or empty");
        return -1;
    }

    pthread_mutex_lock(&g_sched_lock);

    int found = 0;
    int write_idx = 0;
    for (int i = 0; i < g_scheduled_count; i++) {
        if (strcmp(g_scheduled[i].id, id) != 0) {
            if (write_idx != i) {
                memcpy(&g_scheduled[write_idx], &g_scheduled[i], sizeof(reminder_t));
            }
            write_idx++;
        } else {
            found = 1;
        }
    }
    g_scheduled_count = write_idx;

    pthread_mutex_unlock(&g_sched_lock);

    if (found) {
        LOG_DEBUG_T("ReminderSched", "Remove", "OK", "reminder %s removed from scheduler", id);
    } else {
        LOG_DEBUG_T("ReminderSched", "Remove", "NotFound", "reminder %s not in scheduler", id);
    }
    return 0;
}

int reminder_scheduler_count(void) {
    pthread_mutex_lock(&g_sched_lock);
    int count = g_scheduled_count;
    pthread_mutex_unlock(&g_sched_lock);
    return count;
}

int reminder_scheduler_is_running(void) {
    return g_scheduler_running;
}