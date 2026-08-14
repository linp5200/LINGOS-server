/**
 * @file    src/health/health_watchdog.c
 * @brief   后台健康检查守护线程（定时触发 system_health.c 中的健康检查）
 * @version LN-B-4.3.0.0
 * @changes 新增配置向导步骤注册
 */

#include "health_watchdog.h"
#include "system_health.h"
#include "repair/active_repair.h"
#include "../common/safe_string.h"
#include "../common/data_path.h"
#include "../lib/log_extra.h"
#include "../drivers/uart.h"
#include "../common/lang.h"
#include <pthread.h>
#include <unistd.h>
#include <sys/stat.h>
#include <stdio.h>
#include <string.h>

#define HEALTH_CONFIG_PATH "/system/config/health.conf"

static pthread_t watchdog_thread;
static volatile int running = 0;
static volatile int stop_flag = 0;
static int check_interval = 3600;

static const char *get_config_path(void) {
    static char path[512];
    if (path[0] == '\0') {
        const char *root = lingos_data_root();
        safe_snprintf(path, sizeof(path), "%s%s", root, HEALTH_CONFIG_PATH);
    }
    return path;
}

static void create_default_config(void) {
    const char *path = get_config_path();
    if (access(path, F_OK) == 0) return;
    char dir[512];
    const char *root = lingos_data_root();
    safe_snprintf(dir, sizeof(dir), "%s/system/config", root);
    mkdir(dir, 0755);
    FILE *fp = fopen(path, "w");
    if (!fp) {
        LOG_ERROR_T("HealthWatchdog", "CreateConfig", "Fail", "cannot create %s", path);
        return;
    }
    fprintf(fp,
        "# Health watchdog configuration\n"
        "check_interval = 3600\n");
    fclose(fp);
    LOG_INFO_T("HealthWatchdog", "CreateConfig", "OK", "created %s", path);
}

static void load_config(void) {
    const char *path = get_config_path();
    FILE *fp = fopen(path, "r");
    if (!fp) {
        create_default_config();
        fp = fopen(path, "r");
        if (!fp) return;
    }
    char line[256];
    while (fgets(line, sizeof(line), fp)) {
        if (line[0] == '#' || line[0] == '\n') continue;
        char key[64];
        int val;
        if (sscanf(line, "%63[^=]=%d", key, &val) == 2) {
            if (strcmp(key, "check_interval") == 0 && val >= 60) check_interval = val;
        }
    }
    fclose(fp);
    LOG_DEBUG_T("HealthWatchdog", "LoadConfig", "OK", "interval=%d", check_interval);
}

/* ============================================================
 * 保存配置
 * ============================================================ */

int health_config_save(int interval) {
    const char *root = lingos_data_root();
    char path[512];
    safe_snprintf(path, sizeof(path), "%s/system/config/health.conf", root);

    char dir[512];
    safe_snprintf(dir, sizeof(dir), "%s/system/config", root);
    mkdir(dir, 0755);

    FILE *fp = fopen(path, "w");
    if (!fp) {
        LOG_ERROR_T("HealthWatchdog", "Save", "OpenFail", "cannot write %s", path);
        return -1;
    }

    fprintf(fp, "# Health watchdog configuration\n");
    fprintf(fp, "check_interval = %d\n", interval);
    fclose(fp);

    LOG_INFO_T("HealthWatchdog", "Save", "OK", "health.conf saved, interval=%d", interval);
    return 0;
}

/* ============================================================
 * 看门狗线程
 * ============================================================ */

static void* watchdog_loop(void *arg) {
    (void)arg;
    running = 1;
    LOG_INFO_T("HealthWatchdog", "Thread", "Start", "interval=%d seconds", check_interval);

    while (!stop_flag) {
        for (int i = 0; i < check_interval && !stop_flag; i++) sleep(1);
        if (stop_flag) break;

        health_check_and_alert();

        int mem_usage = get_memory_usage();
        int disk_usage = get_disk_usage(lingos_data_root());
        double load1, load5, load15;
        get_load_avg(&load1, &load5, &load15);
        int python_ok = check_python();
        int ai_ok = check_ai_backend();

        char error_buf[256] = {0};
        int trigger_repair = 0;

        if (mem_usage > 90) {
            safe_snprintf(error_buf, sizeof(error_buf), "memory usage: %d%%", mem_usage);
            trigger_repair = 1;
        } else if (disk_usage > 85) {
            safe_snprintf(error_buf, sizeof(error_buf), "disk usage: %d%%", disk_usage);
            trigger_repair = 1;
        } else if (!ai_ok) {
            safe_snprintf(error_buf, sizeof(error_buf), "AI backend unreachable");
            trigger_repair = 1;
        } else if (load1 > 2.0) {
            safe_snprintf(error_buf, sizeof(error_buf), "load average: %.2f", load1);
            trigger_repair = 1;
        }

        if (trigger_repair) {
            LOG_INFO_T("HealthWatchdog", "Repair", "Trigger", "repair triggered: %s", error_buf);
            repair_result_t result;
            int ret = active_repair_trigger(error_buf, "health_watchdog", &result);
            if (ret == 0 && result.success) {
                LOG_INFO_T("HealthWatchdog", "Repair", "OK", "repair succeeded: %s", result.action_used);
                uart_puts(COLOR_GREEN);
                uart_puts(tr("[REPAIR] Auto-repair succeeded: ", "[修复] 自动修复成功："));
                uart_puts(result.action_used);
                uart_puts(COLOR_RESET);
                uart_puts("\n");
            } else if (ret == 0 && !result.success) {
                LOG_WARN_T("HealthWatchdog", "Repair", "Fail", "repair failed: %s", result.error_msg);
            }
        }
    }

    running = 0;
    LOG_INFO_T("HealthWatchdog", "Thread", "Stop", "watchdog stopped");
    return NULL;
}

/* ============================================================
 * 公共 API
 * ============================================================ */

int health_watchdog_start(void) {
    if (running) {
        LOG_WARN_T("HealthWatchdog", "Start", "AlreadyRunning", "watchdog already running");
        return 0;
    }

    active_repair_init();

    load_config();
    stop_flag = 0;

    /* ====== 新增：注册健康看门狗配置步骤 ====== */
    LOG_DEBUG_T("HealthWatchdog", "Start", "ConfigStep", "health config step available");

    int ret = pthread_create(&watchdog_thread, NULL, watchdog_loop, NULL);
    if (ret != 0) {
        LOG_ERROR_T("HealthWatchdog", "Start", "Fail", "pthread_create error %d", ret);
        return -1;
    }
    return 0;
}

void health_watchdog_stop(void) {
    if (!running) return;
    stop_flag = 1;
    pthread_join(watchdog_thread, NULL);
}

int health_watchdog_is_running(void) {
    return running;
}

int health_watchdog_set_interval(int seconds) {
    if (seconds < 60) seconds = 60;
    check_interval = seconds;
    health_config_save(seconds);
    LOG_INFO_T("HealthWatchdog", "SetInterval", "OK", "new interval=%d", check_interval);
    return 0;
}

int health_watchdog_get_interval(void) {
    return check_interval;
}

int health_watchdog_trigger_now(void) {
    if (!running) {
        LOG_WARN_T("HealthWatchdog", "Trigger", "NotRunning", "watchdog not running");
        return -1;
    }
    health_check_and_alert();
    return 0;
}