/**
 * @file    scan_daemon.c
 * @brief   后台扫描守护线程（定期扫描自定义技能风险）
 * @version 2.0.0.0
 */

#include "scan_daemon.h"
#include "scan_config.h"
#include "scan_analyzer.h"
#include "log_extra.h"
#include "uart.h"
#include "../common/lang.h"
#include <pthread.h>
#include <unistd.h>
#include <sched.h>
#include <signal.h>
#include <errno.h>
#include <time.h>

static pthread_t scan_thread;
static volatile int thread_running = 0;
static volatile int thread_should_stop = 0;

static void* scan_thread_func(void *arg) {
    (void)arg;
    thread_running = 1;
    LOG_INFO_T("ScanDaemon", "Thread", "Start", "background scanner started");
    while (!thread_should_stop) {
        if (scan_is_enabled()) {
            int interval = scan_get_interval();
            for (int i = 0; i < interval && !thread_should_stop; i++) {
                sleep(1);
            }
            if (thread_should_stop) break;

            LOG_INFO_T("ScanDaemon", "Scan", "Start", "performing full system scan");
            scan_result_t result;
            if (scan_perform_full(&result) == 0) {
                if (result.high_risk_count > 0) {
                    uart_puts(tr("\n[SCAN] Warning: ", "\n[扫描] 警告："));
                    char buf[256];
                    snprintf(buf, sizeof(buf), "High-risk skills detected: %d\n", result.high_risk_count);
                    uart_puts(buf);
                }
                LOG_INFO_T("ScanDaemon", "Scan", "Result", "%s", result.summary);
                scan_set_last_completed(time(NULL));
            } else {
                LOG_ERROR_T("ScanDaemon", "Scan", "Fail", "scan failed");
            }
        } else {
            sleep(1);
        }
    }
    thread_running = 0;
    LOG_INFO_T("ScanDaemon", "Thread", "Stop", "scanner stopped");
    return NULL;
}

int scan_daemon_start(void) {
    if (thread_running) return 0;
    thread_should_stop = 0;
    int ret = pthread_create(&scan_thread, NULL, scan_thread_func, NULL);
    if (ret != 0) {
        LOG_ERROR_T("ScanDaemon", "Start", "Fail", "pthread_create error %d", ret);
        return -1;
    }
    struct sched_param param;
    param.sched_priority = 0;
    pthread_setschedparam(scan_thread, SCHED_IDLE, &param);
    return 0;
}

void scan_daemon_stop(void) {
    if (!thread_running) return;
    thread_should_stop = 1;
    pthread_join(scan_thread, NULL);
}

int scan_daemon_is_running(void) {
    return thread_running;
}

int scan_daemon_trigger_now(void) {
    if (!thread_running) {
        LOG_WARN_T("ScanDaemon", "Trigger", "NotRunning", "scanner not running");
        return -1;
    }
    scan_result_t result;
    if (scan_perform_full(&result) == 0) {
        uart_puts(tr("[SCAN] Manual scan completed.\n", "[扫描] 手动扫描完成。\n"));
        char buf[256];
        snprintf(buf, sizeof(buf), tr("Total: %d, High: %d, Medium: %d, Low: %d\n",
                                      "总计: %d, 高风险: %d, 中风险: %d, 低风险: %d\n"),
                 result.total_skills, result.high_risk_count,
                 result.medium_risk_count, result.low_risk_count);
        uart_puts(buf);
        return 0;
    }
    return -1;
}