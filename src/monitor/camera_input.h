/**
 * @file    camera_input.h
 * @brief   摄像头输入头文件
 * @version LN-B-4.3.0.0
 */

#ifndef VISION_CAMERA_INPUT_H
#define VISION_CAMERA_INPUT_H

#include "visiond.h"
#include <stdint.h>
#include <time.h>

typedef struct {
    unsigned char *data;
    int width;
    int height;
    int size;
    time_t timestamp;
} camera_frame_t;

int camera_init(const vision_config_t *config);
int camera_capture(camera_frame_t *frame);
void camera_cleanup(void);

#endif /* VISION_CAMERA_INPUT_H */