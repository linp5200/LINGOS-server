/**
 * @file    spatial_mapper.h
 * @brief   空间映射器头文件
 * @version LN-B-4.3.0.0
 */

#ifndef VISION_SPATIAL_MAPPER_H
#define VISION_SPATIAL_MAPPER_H

#include "detection_engine.h"
#include "visiond.h"

typedef struct {
    char name[64];
    double points[8][2];
    int point_count;
} zone_t;

void spatial_init(const vision_config_t *config);
int spatial_load_calibration(const char *path);
int spatial_map(detection_result_t *result);
const char* spatial_match_zone(double wx, double wy);
/* 【2026-10-11】标定状态查询：未标定时 world 坐标只是近似比例，消费方需据此判断可信度 */
int spatial_is_calibrated(void);
void spatial_cleanup(void);

#endif /* VISION_SPATIAL_MAPPER_H */