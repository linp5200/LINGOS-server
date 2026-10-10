/**
 * @file    src/monitor/frame.h
 * @brief   LINGOS 帧数据定义（监控系统 monitor 拥有；AI 识别引擎经帧通道消费）
 * @version LN-0.9.0
 * @par     架构依据：docs/ARCHITECTURE_系统类目.md（先生 2026-09-05 裁决）
 *          monitor（给人看：采集/预览/快照/录像/存储）
 *            └─ 帧 → frame_channel（共享/套接字）
 *          ai_vision（给 AI 用：检测/标定/追踪）消费 monitor 帧
 *          二者经"帧通道 + 事件"解耦，不互相内含。
 * @par     兼容：历史类型名 camera_frame_t 保留为别名（0.7.x 调用方不改）
 */

#ifndef MONITOR_FRAME_H
#define MONITOR_FRAME_H

#include <stdint.h>
#include <stddef.h>
#include <time.h>

/* ============================================================
 * 帧像素格式（帧通道线上枚举——只增不改，保证协议向后兼容）
 * ============================================================ */
typedef enum {
    FRAME_FMT_UNKNOWN = 0,
    FRAME_FMT_YUYV    = 1,   /* V4L2 原始 YUY2（2 字节/像素） */
    FRAME_FMT_JPEG    = 2,   /* 单帧 JPEG（RTSP/MJPEG 拉流） */
    FRAME_FMT_MJPEG   = 3    /* MJPEG 流中的一帧（与 JPEG 同构，仅标记来源） */
} frame_format_t;

typedef struct {
    unsigned char *data;        /* 帧数据（归属者负责释放/复用，不转移所有权） */
    int            width;
    int            height;
    int            size;        /* 字节数 */
    frame_format_t format;      /* 像素格式 */
    uint32_t       seq;         /* 采集序号（monitor 递增；消费者据此判丢帧） */
    time_t         timestamp;   /* 采集时刻（秒） */
    char           camera_id[32]; /* 摄像头 ID（多路：cam0/cam1…） */
} lingos_frame_t;

/* 历史类型名（0.7.x 及更早为 camera_frame_t——保留别名，不破坏既有调用） */
typedef lingos_frame_t camera_frame_t;

static inline const char *frame_format_name(frame_format_t fmt) {
    switch (fmt) {
        case FRAME_FMT_YUYV:  return "yuyv";
        case FRAME_FMT_JPEG:  return "jpeg";
        case FRAME_FMT_MJPEG: return "mjpeg";
        default:              return "unknown";
    }
}

static inline int frame_is_jpeg(const lingos_frame_t *f) {
    if (!f || !f->data || f->size < 3) return 0;
    if (f->format == FRAME_FMT_JPEG || f->format == FRAME_FMT_MJPEG) return 1;
    /* 按魔数兜底判定（FF D8 FF = JPEG SOI） */
    return (f->data[0] == 0xFF && f->data[1] == 0xD8 && f->data[2] == 0xFF);
}

#endif /* MONITOR_FRAME_H */
