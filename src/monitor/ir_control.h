/**
 * @file    src/monitor/ir_control.h
 * @brief   红外控制（IR-cut / IR-LED / Day-Night）——真实 V4L2 ioctl，失败诚实降级
 * @version LN-0.9.0
 * @par     架构依据（先生 2026-09-05 裁决 · docs/ARCHITECTURE_系统类目.md）：
 *            monitor = 采集(V4L2/RTSP)/预览/录像/拍照/IR/OSD/存储 —— 独立进程
 * @par     诚实红线（先生 2026-09-25 审计）：
 *            · 只设置驱动**真实暴露**的 V4L2 控制项（VIDIOC_QUERYCTRL/VIDIOC_S_CTRL）
 *            · 驱动没有 IR 控制项 → 返回失败 + 明确原因，绝不伪造"已切换"状态
 *            · 不猜测寄存器/不做平台魔法（仅标准 V4L2 控制面）
 */

#ifndef MONITOR_IR_CONTROL_H
#define MONITOR_IR_CONTROL_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* IR 工作模式（配置面取值；KEEP = 不干预硬件） */
typedef enum {
    IR_MODE_KEEP = 0,   /* 不干预硬件（默认）：只枚举/报告，最安全 */
    IR_MODE_OFF  = 1,   /* 白天模式：IR-cut 合上 / 红外灯关 */
    IR_MODE_ON   = 2,   /* 夜晚模式：IR-cut 打开 / 红外灯开 */
    IR_MODE_AUTO = 3    /* 驱动自动（仅当驱动暴露 auto 菜单项时可用） */
} ir_mode_t;

/**
 * @brief 绑定已打开的 V4L2 设备并枚举控制项
 * @param v4l2_fd camera_input 的 V4L2 fd（RTSP 模式传 -1）
 * @return 0 = 发现至少一个 IR 相关控制项；-1 = 无（诚实：驱动未暴露）
 */
int ir_control_init(int v4l2_fd);

/**
 * @brief 设置 IR 模式（真实 VIDIOC_S_CTRL + 回读校验）
 * @param mode ir_mode_t（IR_MODE_KEEP 恒成功——不触碰硬件）
 * @return 0 = 已生效（回读一致）；-1 = 失败（原因见 ir_control_status）
 */
int ir_control_set_mode(int mode);

/** @brief 当前模式（回读硬件；未知返回 -1） */
int ir_control_get_mode(void);

/** @brief 是否发现 IR 控制项：1 有 / 0 无 */
int ir_control_available(void);

/** @brief 诚实状态文本（人类可读；含失败原因） */
const char *ir_control_status(void);

/**
 * @brief 导出枚举到的控制项清单（诊断用）
 * @return 写入字节数；-1 = 缓冲不足
 */
int ir_control_list_controls(char *out, size_t out_sz);

/** @brief 卸载（不改变硬件状态） */
void ir_control_cleanup(void);

#ifdef __cplusplus
}
#endif

#endif /* MONITOR_IR_CONTROL_H */
