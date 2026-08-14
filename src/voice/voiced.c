/**
 * @file    src/voice/voiced.c
 * @brief   语音控制独立子进程（lingos_voiced）
 * @version LN-B-5.0.0.0
 * @changes 添加 voice_config_load、voice_config_set_defaults 实现
 * @par     核心协议：防弹编程（独立进程 + 心跳监控）
 */

#include "voiced.h"
#include "voice_config.h"
#include "audio_input.h"
#include "wakeword_engine.h"
#include "voice_cmds.h"
#include "../common/error_report.h"
#include "../common/safe_string.h"
#include "../common/data_path.h"
#include "../common/lang.h"
#include "../lib/log_extra.h"
#include "../core/version.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <pthread.h>
#include <ctype.h>
#include <time.h>

/* ============================================================
 * 全局状态
 * ============================================================ */

static volatile int g_running = 1;
static volatile int g_heartbeat = 0;
static pthread_t g_heartbeat_thread;
static pthread_t g_voice_thread;
static voice_config_t g_config;



/* ============================================================
 * 信号处理
 * ============================================================ */

static void signal_handler(int sig) {
    LOG_INFO_T("Voiced", "Signal", "Received", "signal=%d", sig);
    if (sig == SIGTERM || sig == SIGINT) {
        g_running = 0;
    }
}

/* ============================================================
 * 心跳线程
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
 * 语音主循环
 * ============================================================ */

static void* voice_loop_thread(void *arg) {
    (void)arg;

    if (audio_init(&g_config) != 0) {
        REPORT_ERROR("voiced: audio_init failed");
    }

    if (wakeword_init(&g_config) != 0) {
        REPORT_ERROR("voiced: wakeword_init failed");
    }

    LOG_INFO_T("Voiced", "VoiceLoop", "Start", "voice monitoring started, wakeword='%s'",
               g_config.wakeword[0] ? g_config.wakeword : tr("诺克", "Nook"));

    while (g_running) {
        audio_frame_t frame;
        if (audio_capture(&frame) != 0) {
            LOG_WARN_T("Voiced", "VoiceLoop", "CaptureFail", "audio capture failed");
            sleep(1);
            continue;
        }

        int detected = wakeword_detect(&frame);
        if (detected) {
            LOG_INFO_T("Voiced", "VoiceLoop", "Wakeword", "wakeword detected!");
            audio_feedback_beep();
            voice_cmd_execute("hello");
        }

        usleep(100000);
    }

    wakeword_cleanup();
    audio_cleanup();

    return NULL;
}

/* ============================================================
 * 主入口
 * ============================================================ */

int main(int argc, char **argv) {
    (void)argc;
    (void)argv;

    log_system_init();
    LOG_INFO_T("Voiced", "Main", "Start", "LING OS Voice Daemon v%s starting", LINGOS_VERSION);

    signal(SIGTERM, signal_handler);
    signal(SIGINT, signal_handler);

    if (voice_config_load(&g_config) != 0) {
        LOG_WARN_T("Voiced", "Main", "ConfigLoadFail", "using defaults");
        voice_config_set_defaults(&g_config);
    }

    LOG_DEBUG_T("Voiced", "Main", "ConfigStep", "voice config step available");

    pthread_create(&g_heartbeat_thread, NULL, heartbeat_thread_func, NULL);
    pthread_create(&g_voice_thread, NULL, voice_loop_thread, NULL);

    pthread_join(g_voice_thread, NULL);

    LOG_INFO_T("Voiced", "Main", "Exit", "voiced exiting");
    return 0;
}