/**
 * @file    src/monitor/monitor_config.h
 * @brief   监控系统配置（monitor.json 为主；vision.conf 历史键回退）
 * @version LN-0.9.0
 * @par     架构依据：docs/ARCHITECTURE_系统类目.md §三
 *          「配置由 config 系统统一管（monitor.conf 由 config 读写——monitor 只管采集）」
 *          本模块只做**读**与目录准备；写入由 Python 命令面（monitor_config_set）负责。
 * @par     兼容：monitor.json 不存在时读历史 vision.conf 的采集键（不减少功能）。
 */

#ifndef MONITOR_MONITOR_CONFIG_H
#define MONITOR_MONITOR_CONFIG_H

#include <stddef.h>

typedef struct {
    /* ---- 采集 ---- */
    int  camera_device;             /* 历史字段（vision.conf: camera_device） */
    char device_path[128];          /* V4L2 设备节点 */
    char camera_source[16];         /* v4l2 / rtsp / mjpeg */
    char rtsp_url[256];             /* RTSP/MJPEG 流地址 */
    int  rtsp_frame_port;           /* rtsp_streamer 帧通道端口 */
    int  rtsp_http_port;            /* rtsp_streamer 预览端口 */
    int  width;
    int  height;
    int  fps;
    char camera_id[32];             /* 摄像头 ID（多路预留，默认 cam0） */

    /* ---- 帧通道（发布端） ---- */
    char frame_channel_path[256];
    int  frame_channel_max_clients;

    /* ---- 存储 ---- */
    char snapshot_dir[256];
    char record_dir[256];
    int  record_segment_min;        /* 录像分段分钟 */
    int  free_space_reserve_mb;     /* 剩余空间低于此值停止录像（诚实保护） */

    /* ---- 控制面（Python 命令面调用） ---- */
    char control_socket_path[256];

    /* ---- 预览元数据（MJPEG 渲染在 Python/App 侧） ---- */
    int  preview_port;
    int  preview_quality;

    /* ---- 多路（预留：当前主实例单路采集，配置保留多路语义） ---- */
    int  multi_camera;

    /* ---- OSD 叠加（monitor 侧真实绘制：时间戳 + 摄像头 ID） ---- */
    int  osd_enabled;               /* 1=在未压缩帧上叠加（默认 1；JPEG 需解码→诚实跳过） */
    int  osd_scale;                 /* 5x7 点阵放大倍数 1–4（默认 1） */

    /* ---- 红外控制（真实 V4L2 控制面；驱动无 IR 项则诚实失败） ---- */
    char ir_mode[16];               /* keep(默认，不干预) / off(白天) / on(夜晚) / auto */
} monitor_config_t;

void monitor_config_set_defaults(monitor_config_t *cfg);
/**
 * 读配置：先取历史 vision.conf 采集键，再用 monitor.json 覆盖（JSON 优先）。
 * @return 0 = 至少应用了默认值（永不失败——配置缺失不算错误，降级为默认）
 */
int  monitor_config_load(monitor_config_t *cfg);
/** 保证 snapshot_dir / record_dir 存在（mkdir -p 语义） */
int  monitor_config_ensure_dirs(const monitor_config_t *cfg);

#endif /* MONITOR_MONITOR_CONFIG_H */
