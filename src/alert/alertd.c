/**
 * @file    alertd.c
 * @brief   预警系统独立子进程（lingos_alertd）
 * @version LN-B-4.3.0.0
 * @par     核心协议：防弹编程（独立进程 + 心跳监控）
 */

#include "alertd.h"
#include "alert_manager.h"
#include "alert_config.h"
#include "alert_notify.h"
#include "../common/error_report.h"
#include "../common/safe_string.h"
#include "../lib/log_extra.h"
#include "../core/version.h"
#include "alert_history.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <time.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <pthread.h>

/* ============================================================
 * 全局状态
 * ============================================================ */

static volatile int g_running = 1;
static volatile int g_heartbeat = 0;
static pthread_t g_heartbeat_thread;
static alert_config_t g_config;

/* ============================================================
 * 信号处理
 * ============================================================ */

static void signal_handler(int sig) {
    LOG_INFO_T("Alertd", "Signal", "Received", "signal=%d", sig);
    if (sig == SIGTERM || sig == SIGINT) {
        g_running = 0;
    }
}

/* ============================================================
 * 心跳线程（供父进程监控）
 * ============================================================ */

static void* heartbeat_thread_func(void *arg) {
    (void)arg;
    while (g_running) {
        g_heartbeat = 1;
        sleep(5);
        g_heartbeat = 0;
        sleep(1);
    }
    return NULL;
}

/* ============================================================
 * 主循环
 * ============================================================ */

static void check_local_health(const alert_config_t *cfg);

static void main_loop(void) {
    LOG_INFO_T("Alertd", "MainLoop", "Start", "alertd main loop started");

    time_t last_check = 0;
    time_t last_history_cleanup = 0;
    int check_interval = g_config.base_interval;

    static time_t last_health_check = 0;
    while (g_running) {
        time_t now = time(NULL);
        /* 【C修复】健康检查独立短间隔（60s），不等 base_interval */
        if (now - last_health_check >= 60) {
            check_local_health(&g_config);
            last_health_check = now;
        }

        /* 历史清理（每天一次） */
        if (now - last_history_cleanup > 86400) {
            alert_history_cleanup(30); /* 保留 30 天 */
            last_history_cleanup = now;
        }

        /* 检查是否需要轮询 */
        if (now - last_check >= check_interval) {
            LOG_DEBUG_T("Alertd", "MainLoop", "Check", "performing alert check");
            alert_manager_check_all(&g_config);
            last_check = now;

            /* 根据是否检测到异常，动态调整轮询间隔 */
            if (alert_manager_has_exception()) {
                check_interval = g_config.exception_interval;
                LOG_DEBUG_T("Alertd", "MainLoop", "Exception", "switched to exception interval: %d", check_interval);
            } else {
                check_interval = g_config.base_interval;
            }
        }

        sleep(1);
    }

    LOG_INFO_T("Alertd", "MainLoop", "Stop", "main loop stopped");
}

/* ============================================================
 * 主入口
 * ============================================================ */


/* ============================================================
 * R7: 本地系统健康检查（CPU/内存/磁盘 → 超阈值产生预警事件）
 * ============================================================ */
