/**
 * @file    audio_input.h
 * @brief   音频输入层头文件
 * @version LN-B-4.3.0.0
 */

#ifndef VOICE_AUDIO_INPUT_H
#define VOICE_AUDIO_INPUT_H

#include "voiced.h"
#include <stdint.h>
#include <time.h>

typedef struct {
    unsigned char *data;
    int size;
    int sample_rate;
    int channels;
    int bits_per_sample;
    time_t timestamp;
} audio_frame_t;

/* 【2026-10-11 P1】采集身份（麦克风权限闸门用；默认 "voiced"） */
void audio_set_app_id(const char *app_id);
const char *audio_get_app_id(void);

int audio_init(const voice_config_t *config);
int audio_capture(audio_frame_t *frame);
void audio_feedback_beep(void);
void audio_cleanup(void);

#endif /* VOICE_AUDIO_INPUT_H */