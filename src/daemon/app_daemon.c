/**
 * @file    app_daemon.c
 * @brief   应用守护进程（定期检查并重启崩溃的应用）
 * @version 2.0.0.0
 */

#include "app_daemon.h"
#include "../core/app_runner.h"
#include "../core/app_sandbox.h"
#include "log_extra.h"
#include <pthread.h>
#include <unistd.h>
#include <string.h>
#include <stdlib.h>
#include <dirent.h>
#include <signal.h>

#define MAX_MONITORED_APPS 32
#define CHECK_INTERVAL_SEC 10

static pthread_t daemon_thread;
static volatile int running = 0;
static volatile int stop_flag = 0;
static char monitored_apps[MAX_MONITORED_APPS][128];
static int monitored_count = 0;

static void* daemon_loop(void *arg) {
    (void)arg;
    running = 1;
    LOG_INFO_T("AppDaemon", "Loop", "Start", "monitoring started");
    while (!stop_flag) {
        for (int i = 0; i < CHECK_INTERVAL_SEC && !stop_flag; i++) sleep(1);
        if (stop_flag) break;
        for (int i = 0; i < monitored_count; i++) {
            if (!app_is_running(monitored_apps[i])) {
                LOG_INFO_T("AppDaemon", "Restart", "Detected", "%s is dead, restarting", monitored_apps[i]);
                app_start_sandboxed(monitored_apps[i]);
            }
        }
    }
    running = 0;
    LOG_INFO_T("AppDaemon", "Loop", "Stop", "monitoring stopped");
    return NULL;
}

int app_daemon_start(void) {
    if (running) return 0;
    stop_flag = 0;
    int ret = pthread_create(&daemon_thread, NULL, daemon_loop, NULL);
    if (ret != 0) {
        LOG_ERROR_T("AppDaemon", "Start", "Fail", "pthread_create error %d", ret);
        return -1;
    }
    return 0;
}

void app_daemon_stop(void) {
    if (!running) return;
    stop_flag = 1;
    pthread_join(daemon_thread, NULL);
}

int app_daemon_is_running(void) {
    return running;
}

void app_daemon_monitor_add(const char *app_name) {
    if (monitored_count >= MAX_MONITORED_APPS) {
        LOG_WARN_T("AppDaemon", "Add", "Overflow", "too many monitored apps");
        return;
    }
    strncpy(monitored_apps[monitored_count], app_name, sizeof(monitored_apps[0])-1);
    monitored_count++;
    LOG_INFO_T("AppDaemon", "Add", "OK", "%s", app_name);
}