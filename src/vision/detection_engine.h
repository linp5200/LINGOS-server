/**
 * @file    src/vision/detection_engine.h
 * @brief   检测引擎头文件
 * @version LN-B-4.3.0.0
 * @changes 添加 track_id 字段（配置向导集成）
 */

#ifndef VISION_DETECTION_ENGINE_H
#define VISION_DETECTION_ENGINE_H

#include "camera_input.h"
#include "visiond.h"
#include <time.h>

typedef struct {
    int x;
    int y;
    int width;
    int height;
    int class_id;
    char label[32];
    double confidence;
    double world_x;   /* 世界坐标X（cm） */
    double world_y;   /* 世界坐标Y（cm） */
    time_t timestamp;
    int track_id;     /* 新增：追踪器 ID */
} detection_result_t;

int detection_init(const vision_config_t *config);
int detection_run(const camera_frame_t *frame, detection_result_t *results, int max_count);
const char* detection_get_class_name(int class_id);
void detection_cleanup(void);

#endif /* VISION_DETECTION_ENGINE_H */