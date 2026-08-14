/**
 * @file    update_auto_check.c
 * @brief   自动检查更新（定时任务）
 * @version LN-B-5.0.0.0
 * @par     核心协议：容错编程（检查失败不影响主流程）
 * @changes 线程 join 修复；HTTP 请求使用 tcp_client；安全字符串替换
 */

#include "update_auto_check.h"
#include "../common/safe_string.h"
#include "../common/data_path.h"
#include "../common/lang.h"
#include "../lib/log_extra.h"
#include "../net/tcp_client.h"
#include <sys/stat.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <pthread.h>
#include <unistd.h>
#include <signal.h>

#define REPO_URL "repo.lingos.local"
#define REPO_PATH "/version"
#define CHECK_INTERVAL 86400
#define UPDATE_CHECK_FILE "/LINGOS/state/last_update_check"

static pthread_t g_check_thread;
static volatile int g_running = 0;
static volatile int g_stop = 0;

/* ============================================================
 * 读取上次检查时间
 * ============================================================ */

static time_t get_last_check_time(void) {
    const char *root = lingos_data_root();
    char path[512];
    safe_snprintf(path, sizeof(path), "%s%s", root, UPDATE_CHECK_FILE);

    FILE *fp = fopen(path, "r");
    if (!fp) return 0;

    time_t t;
    if (fscanf(fp, "%ld", (long*)&t) != 1) t = 0;
    fclose(fp);
    return t;
}

/* ============================================================
 * 保存检查时间
 * ============================================================ */

static void save_check_time(time_t t) {
    const char *root = lingos_data_root();
    char dir[512];
    safe_snprintf(dir, sizeof(dir), "%s/state", root);
    mkdir(dir, 0755);

    char path[512];
    safe_snprintf(path, sizeof(path), "%s%s", root, UPDATE_CHECK_FILE);
    FILE *fp = fopen(path, "w");
    if (fp) {
        fprintf(fp, "%ld\n", (long)t);
        fclose(fp);
    }
}

/* ============================================================
 * 【修改】检查更新（使用 tcp_client）
 * ============================================================ */

int update_auto_check_now(void) {
    LOG_INFO_T("UpdateAuto", "Check", "Start", "checking for updates");

    char request[512];
    safe_snprintf(request, sizeof(request),
        "GET %s HTTP/1.0\r\n"
        "Host: %s\r\n"
        "Connection: close\r\n"
        "\r\n",
        REPO_PATH, REPO_URL);

    char response[4096] = {0};
    int ret = tcp_send_recv(REPO_URL, 80, request, response, sizeof(response), 10000);

    if (ret != 0) {
        LOG_WARN_T("UpdateAuto", "Check", "HTTPFail", "tcp_send_recv returned %d", ret);
        return 0;
    }

    char *body = strstr(response, "\r\n\r\n");
    if (!body) {
        LOG_WARN_T("UpdateAuto", "Check", "ParseFail", "no HTTP body");
        return 0;
    }
    body += 4;

    const char *latest = strstr(body, "\"latest\":\"");
    if (latest) {
        latest += 10;
        char version[32];
        int i = 0;
        while (*latest && *latest != '\"' && i < 31) {
            version[i++] = *latest++;
        }
        version[i] = '\0';

        LOG_INFO_T("UpdateAuto", "Check", "Latest", "latest version: %s", version);
        return 1;
    }

    LOG_WARN_T("UpdateAuto", "Check", "Fail", "failed to parse response");
    return 0;
}

/* ============================================================
 * 后台检查线程
 * ============================================================ */

static void* check_thread_func(void *arg) {
    (void)arg;
    g_running = 1;
    LOG_INFO_T("UpdateAuto", "Thread", "Start", "auto check thread started");

    while (!g_stop) {
        time_t now = time(NULL);
        time_t last = get_last_check_time();

        if (now - last >= CHECK_INTERVAL) {
            LOG_DEBUG_T("UpdateAuto", "Thread", "Check", "performing check");
            update_auto_check_now();
            save_check_time(now);
        }

        for (int i = 0; i < 3600 && !g_stop; i++) {
            sleep(1);
        }
    }

    g_running = 0;
    LOG_INFO_T("UpdateAuto", "Thread", "Stop", "auto check thread stopped");
    return NULL;
}

/* ============================================================
 * 启动自动检查
 * ============================================================ */

int update_auto_check_start(void) {
    LOG_INFO_T("UpdateAuto", "Start", "Enter", "starting auto check thread");

    if (g_running) {
        LOG_WARN_T("UpdateAuto", "Start", "AlreadyRunning", "thread already running");
        return 0;
    }

    g_stop = 0;
    if (pthread_create(&g_check_thread, NULL, check_thread_func, NULL) != 0) {
        LOG_ERROR_T("UpdateAuto", "Start", "ThreadFail", "pthread_create failed");
        return -1;
    }

    LOG_INFO_T("UpdateAuto", "Start", "OK", "auto check started");
    return 0;
}

/* ============================================================
 * 【修改】停止自动检查（正确 join）
 * ============================================================ */

void update_auto_check_stop(void) {
    LOG_INFO_T("UpdateAuto", "Stop", "Enter", "stopping auto check thread");

    if (!g_running) {
        LOG_WARN_T("UpdateAuto", "Stop", "NotRunning", "thread not running");
        return;
    }

    g_stop = 1;
    /* 等待线程退出（最多5秒） */
    int wait_count = 0;
    while (g_running && wait_count < 50) {
        usleep(100000);
        wait_count++;
    }
    if (g_running) {
        LOG_WARN_T("UpdateAuto", "Stop", "Timeout", "thread did not stop, forcing cancel");
        pthread_cancel(g_check_thread);
        pthread_join(g_check_thread, NULL);
    } else {
        pthread_join(g_check_thread, NULL);
    }

    LOG_INFO_T("UpdateAuto", "Stop", "OK", "auto check stopped");
}