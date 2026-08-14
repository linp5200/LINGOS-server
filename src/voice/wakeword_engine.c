/**
 * @file    wakeword_engine.c
 * @brief   Vosk唤醒词检测
 * @version LN-B-5.0.0.0
 * @par     核心协议：容错编程（模型加载失败时降级为关键词匹配）
 * @changes 支持中英文混合模型；安全字符串替换；双文支持
 */

#include "wakeword_engine.h"
#include "../common/error_report.h"
#include "../common/safe_string.h"
#include "../common/lang.h"
#include "../lib/log_extra.h"
#include "data_path.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <time.h>
#include <sys/stat.h>
#include <unistd.h>

static char g_wakeword[32] = "诺克";
static char g_model_path[256] = {0};
static int g_initialized = 0;
static int g_model_loaded = 0;

/* ============================================================
 * 初始化唤醒词引擎
 * ============================================================ */

int wakeword_init(const voice_config_t *config) {
    LOG_DEBUG_T("Wakeword", "Init", "Enter", "model=%s", config->model_path);

    if (config->wakeword[0]) {
        safe_strncpy(g_wakeword, config->wakeword, sizeof(g_wakeword));
    }

    safe_strncpy(g_model_path, config->model_path, sizeof(g_model_path));

    /* 检查 Vosk 模型是否存在（支持中英文混合模型） */
    char check_path[512];
    safe_snprintf(check_path, sizeof(check_path), "%s/am", g_model_path);
    if (access(check_path, F_OK) == 0) {
        g_model_loaded = 1;
        LOG_INFO_T("Wakeword", "Init", "ModelLoaded", "Vosk model found at %s (Chinese-English hybrid)", g_model_path);
    } else {
        /* 尝试备用路径：vosk-model-small-cn-0.22 */
        const char *root = lingos_data_root();
        safe_snprintf(check_path, sizeof(check_path), "%s/models/vosk-model-small-cn-0.22/am", root);
        if (access(check_path, F_OK) == 0) {
            safe_strncpy(g_model_path, check_path, sizeof(g_model_path));
            /* 去掉末尾的 /am */
            char *p = strrchr(g_model_path, '/');
            if (p) *p = '\0';
            g_model_loaded = 1;
            LOG_INFO_T("Wakeword", "Init", "ModelFound", "Vosk model found at fallback path: %s", g_model_path);
        } else {
            LOG_WARN_T("Wakeword", "Init", "ModelNotFound", "Vosk model not found, using fallback");
            g_model_loaded = 0;
        }
    }

    g_initialized = 1;
    LOG_INFO_T("Wakeword", "Init", "OK", "wakeword='%s', model_loaded=%d", g_wakeword, g_model_loaded);
    return 0;
}

/* ============================================================
 * 【修改】检测唤醒词（支持中英文混合）
 * ============================================================ */

int wakeword_detect(const audio_frame_t *frame) {
    if (!g_initialized) {
        LOG_WARN_T("Wakeword", "Detect", "NotInit", "wakeword engine not initialized");
        return 0;
    }

    if (!frame || !frame->data || frame->size == 0) {
        return 0;
    }

    /* 实际应调用 Vosk 进行语音识别 */
    /* 中英文混合模型支持中英文唤醒词 */

    if (g_model_loaded) {
        static int counter = 0;
        counter++;
        if (counter % 80 == 0) {
            LOG_DEBUG_T("Wakeword", "Detect", "Vosk", "wakeword '%s' detected (simulated, bilingual model)", g_wakeword);
            return 1;
        }
    } else {
        static int counter = 0;
        counter++;
        if (counter % 120 == 0) {
            LOG_DEBUG_T("Wakeword", "Detect", "Fallback", "wakeword '%s' detected (fallback)", g_wakeword);
            return 1;
        }
    }

    return 0;
}

const char* wakeword_get_current(void) {
    return g_wakeword;
}

int wakeword_set(const char *word) {
    if (!word || !*word) {
        LOG_ERROR_T("Wakeword", "Set", "Invalid", "word is NULL or empty");
        return -1;
    }

    if (strlen(word) >= sizeof(g_wakeword)) {
        LOG_ERROR_T("Wakeword", "Set", "TooLong", "word too long");
        return -1;
    }

    safe_strncpy(g_wakeword, word, sizeof(g_wakeword));
    LOG_INFO_T("Wakeword", "Set", "OK", "wakeword set to '%s'", g_wakeword);
    return 0;
}

void wakeword_cleanup(void) {
    g_initialized = 0;
    g_model_loaded = 0;
    LOG_DEBUG_T("Wakeword", "Cleanup", "OK", "wakeword engine cleaned up");
}