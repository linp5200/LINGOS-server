/**
 * @file    src/vision/visiond.h
 * @brief   视觉检测独立子进程头文件
 * @version LN-B-5.0.0.0
 * @changes 添加 enable_yolo 字段
 */

#ifndef VISION_VISIOND_H
#define VISION_VISIOND_H

#include <stdint.h>

typedef struct {
    int camera_device;
    char device_path[128];
    int width;
    int height;
    int fps;
    double confidence_threshold;
    int enable_tracking;
    int enable_spatial_mapping;
    char model_path[256];
    char calibration_path[256];
    int enable_yolo;          /* 新增：是否启用 YOLO 服务 */
} vision_config_t;

int vision_config_load(vision_config_t *cfg);
void vision_config_set_defaults(vision_config_t *cfg);

#endif /* VISION_VISIOND_H */