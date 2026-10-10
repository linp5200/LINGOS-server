/**
 * @file    src/monitor/camera_input.h
 * @brief   摄像头输入层头文件（监控系统 monitor——采集归 monitor，画面给人看）
 * @version LN-0.9.0
 * @par     2026-10-10 P4 拆分：由 src/vision/ 迁入 src/monitor/（先生 2026-09-05 裁决）
 *          采集（V4L2/RTSP）→ monitor；检测/标定/追踪 → ai_vision（经帧通道消费）
 * @par     兼容：帧类型 camera_frame_t 保留（frame.h 别名），历史调用方不改
 */

#ifndef MONITOR_CAMERA_INPUT_H
#define MONITOR_CAMERA_INPUT_H

#include "frame.h"
#include "monitor_config.h"

/**
 * 采集进程身份（设备权限闸门用）。
 * 默认 "visiond"——沿用历史摄像头权限集（permission_whitelist 已播种
 * wl_seed_app("visiond", ...) = PERM_CAMERA）；待白名单播种 "monitor" 后，
 * 由 monitord 调 camera_set_app_id("monitor") 切换。
 * 语义保持 fail-closed：无 CAMERA 权限即拒绝采集（不静默放行）。
 */
void camera_set_app_id(const char *app_id);
const char *camera_get_app_id(void);

int camera_init(const monitor_config_t *config);
int camera_capture(camera_frame_t *frame);
void camera_cleanup(void);

#endif /* MONITOR_CAMERA_INPUT_H */
