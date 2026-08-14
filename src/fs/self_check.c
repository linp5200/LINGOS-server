/**
 * @file    src/fs/self_check.c
 * @brief   启动自检（调用新 check_manager）
 * @version LN-B-5.1.2.6-rc
 * @changes 使用新的 check_manager 替代原有自检逻辑；
 *          保持 API 兼容性。
 */

#include "self_check.h"
#include "../common/safe_string.h"
#include "../common/data_path.h"
#include "../common/lang.h"
#include "../common/init_cache.h"
#include "../lib/log_extra.h"
#include "../core/version.h"
#include "../drivers/uart.h"
#include "../health/check_manager.h"
#include "../health/check_items.h"
#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <time.h>
#include <errno.h>
#include <pthread.h>

int need_configuration = 0;
static char last_error[512] = {0};
static int background_check_done = 0;
static int background_check_result = 0;
static int g_initialized = 0;
static pthread_t g_background_thread;

static void set_error(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vsnprintf(last_error, sizeof(last_error), fmt, args);
    va_end(args);
    LOG_ERROR_T("SelfCheck", "Error", "Set", "%s", last_error);
}

const char* get_last_selfcheck_error(void) {
    return last_error;
}

/* ============================================================
 * 自检主函数
 * ============================================================ */
int self_check_and_sync(void) {
    LOG_INFO_T("SelfCheck", "Start", "begin", "Running quick self-check (using new check_manager)");

    last_error[0] = '\0';

    // 初始化检查管理器
    check_manager_init();

    // 注册所有检查项
    if (check_items_register_all() != 0) {
        set_error("Failed to register check items");
        need_configuration = 0;
        return -1;
    }

    // 运行快速检查
    check_summary_t summary;
    int ret = check_manager_run_quick(&summary);

    // 获取是否需要配置
    need_configuration = summary.need_configuration;

    // 打印结果
    if (summary.failed > 0 || summary.errors > 0) {
        LOG_WARN_T("SelfCheck", "Result", "Failures", "failed=%d, errors=%d",
                   summary.failed, summary.errors);
        if (summary.need_configuration) {
            LOG_WARN_T("SelfCheck", "Result", "NeedConfig", "configuration required");
        }
    } else {
        LOG_INFO_T("SelfCheck", "Result", "OK", "all checks passed");
    }

    // 保存到缓存
    init_cache_t cache;
    if (init_cache_load(&cache) == 0) {
        cache.configs_ok = (summary.failed == 0 && summary.errors == 0);
        cache.timestamp = time(NULL);
        init_cache_save(&cache);
    }

    LOG_INFO_T("SelfCheck", "End", "Result", "need_configuration=%d", need_configuration);
    return ret;
}

/* ============================================================
 * 缓存检查
 * ============================================================ */
int is_env_cache_valid(void) {
    init_cache_t cache;
    if (init_cache_load(&cache) != 0) return 0;
    return init_cache_is_valid(&cache);
}

int get_env_cache(int *python_ok, int *libcurl_ok, int *microhttpd_ok) {
    init_cache_t cache;
    if (init_cache_load(&cache) != 0) return -1;
    if (python_ok) *python_ok = cache.python_ok;
    if (libcurl_ok) *libcurl_ok = cache.libcurl_ok;
    if (microhttpd_ok) *microhttpd_ok = cache.microhttpd_ok;
    return 0;
}

int set_env_cache(int python_ok, int libcurl_ok, int microhttpd_ok) {
    init_cache_t cache;
    if (init_cache_load(&cache) != 0) {
        memset(&cache, 0, sizeof(cache));
    }
    cache.python_ok = python_ok;
    cache.libcurl_ok = libcurl_ok;
    cache.microhttpd_ok = microhttpd_ok;
    cache.timestamp = time(NULL);
    return init_cache_save(&cache);
}

/* ============================================================
 * 后台异步检查
 * ============================================================ */
static void* background_check_thread(void *arg) {
    (void)arg;
    LOG_DEBUG_T("SelfCheck", "Background", "Start", "background thread started");
    check_summary_t summary;
    int ret = check_manager_run_all(&summary);
    background_check_result = ret;
    background_check_done = 1;
    LOG_DEBUG_T("SelfCheck", "Background", "Done", "background thread finished, ret=%d", ret);
    return NULL;
}

int async_self_check(void) {
    LOG_DEBUG_T("SelfCheck", "Async", "enter", "Starting async self-check");
    if (background_check_done) {
        return background_check_result;
    }

    int ret = pthread_create(&g_background_thread, NULL, background_check_thread, NULL);
    if (ret != 0) {
        LOG_ERROR_T("SelfCheck", "Async", "ThreadFail", "failed to create background thread: %s", strerror(ret));
        return -1;
    }
    pthread_detach(g_background_thread);
    LOG_INFO_T("SelfCheck", "Async", "Started", "background check thread started");
    return 0;
}