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

    /* 【0.7.0-hf2】降级式初始化（原 REPORT_ERROR → abort 崩溃循环——
     *   audio/wakeword 初始化失败时进程自杀被 watchdog 反复重启）。 */
    if (audio_init(&g_config) != 0) {
        LOG_WARN_T("Voiced", "VoiceLoop", "AudioInitFail", "audio_init failed (continuing in standby)");
    }

    if (wakeword_init(&g_config) != 0) {
        LOG_WARN_T("Voiced", "VoiceLoop", "WakewordInitFail", "wakeword_init failed (continuing in standby)");
    }

    /* 【第9期】用真实运行时值（wakeword_get_current）而非配置快照——wakeword_set 运行时改名也能反映 */
    LOG_INFO_T("Voiced", "VoiceLoop", "Start", "voice monitoring started, wakeword='%s', mic_app_id='%s'",
               wakeword_get_current(), audio_get_app_id());
    /* 【第9期·隐私透明】麦克风行为必须对用户明示：只做本机唤醒检测，音频不出设备。 */
    LOG_INFO_T("Voiced", "VoiceLoop", "Privacy",
               "microphone capture is local-only (wakeword detection); no audio is uploaded");

    while (g_running) {
        audio_frame_t frame;
        if (audio_capture(&frame) != 0) {
            /* 无麦克风 → 待机（audio_input 内部 60s 限频日志；此处 5s 节拍） */
            for (int i = 0; i < 5 && g_running; i++) sleep(1);
            continue;
        }

        int detected = wakeword_detect(&frame);
        if (detected) {
            LOG_INFO_T("Voiced", "VoiceLoop", "Wakeword", "wakeword detected! (word='%s')",
                       wakeword_get_current());
            audio_feedback_beep();
            /* 【第9期·禁假成功/占位】原实现固定执行字面量 "hello"——用户说什么都一样，
             *   看起来像"AI 已理解"实则只打一条横幅。现显式走"唤醒"语义，并把
             *   "STT 未接线"这一事实告知用户（每进程一次，不刷屏）+ 给出可行动路径。 */
            voice_cmd_execute("wake");
            static int stt_hint_shown = 0;
            if (!stt_hint_shown) {
                stt_hint_shown = 1;
                LOG_WARN_T("Voiced", "VoiceLoop", "SttNotWired",
                           "语音识别(STT)未接线：唤醒仅本机提示，不解析用户语句；"
                           "需要对话请在 App「AI 对话」输入（麦克风音频仅本机处理，不出设备）");
            }
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