/**
 * @file    src/core/background_init.c
 * @brief   后台初始化线程实现
 * @version LN-0.4.3
 * @par     核心协议：容错编程（任何失败都不影响主进程）
 */

#include "background_init.h"
#include "registry.h"
#include "security_config.h"
#include "defense_mode.h"
#include "config_loader.h"
#include "log_extra.h"
#include <pthread.h>
#include <stdlib.h>
#include <unistd.h>

static pthread_t g_bg_thread;
static volatile int g_bg_running = 0;
static volatile int g_bg_done = 0;
static volatile int g_bg_stop = 0;

/**
 * @brief 后台初始化线程主函数
 */
static void* background_init_thread(void *arg) {
    (void)arg;
    LOG_INFO_T("BackgroundInit", "Thread", "Start", "background initialization thread started");
    g_bg_running = 1;
    g_bg_done = 0;

    /* ============================================================
     * 1. registry_init（带超时保护，registry.c 内部已实现）
     * ============================================================ */
    LOG_DEBUG_T("BackgroundInit", "Step", "Registry", "initializing registry");
    int reg_ret = registry_init();
    if (reg_ret != 0) {
        LOG_WARN_T("BackgroundInit", "Step", "Registry", "registry_init failed, continuing");
    } else {
        LOG_DEBUG_T("BackgroundInit", "Step", "Registry", "registry_init completed");
    }

    /* ============================================================
     * 2. security_config_load（容错）
     * ============================================================ */
    LOG_DEBUG_T("BackgroundInit", "Step", "Security", "loading security config");
    int sec_ret = security_config_load();
    if (sec_ret != 0) {
        LOG_WARN_T("BackgroundInit", "Step", "Security", "security_config_load failed, using defaults");
        security_config_set_defaults();
    } else {
        LOG_DEBUG_T("BackgroundInit", "Step", "Security", "security_config_load completed");
    }

    /* ============================================================
     * 3. defense_mode_apply_current（容错）
     * ============================================================ */
    LOG_DEBUG_T("BackgroundInit", "Step", "Defense", "applying defense mode");
    defense_mode_apply_current();
    LOG_DEBUG_T("BackgroundInit", "Step", "Defense", "defense mode applied");

    /* ============================================================
     * 4. config_load_all（容错）
     * ============================================================ */
    LOG_DEBUG_T("BackgroundInit", "Step", "Config", "loading all configs");
    int cfg_ret = config_load_all();
    if (cfg_ret != 0) {
        LOG_WARN_T("BackgroundInit", "Step", "Config", "config_load_all had errors");
    } else {
        LOG_DEBUG_T("BackgroundInit", "Step", "Config", "config_load_all completed");
    }

    g_bg_done = 1;
    g_bg_running = 0;
    LOG_INFO_T("BackgroundInit", "Thread", "Done", "background initialization completed");
    return NULL;
}

int start_background_initialization(void) {
    if (g_bg_running) {
        LOG_DEBUG_T("BackgroundInit", "Start", "Already", "background thread already running");
        return 0;
    }

    g_bg_stop = 0;
    g_bg_done = 0;

    if (pthread_create(&g_bg_thread, NULL, background_init_thread, NULL) != 0) {
        LOG_ERROR_T("BackgroundInit", "Start", "ThreadFail", "failed to create background thread");
        return -1;
    }

    LOG_INFO_T("BackgroundInit", "Start", "OK", "background initialization thread started");
    return 0;
}

void stop_background_initialization(void) {
    if (!g_bg_running && g_bg_done) {
        LOG_DEBUG_T("BackgroundInit", "Stop", "AlreadyDone", "background init already completed");
        return;
    }

    if (!g_bg_running) {
        LOG_DEBUG_T("BackgroundInit", "Stop", "NotRunning", "background thread not running");
        return;
    }

    g_bg_stop = 1;
    LOG_DEBUG_T("BackgroundInit", "Stop", "Waiting", "waiting for background thread to finish");

    /* 等待线程完成（最多 5 秒） */
    int wait_count = 0;
    while (g_bg_running && wait_count < 50) {
        usleep(100000);
        wait_count++;
    }

    if (g_bg_running) {
        LOG_WARN_T("BackgroundInit", "Stop", "Timeout", "background thread did not stop, detaching");
        pthread_detach(g_bg_thread);
    } else {
        pthread_join(g_bg_thread, NULL);
        LOG_DEBUG_T("BackgroundInit", "Stop", "OK", "background thread stopped");
    }

    g_bg_running = 0;
}

int background_init_is_running(void) {
    return g_bg_running;
}

int background_init_is_done(void) {
    return g_bg_done;
}