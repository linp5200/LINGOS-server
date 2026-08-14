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

int audio_init(const voice_config_t *config);
int audio_capture(audio_frame_t *frame);
void audio_feedback_beep(void);
void audio_cleanup(void);

#endif /* VOICE_AUDIO_INPUT_H */