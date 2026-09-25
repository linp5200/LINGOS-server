/**
 * @file    audio_input.c
 * @brief   音频输入层（麦克风采集）—— 真实设备检测 + arecord 捕获
 * @version LN-0.7.0
 * @par     核心协议：容错编程（设备不可用时诚实降级——不伪造数据）
 *
 * 【0.7.0-hf2 重写】原实现为"模拟音频（rand 噪音）"——纯假数据空转。
 * 现实现：
 *   · 有声卡（/dev/snd）+ arecord → 真实 PCM 捕获（arecord 管道）
 *   · 无声卡/无 arecord → 诚实降级：capture 返回 -1（voiced 待机重试，
 *     日志 60s 限频）；绝不返回伪造音频（防唤醒链路被假数据干扰）
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
static int g_audio_device_ok = 0;      /* 真实设备可用性 */
static char g_audio_device[128] = {0};
static int g_audio_sample_rate = 16000;
static FILE *g_rec_pipe = NULL;        /* arecord 管道 */
static unsigned char g_frame_buf[65536];

/* ============================================================
 * 初始化音频
 * ============================================================ */

int audio_init(const voice_config_t *config) {
    LOG_DEBUG_T("Audio", "Init", "Enter", "device=%s", config->device_path);

    safe_strncpy(g_audio_device, config->device_path, sizeof(g_audio_device));
    g_audio_sample_rate = config->sample_rate > 0 ? config->sample_rate : 16000;

    /* 【0.7.0-hf2】真实设备检测（ALSA 声卡 + arecord 工具） */
    {
        int has_dev = (access("/dev/snd", F_OK) == 0) || (access("/dev/dsp", F_OK) == 0);
        int has_rec = (access("/usr/bin/arecord", X_OK) == 0) ||
                      (access("/bin/arecord", X_OK) == 0) ||
                      (access("/usr/local/bin/arecord", X_OK) == 0);
        if (has_dev && has_rec) {
            g_audio_device_ok = 1;
            LOG_INFO_T("Audio", "Init", "OK",
                       "audio device ready (arecord), device=%s rate=%d",
                       g_audio_device, g_audio_sample_rate);
        } else {
            g_audio_device_ok = 0;
            LOG_WARN_T("Audio", "Init", "NoDevice",
                       "audio unavailable (dev=%d arecord=%d) — microphone standby (honest, no fake data)",
                       has_dev, has_rec);
        }
    }

    g_audio_initialized = 1;   /* 初始化成功（voiced 进入待机而非崩溃） */
    return 0;
}

/* ============================================================
 * 采集音频帧（真实 PCM 或诚实失败）
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

    /* 诚实降级：无设备 → 失败（voiced 主循环待机；日志 60s 限频） */
    if (!g_audio_device_ok) {
        static time_t last_log = 0;
        time_t now = time(NULL);
        if (now - last_log >= 60) {
            LOG_WARN_T("Audio", "Capture", "Standby",
                       "microphone unavailable — capture skipped (configure audio device to enable)");
            last_log = now;
        }
        return -1;
    }

    /* 懒启动 arecord 原始 PCM 管道 */
    if (!g_rec_pipe) {
        char cmd[256];
        safe_snprintf(cmd, sizeof(cmd),
                      "arecord -q -f S16_LE -r %d -c 1 -t raw 2>/dev/null",
                      g_audio_sample_rate);
        g_rec_pipe = popen(cmd, "r");
        if (!g_rec_pipe) {
            LOG_WARN_T("Audio", "Capture", "PopenFail", "arecord start failed: %s", strerror(errno));
            g_audio_device_ok = 0;
            return -1;
        }
        setvbuf(g_rec_pipe, NULL, _IONBF, 0);
        LOG_INFO_T("Audio", "Capture", "RecStart", "arecord pipeline started (rate=%d)", g_audio_sample_rate);
    }

    /* 读取一帧 PCM（sample_rate 字节/秒 × 2 字节 × 约 0.1s ≈ 帧大小） */
    int want = g_audio_sample_rate * 2 / 10;   /* 100ms 帧 */
    if (want > (int)sizeof(g_frame_buf)) want = (int)sizeof(g_frame_buf);
    size_t got = fread(g_frame_buf, 1, (size_t)want, g_rec_pipe);
    if (got == 0) {
        /* 管道断（arecord 退出）→ 重启下一轮 */
        pclose(g_rec_pipe);
        g_rec_pipe = NULL;
        return -1;
    }

    frame->data = g_frame_buf;
    frame->size = (int)got;
    frame->sample_rate = g_audio_sample_rate;
    frame->channels = 1;
    frame->bits_per_sample = 16;
    frame->timestamp = time(NULL);
    return 0;
}

/* ============================================================
 * 播放反馈音
 * ============================================================ */

void audio_feedback_beep(void) {
    printf("\a");
    fflush(stdout);
}

/* ============================================================
 * 清理
 * ============================================================ */

void audio_cleanup(void) {
    if (g_rec_pipe) {
        pclose(g_rec_pipe);
        g_rec_pipe = NULL;
    }
    g_audio_initialized = 0;
    g_audio_device_ok = 0;
    LOG_DEBUG_T("Audio", "Cleanup", "OK", "audio cleaned up");
}
