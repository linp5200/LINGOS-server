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
    char camera_source[16];   /* 【0.2.2】v4l2 / rtsp / mjpeg（先生裁决：RTSP 走 Python 拉流） */
    char rtsp_url[256];       /* 【0.2.2】RTSP/MJPEG 流地址（camera_source=rtsp 时用） */
    int rtsp_frame_port;      /* 【0.2.2】rtsp_streamer 帧通道端口 */
    int rtsp_http_port;       /* 【0.2.2】rtsp_streamer 预览 MJPEG 端口 */
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