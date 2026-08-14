/**
 * @file    voice_config.c
 * @brief   语音模块配置加载（从 voiced.c 中分离）
 * @version LN-B-5.0.0.0
 */

#include "voiced.h"
#include "voice_config.h"
#include "../common/safe_string.h"
#include "../common/data_path.h"
#include "../lib/log_extra.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

int voice_config_load(voice_config_t *cfg) {
    LOG_DEBUG_T("VoiceConfig", "Load", "Enter", "loading voice config");

    if (!cfg) return -1;

    voice_config_set_defaults(cfg);

    const char *root = lingos_data_root();
    char path[512];
    safe_snprintf(path, sizeof(path), "%s/system/config/voice.conf", root);

    FILE *fp = fopen(path, "r");
    if (!fp) {
        LOG_WARN_T("VoiceConfig", "Load", "NotFound", "voice.conf not found, using defaults");
        return 0;
    }

    char line[256];
    while (fgets(line, sizeof(line), fp)) {
        if (line[0] == '#' || line[0] == '\n') continue;
        char key[64], val[64];
        if (sscanf(line, "%63[^=]=%63s", key, val) == 2) {
            if (strcmp(key, "audio_device") == 0) cfg->audio_device = atoi(val);
            else if (strcmp(key, "device_path") == 0) safe_strncpy(cfg->device_path, val, sizeof(cfg->device_path));
            else if (strcmp(key, "sample_rate") == 0) cfg->sample_rate = atoi(val);
            else if (strcmp(key, "channels") == 0) cfg->channels = atoi(val);
            else if (strcmp(key, "bits_per_sample") == 0) cfg->bits_per_sample = atoi(val);
            else if (strcmp(key, "wakeword") == 0) safe_strncpy(cfg->wakeword, val, sizeof(cfg->wakeword));
            else if (strcmp(key, "model_path") == 0) safe_strncpy(cfg->model_path, val, sizeof(cfg->model_path));
            else if (strcmp(key, "enable_feedback") == 0) cfg->enable_feedback = atoi(val);
            else if (strcmp(key, "feedback_volume") == 0) cfg->feedback_volume = atoi(val);
        }
    }
    fclose(fp);

    LOG_INFO_T("VoiceConfig", "Load", "OK", "voice config loaded");
    return 0;
}

void voice_config_set_defaults(voice_config_t *cfg) {
    if (!cfg) return;
    cfg->audio_device = 0;
    safe_strncpy(cfg->device_path, "default", sizeof(cfg->device_path));
    cfg->sample_rate = 16000;
    cfg->channels = 1;
    cfg->bits_per_sample = 16;
    safe_strncpy(cfg->wakeword, "诺克", sizeof(cfg->wakeword));
    safe_strncpy(cfg->model_path, "/LINGOS/models/vosk-model-small-cn-0.22", sizeof(cfg->model_path));
    cfg->enable_feedback = 1;
    cfg->feedback_volume = 80;
}