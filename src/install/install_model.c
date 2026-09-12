/**
 * @file    src/install/install_model.c
 * @brief   模型下载实现（支持断点续传）
 * @version LN-0.4.3
 * @par     核心协议：C1, C-C
 */

#include "install_model.h"
#include "install_speed.h"
#include "install_progress.h"
#include "../common/safe_string.h"
#include "../common/lang.h"
#include "../lib/log_extra.h"
#include "../drivers/uart.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <errno.h>
#include <time.h>
#include "../net/http_client.h"

static char g_last_error[256] = {0};
static char g_model_mirror[256] = "";

/* ============================================================
 * 模型定义
 * ============================================================ */
typedef struct model_def {
    const char *name;
    const char *url;
    const char *dest_path;
    size_t expected_size;
} model_def_t;

static model_def_t g_models[] = {
    {"yolov8n",
     "https://github.com/ultralytics/assets/releases/download/v0.0.0/yolov8n.pt",
     "/LINGOS/models/yolov8n.pt",
     6200000},
    {"vosk-model-small-cn-0.22",
     "https://alphacephei.com/vosk/models/vosk-model-small-cn-0.22.zip",
     "/LINGOS/models/vosk-model-small-cn-0.22",
     42000000},
    {NULL, NULL, NULL, 0}
};

/* ============================================================
 * CURL 写入回调
 * ============================================================ */
typedef struct download_ctx {
    FILE *fp;
    size_t total;
    size_t downloaded;
    speed_calc_t speed;
    int last_progress;
} download_ctx_t;

/* 【0.5.0】原 curl 写回调与进度上下文已由 net/http_client 内部处理，此处不再需要 */

/* ============================================================
 * 执行下载（使用 libcurl）
 * ============================================================ */
static int curl_download(const char *url, const char *dest_path, const char *model_name) {
    (void)model_name;

    // 确保目录存在
    char dir[512];
    safe_strncpy(dir, dest_path, sizeof(dir));
    char *last_slash = strrchr(dir, '/');
    if (last_slash) {
        *last_slash = '\0';
        char cmd[512];
        safe_snprintf(cmd, sizeof(cmd), "mkdir -p '%s' 2>/dev/null", dir);
        system(cmd);
    }

    /* 【0.5.0】改用内置 HTTP 客户端（去掉 libcurl 硬依赖，适配 Ubuntu 22.04~25.10）
     *  - 有 libcurl → dlopen 加载，支持 https + 跟随重定向
     *  - 无 libcurl → 纯 socket 回退（仅 http://）
     *  注：断点续传能力由 http_download 内部决定（当前实现为整文件下载；
     *      失败会删半成品，避免脏文件）。 */

    int dl_rc = http_download(url, dest_path, 600);

    if (dl_rc == 0) {
        struct stat st;
        if (stat(dest_path, &st) == 0 && st.st_size > 0) {
            LOG_INFO_T("InstallModel", "Download", "OK",
                       "%s downloaded (%ld bytes)", model_name, (long)st.st_size);
            install_progress_finish_item(1);
            return 0;
        }
    }

    safe_snprintf(g_last_error, sizeof(g_last_error),
                  "Download failed (rc=%d, %s)", dl_rc,
                  http_curl_available() ? "curl" : "socket-only");
    install_progress_finish_item(0);
    return -1;
}

/* ============================================================
 * 下载模型
 * ============================================================ */
int install_model_download(const char *model_name) {
    if (!model_name) {
        safe_strncpy(g_last_error, "Invalid model name", sizeof(g_last_error));
        return -1;
    }

    LOG_INFO_T("InstallModel", "Download", "Start", "model=%s", model_name);

    // 查找模型
    model_def_t *model = NULL;
    for (int i = 0; g_models[i].name; i++) {
        if (strcmp(g_models[i].name, model_name) == 0) {
            model = &g_models[i];
            break;
        }
    }
    if (!model) {
        safe_snprintf(g_last_error, sizeof(g_last_error), "Unknown model: %s", model_name);
        return -1;
    }

    // 检查是否已存在
    if (install_model_is_ready(model_name)) {
        LOG_DEBUG_T("InstallModel", "Download", "AlreadyReady", "%s", model_name);
        install_progress_finish_item(1);
        return 0;
    }

    install_progress_set_item(model_name, 1, 1);

    // 构建 URL（支持镜像）
    char url[512];
    if (g_model_mirror[0] != '\0') {
        safe_snprintf(url, sizeof(url), "%s/%s", g_model_mirror, model->name);
    } else {
        safe_strncpy(url, model->url, sizeof(url));
    }

    return curl_download(url, model->dest_path, model_name);
}

/* ============================================================
 * 检查模型是否就绪
 * ============================================================ */
int install_model_is_ready(const char *model_name) {
    if (!model_name) return 0;
    for (int i = 0; g_models[i].name; i++) {
        if (strcmp(g_models[i].name, model_name) == 0) {
            struct stat st;
            if (stat(g_models[i].dest_path, &st) != 0) return 0;
            if (st.st_size < 1024) return 0;
            return 1;
        }
    }
    return 0;
}

/* ============================================================
 * 检查所有模型
 * ============================================================ */
int install_model_check_all(void) {
    int missing = 0;
    for (int i = 0; g_models[i].name; i++) {
        if (!install_model_is_ready(g_models[i].name)) {
            missing++;
            LOG_DEBUG_T("InstallModel", "CheckAll", "Missing", "%s", g_models[i].name);
        }
    }
    return missing > 0 ? -1 : 0;
}

/* ============================================================
 * 获取错误信息
 * ============================================================ */
const char* install_model_get_last_error(void) {
    return g_last_error;
}

/* ============================================================
 * 设置镜像源
 * ============================================================ */
void install_model_set_mirror(const char *mirror) {
    if (mirror) {
        safe_strncpy(g_model_mirror, mirror, sizeof(g_model_mirror));
    }
}

/* ============================================================
 * 清空缓存
 * ============================================================ */
void install_model_clear_cache(void) {
    g_last_error[0] = '\0';
}