/**
 * @file    audio_input.c
 * @brief   音频输入层（麦克风采集）
 * @version LN-B-5.0.0.0
 * @par     核心协议：容错编程（设备不可用时降级）
 * @changes PortAudio/ALSA 集成框架；安全字符串替换
 */

#include "audio_input.h"
#include "../common/error_report.h"
#include "../common/safe_string.h"
#include "../common/lang.h"
#include "../lib/log_extra.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <errno.h>
#include <time.h>

static int g_audio_initialized = 0;
static char g_audio_device[128] = {0};
static int g_audio_sample_rate = 16000;

/* ============================================================
 * 初始化音频
 * ============================================================ */

int audio_init(const voice_config_t *config) {
    LOG_DEBUG_T("Audio", "Init", "Enter", "device=%s", config->device_path);

    safe_strncpy(g_audio_device, config->device_path, sizeof(g_audio_device));
    g_audio_sample_rate = config->sample_rate > 0 ? config->sample_rate : 16000;

    /* 实际应调用 PortAudio/ALSA 初始化 */
    /* 当前为模拟实现 */

    g_audio_initialized = 1;
    LOG_INFO_T("Audio", "Init", "OK", "audio initialized (simulated), device=%s, rate=%d",
               g_audio_device, g_audio_sample_rate);
    return 0;
}

/* ============================================================
 * 采集音频帧
 * ============================================================ */

int audio_capture(audio_frame_t *frame) {
    if (!g_audio_initialized) {
        LOG_ERROR_T("Audio", "Capture", "NotInit", "audio not initialized");
        return -1;
    }

    if (!frame) {
        LOG_ERROR_T("Audio", "Capture", "Invalid", "frame is NULL");
        return -1;
    }

    frame->timestamp = time(NULL);
    frame->size = g_audio_sample_rate * 2;
    frame->sample_rate = g_audio_sample_rate;
    frame->channels = 1;
    frame->bits_per_sample = 16;

    /* 实际应从设备读取数据，当前使用模拟数据 */
    static unsigned char dummy_data[65536];
    frame->data = dummy_data;

    for (int i = 0; i < frame->size && i < (int)sizeof(dummy_data); i++) {
        dummy_data[i] = rand() & 0xFF;
    }

    LOG_DEBUG_T("Audio", "Capture", "OK", "captured %d bytes (simulated)", frame->size);
    return 0;
}

/* ============================================================
 * 播放反馈音
 * ============================================================ */

void audio_feedback_beep(void) {
    LOG_DEBUG_T("Audio", "Feedback", "Beep", "playing feedback beep");
    printf("\a");
    fflush(stdout);
}

/* ============================================================
 * 清理
 * ============================================================ */

void audio_cleanup(void) {
    g_audio_initialized = 0;
    LOG_DEBUG_T("Audio", "Cleanup", "OK", "audio cleaned up");
}