/**
 * @file    wakeword_engine.h
 * @brief   Vosk唤醒词检测头文件
 * @version LN-B-4.3.0.0
 */

#ifndef VOICE_WAKEWORD_ENGINE_H
#define VOICE_WAKEWORD_ENGINE_H

#include "audio_input.h"
#include "voiced.h"

int wakeword_init(const voice_config_t *config);
int wakeword_detect(const audio_frame_t *frame);
const char* wakeword_get_current(void);
int wakeword_set(const char *word);
void wakeword_cleanup(void);

#endif /* VOICE_WAKEWORD_ENGINE_H */