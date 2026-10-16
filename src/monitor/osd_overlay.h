/**
 * @file    src/monitor/osd_overlay.h
 * @brief   OSD 叠加（时间戳 + 摄像头 ID）——5x7 点阵，真实像素绘制
 * @version LN-0.9.0
 * @par     架构依据（先生 2026-09-05 裁决 · docs/ARCHITECTURE_系统类目.md）：
 *            monitor = 采集/预览/录像/拍照/IR/OSD/存储；叠加在 monitor 侧完成（给人看）
 * @par     诚实红线：
 *            · 只对**未压缩帧**（YUYV/YUY2）逐像素真实绘制（直接写 Y 平面，U/V 置中性）
 *            · JPEG/MJPEG 帧需先解码才能绘制 —— 本模块不做假叠加，返回 -2 并如实记录
 *            · 无外部字体依赖：内置 5x7 点阵（ASCII 0x20–0x5F，小写自动转大写）
 */

#ifndef MONITOR_OSD_OVERLAY_H
#define MONITOR_OSD_OVERLAY_H

#include <stddef.h>
#include "frame.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 叠加结果码 */
#define OSD_OK             0    /* 已真实绘制 */
#define OSD_SKIP_DISABLED  1    /* 配置关闭——未绘制（非错误） */
#define OSD_ERR_INVALID   (-1)  /* 参数/帧尺寸非法 */
#define OSD_ERR_FORMAT    (-2)  /* 帧格式不支持（JPEG 需解码——诚实跳过） */

/**
 * @brief 初始化
 * @param enable     1=启用叠加
 * @param camera_id  摄像头 ID（写入角标；可为 NULL）
 * @param scale      点阵放大倍数 1–4（<=0 取 1）
 * @return 0 成功
 */
int osd_overlay_init(int enable, const char *camera_id, int scale);

/**
 * @brief 在帧上叠加时间戳 + 摄像头 ID（原地修改 frame->data）
 * @return OSD_OK / OSD_SKIP_DISABLED / OSD_ERR_INVALID / OSD_ERR_FORMAT
 */
int osd_overlay_apply(lingos_frame_t *frame);

/**
 * @brief 低位绘制接口（快照/录像/测试可复用）：YUYV 缓冲画一行文本
 * @return 已绘制字符数；-1 = 参数非法（越界坐标会被裁剪）
 */
int osd_overlay_draw_text_yuyv(unsigned char *buf, int width, int height,
                               int x, int y, const char *text, int scale);

/** @brief 当前是否启用（1/0） */
int osd_overlay_enabled(void);

/** @brief 诚实状态文本（上次绘制结果/跳过原因） */
const char *osd_overlay_status(void);

/** @brief 关闭（不释放全局资源——本模块无堆分配） */
void osd_overlay_cleanup(void);

#ifdef __cplusplus
}
#endif

#endif /* MONITOR_OSD_OVERLAY_H */
