/**
 * @file    vision_config.c
 * @brief   视觉模块配置加载（从 visiond.c 中分离）
 * @version LN-B-5.0.0.0
 */

#include "visiond.h"
#include "vision_config.h"
#include "../common/safe_string.h"
#include "../common/data_path.h"
#include "../lib/log_extra.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

int vision_config_load(vision_config_t *cfg) {
    LOG_DEBUG_T("VisionConfig", "Load", "Enter", "loading vision config");

    if (!cfg) return -1;

    vision_config_set_defaults(cfg);

    const char *root = lingos_data_root();
    char path[512];
    safe_snprintf(path, sizeof(path), "%s/system/config/vision.conf", root);

    FILE *fp = fopen(path, "r");
    if (!fp) {
        LOG_WARN_T("VisionConfig", "Load", "NotFound", "vision.conf not found, using defaults");
        return 0;
    }

    char line[256];
    while (fgets(line, sizeof(line), fp)) {
        if (line[0] == '#' || line[0] == '\n') continue;
        char key[64], val[64];
        if (sscanf(line, "%63[^=]=%63s", key, val) == 2) {
            if (strcmp(key, "camera_device") == 0) cfg->camera_device = atoi(val);
            else if (strcmp(key, "device_path") == 0) safe_strncpy(cfg->device_path, val, sizeof(cfg->device_path));
            else if (strcmp(key, "camera_source") == 0) safe_strncpy(cfg->camera_source, val, sizeof(cfg->camera_source));
            else if (strcmp(key, "rtsp_url") == 0) safe_strncpy(cfg->rtsp_url, val, sizeof(cfg->rtsp_url));
            else if (strcmp(key, "rtsp_frame_port") == 0) cfg->rtsp_frame_port = atoi(val);
            else if (strcmp(key, "rtsp_http_port") == 0) cfg->rtsp_http_port = atoi(val);
            else if (strcmp(key, "width") == 0) cfg->width = atoi(val);
            else if (strcmp(key, "height") == 0) cfg->height = atoi(val);
            else if (strcmp(key, "fps") == 0) cfg->fps = atoi(val);
            else if (strcmp(key, "confidence_threshold") == 0) cfg->confidence_threshold = atof(val);
            else if (strcmp(key, "enable_tracking") == 0) cfg->enable_tracking = atoi(val);
            else if (strcmp(key, "enable_spatial_mapping") == 0) cfg->enable_spatial_mapping = atoi(val);
            else if (strcmp(key, "model_path") == 0) safe_strncpy(cfg->model_path, val, sizeof(cfg->model_path));
            else if (strcmp(key, "calibration_path") == 0) safe_strncpy(cfg->calibration_path, val, sizeof(cfg->calibration_path));
            else if (strcmp(key, "enable_yolo") == 0) cfg->enable_yolo = atoi(val);
        }
    }
    fclose(fp);

    LOG_INFO_T("VisionConfig", "Load", "OK", "vision config loaded");
    return 0;
}

void vision_config_set_defaults(vision_config_t *cfg) {
    if (!cfg) return;
    cfg->camera_device = 0;
    safe_strncpy(cfg->device_path, "/dev/video0", sizeof(cfg->device_path));
    safe_strncpy(cfg->camera_source, "v4l2", sizeof(cfg->camera_source));
    safe_strncpy(cfg->rtsp_url, "", sizeof(cfg->rtsp_url));
    cfg->rtsp_frame_port = 8890;
    cfg->rtsp_http_port = 8891;
    cfg->width = 640;
    cfg->height = 480;
    cfg->fps = 10;
    cfg->confidence_threshold = 0.5;
    cfg->enable_tracking = 1;
    cfg->enable_spatial_mapping = 1;
    safe_strncpy(cfg->model_path, "/LINGOS/models/yolov8n.pt", sizeof(cfg->model_path));
    safe_strncpy(cfg->calibration_path, "/LINGOS/data/vision/calibration.json", sizeof(cfg->calibration_path));
    cfg->enable_yolo = 1;
}