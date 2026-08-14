/**
 * @file    vision_memory.h
 * @brief   视觉位置记忆存储头文件
 * @version LN-B-4.3.0.0
 */

#ifndef VISION_VISION_MEMORY_H
#define VISION_VISION_MEMORY_H

#include "detection_engine.h"

typedef struct {
    char label[64];
    double world_x;
    double world_y;
    time_t timestamp;
    int track_id;
} vision_location_t;

int vision_memory_init(void);
int vision_memory_save(const detection_result_t *result);
int vision_memory_locate(const char *label, vision_location_t *out, int max_count);
void vision_memory_cleanup(void);

#endif /* VISION_VISION_MEMORY_H */