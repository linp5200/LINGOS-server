/**
 * @file    tracker.h
 * @brief   物体追踪头文件
 * @version LN-B-4.3.0.0
 */

#ifndef VISION_TRACKER_H
#define VISION_TRACKER_H

#include "detection_engine.h"

typedef struct {
    int id;
    char label[32];
    double x, y;
    double width, height;
} track_info_t;

void tracker_init(void);
void tracker_update(detection_result_t *results, int count);
int tracker_get_active(track_info_t *out, int max_count);
int tracker_find_by_id(int id, track_info_t *out);
void tracker_cleanup(void);

#endif /* VISION_TRACKER_H */