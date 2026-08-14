/**
 * @file    src/voice/voiced.h
 * @brief   语音控制独立子进程头文件
 * @version LN-B-4.3.0.0
 * @changes 添加 voice_config_load、voice_config_set_defaults 声明
 */

#ifndef VOICE_VOICED_H
#define VOICE_VOICED_H

#include <stdint.h>

typedef struct {
    int audio_device;           /* 音频设备索引 */
    char device_path[128];      /* /dev/snd/ 或 PulseAudio 设备 */
    int sample_rate;
    int channels;
    int bits_per_sample;
    char wakeword[32];          /* 唤醒词，默认"诺克" */
    char model_path[256];       /* Vosk 模型路径 */
    int enable_feedback;        /* 是否启用反馈音 */
    int feedback_volume;        /* 反馈音量 0-100 */
} voice_config_t;

/* ====== 配置加载函数（新增声明） ====== */
int voice_config_load(voice_config_t *cfg);
void voice_config_set_defaults(voice_config_t *cfg);

#endif /* VOICE_VOICED_H */