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
#include "../net/http_client.h"
#include <sys/stat.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <pthread.h>
#include <unistd.h>
#include <signal.h>

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
    LOG_INFO_T("UpdateAuto", "Check", "Start", "checking for updates (via local ai_server update_check)");

    /* 【2026-09-18 接线】原实现直连 repo.lingos.local:80（死地址 + 明文 HTTP——
     * 该检查从未成功过）。现改为走本机 lingosd /api/cmd 代理 →
     * ai_server 的 update_check（真实源：repo.conf 的 repo_url / GitHub Releases）。
     * 每日线程逻辑不变；有新版本 → 写通知中心（sys_notify.jsonl）。 */
    char resp[8192] = {0};
    int rc = http_post_json("http://127.0.0.1:8080/api/cmd",
                            "{\"cmd\":\"update_check\"}", resp, sizeof(resp), 15);
    if (rc != 0) {
        LOG_WARN_T("UpdateAuto", "Check", "LocalFail",
                   "update_check via 8080 failed rc=%d (lingosd not ready?)", rc);
        return 0;
    }

    /* 宽松解析 update_available 标志（json.dumps 默认带空格——两种写法都认） */
    int avail = -1;
    const char *k = strstr(resp, "update_available");
    if (k) {
        const char *t = strstr(k, "true");
        const char *f = strstr(k, "false");
        if (t && (!f || t < f)) avail = 1;
        else if (f) avail = 0;
    }

    if (avail == 1) {
        /* 有新版本 → 通知中心（sys_notify.jsonl——与 notify syscall 同文件约定） */
        const char *root = lingos_data_root();
        char ndir[512], nfile[512];
        safe_snprintf(ndir, sizeof(ndir), "%s/data/notifications", root);
        mkdir(ndir, 0755);
        safe_snprintf(nfile, sizeof(nfile), "%s/sys_notify.jsonl", ndir);
        FILE *nf = fopen(nfile, "a");
        if (nf) {
            fprintf(nf, "{\"ts\":%ld,\"title\":\"Update available\","
                        "\"body\":\"A new LINGOS version is available — open the update page.\","
                        "\"level\":\"normal\"}\n", (long)time(NULL));
            fclose(nf);
        }
        LOG_INFO_T("UpdateAuto", "Check", "Available", "update available (notification written)");
        return 1;
    }
    if (avail == 0) {
        LOG_INFO_T("UpdateAuto", "Check", "UpToDate", "no update available");
        return 0;
    }
    LOG_WARN_T("UpdateAuto", "Check", "ParseFail",
               "cannot parse update_check response (no update_available flag)");
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