static void check_local_health(const alert_config_t *cfg) {
    if (!cfg) return;

    /* CPU 使用率（/proc/stat 双采样） */
    double cpu = 0.0;
    FILE *fp = fopen("/proc/stat", "r");
    if (fp) {
        unsigned long long t1 = 0, idle1 = 0, t2 = 0, idle2 = 0;
        unsigned long long u, n, s, i, w, irq, si, st;
        if (fscanf(fp, "cpu %llu %llu %llu %llu %llu %llu %llu %llu",
                   &u, &n, &s, &i, &w, &irq, &si, &st) == 8) {
            t1 = u + n + s + i + w + irq + si + st;
            idle1 = i + w;
        }
        fclose(fp);
        usleep(200000);
        fp = fopen("/proc/stat", "r");
        if (fp) {
            if (fscanf(fp, "cpu %llu %llu %llu %llu %llu %llu %llu %llu",
                       &u, &n, &s, &i, &w, &irq, &si, &st) == 8) {
                t2 = u + n + s + i + w + irq + si + st;
                idle2 = i + w;
            }
            fclose(fp);
        }
        if (t2 > t1) {
            double idle_d = (double)(idle2 - idle1);
            double total_d = (double)(t2 - t1);
            cpu = (1.0 - idle_d / total_d) * 100.0;
            if (cpu < 0) cpu = 0;
        }
    }

    /* 内存使用率（/proc/meminfo） */
    double mem = 0.0;
    fp = fopen("/proc/meminfo", "r");
    if (fp) {
        char line[128];
        unsigned long long total = 0, avail = 0;
        while (fgets(line, sizeof(line), fp)) {
            if (strncmp(line, "MemTotal:", 9) == 0) sscanf(line, "MemTotal: %llu", &total);
            else if (strncmp(line, "MemAvailable:", 13) == 0) sscanf(line, "MemAvailable: %llu", &avail);
        }
        fclose(fp);
        if (total > 0 && avail <= total) mem = (double)(total - avail) / (double)total * 100.0;
    }

    /* 磁盘使用率 */
    double disk = 0.0;
    struct statvfs stv;
    if (statvfs("/", &stv) == 0 && stv.f_blocks > 0) {
        double total = (double)stv.f_blocks * stv.f_frsize;
        double free_b = (double)stv.f_bfree * stv.f_frsize;
        if (total > 0) disk = (total - free_b) / total * 100.0;
    }

    /* 阈值判断 → 产生预警 */
    alert_event_t ev;
    if (cfg->cpu_threshold > 0 && cpu >= (double)cfg->cpu_threshold) {
        memset(&ev, 0, sizeof(ev));
        ev.type = ALERT_TYPE_HEALTH;
        ev.level = (cpu >= 90) ? 4 : 2;
        safe_strncpy(ev.source, "health", sizeof(ev.source));
        safe_snprintf(ev.description, sizeof(ev.description),
                      "CPU 使用率 %.1f%% 超过阈值 %d%%", cpu, cfg->cpu_threshold);
        ev.timestamp = time(NULL);
        alert_notify_send(&ev, cfg);
    }
    if (cfg->memory_threshold > 0 && mem >= (double)cfg->memory_threshold) {
        memset(&ev, 0, sizeof(ev));
        ev.type = ALERT_TYPE_HEALTH;
        ev.level = (mem >= 92) ? 4 : 2;
        safe_strncpy(ev.source, "health", sizeof(ev.source));
        safe_snprintf(ev.description, sizeof(ev.description),
                      "内存使用率 %.1f%% 超过阈值 %d%%", mem, cfg->memory_threshold);
        ev.timestamp = time(NULL);
        alert_notify_send(&ev, cfg);
    }
    if (cfg->disk_threshold > 0 && disk >= (double)cfg->disk_threshold) {
        memset(&ev, 0, sizeof(ev));
        ev.type = ALERT_TYPE_HEALTH;
        ev.level = (disk >= 95) ? 4 : 2;
        safe_strncpy(ev.source, "health", sizeof(ev.source));
        safe_snprintf(ev.description, sizeof(ev.description),
                      "磁盘使用率 %.1f%% 超过阈值 %d%%", disk, cfg->disk_threshold);
        ev.timestamp = time(NULL);
        alert_notify_send(&ev, cfg);
    }
}

int main(int argc, char **argv) {
    (void)argc;
    (void)argv;

    log_system_init();
    LOG_INFO_T("Alertd", "Main", "Start", "LING OS Alert Daemon v%s starting", version_get());

    /* 信号处理 */
    signal(SIGTERM, signal_handler);
    signal(SIGINT, signal_handler);

    /* 加载配置 */
    if (alert_config_load(&g_config) != 0) {
        LOG_ERROR_T("Alertd", "Main", "ConfigLoadFail", "using defaults");
        alert_config_set_defaults(&g_config);
    }
    alert_config_validate(&g_config);

    /* 初始化通知系统 */
    alert_notify_init(&g_config);

    /* 加载历史记录 */
    alert_history_init();

    /* 启动心跳线程 */
    if (pthread_create(&g_heartbeat_thread, NULL, heartbeat_thread_func, NULL) != 0) {
        LOG_ERROR_T("Alertd", "Main", "HeartbeatThreadFail", "cannot start heartbeat");
    }

    /* 主循环 */
    main_loop();

    /* 清理 */
    alert_history_cleanup_all();
    alert_notify_cleanup();
    LOG_INFO_T("Alertd", "Main", "Exit", "alertd exiting");
    return 0;
}