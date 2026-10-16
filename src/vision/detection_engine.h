/**
 * @file    src/vision/detection_engine.h
 * @brief   检测引擎头文件
 * @version LN-B-4.3.0.0
 * @changes 添加 track_id 字段（配置向导集成）
 */

#ifndef VISION_DETECTION_ENGINE_H
#define VISION_DETECTION_ENGINE_H

#include "../monitor/camera_input.h"   /* 【2026-10-10】采集层已迁至 monitor（先生 09-05 裁决）*/
#include "visiond.h"
#include <time.h>

typedef struct {
    int x;
    int y;
    int width;
    int height;
    int class_id;
    char label[32];
    double confidence;
    double world_x;   /* 世界坐标X（cm） */
    double world_y;   /* 世界坐标Y（cm） */
    time_t timestamp;
    int track_id;     /* 新增：追踪器 ID */
} detection_result_t;

int detection_init(const vision_config_t *config);
int detection_run(const camera_frame_t *frame, detection_result_t *results, int max_count);
const char* detection_get_class_name(int class_id);
void detection_cleanup(void);

/* ============================================================
 * YOLO NCNN（C 侧）—— 诚实结论 + 预留 C ABI shim 接口
 * ============================================================
 * 【结论 · 2026-10-16 静态评估】
 *   · ncnn 是 C++ 库（UPSTREAM 无官方 C API）；本仓库构建链为**纯 C**
 *     （Makefile 无 CXX/g++ 规则），且构建环境无第三方 AI 库依赖
 *     （/usr/include/ncnn 不存在，无 libncnn.so）——与本项目
 *     "Ubuntu 22.04~25.10 可移植、不硬依赖第三方 soname"的设计冲突。
 *   · 因此**不能**在本构建中内联真实 ncnn 推理；绝不写假推理/假模型。
 *   · 预留接口（真实生效，非占位）：外部 C++ 侧把 ncnn 推理编译为
 *     liblingos_ncnn.so，导出下列 C ABI；本引擎 dlopen 成功后**真实调用**；
 *     库不存在 / ABI 不符 / 自检失败 → 诚实回退 yolo.sock 后端（现状不变）。
 *
 * 【shim 实现契约（C++ 侧）】
 *   extern "C" const lingos_ncnn_shim_t *lingos_ncnn_shim_get_v1(void);
 *   · abi_version 必须 == LINGOS_NCNN_SHIM_ABI，否则本引擎拒绝加载
 *   · detect() 返回检测数量(>=0) 或负值表示推理失败（本引擎将回退 yolo.sock）
 */

#define LINGOS_NCNN_SHIM_LIB    "liblingos_ncnn.so"
#define LINGOS_NCNN_SHIM_ABI    1
#define LINGOS_NCNN_SHIM_ENTRY  "lingos_ncnn_shim_get_v1"

typedef struct lingos_ncnn_shim {
    int abi_version;                          /* 必须 == LINGOS_NCNN_SHIM_ABI */
    const char *(*name)(void);                /* 后端标识（如 "ncnn-yolov8n"） */
    int (*available)(void);                   /* 1 = 模型 + 运行库就绪 */
    int (*detect)(const unsigned char *frame_data, int frame_size,
                  int width, int height,
                  const char *model_dir, float threshold,
                  detection_result_t *out, int max_out);   /* 返回数量 / 负值=失败 */
} lingos_ncnn_shim_t;

/** shim 入口签名（C++ 侧必须 extern "C" 导出） */
typedef const lingos_ncnn_shim_t *(*lingos_ncnn_shim_get_fn)(void);

/** @brief NCNN shim 是否已加载且自检就绪：1=是（detect 将被真实调用）；0=否 */
int detection_ncnn_available(void);

/** @brief 诚实状态文本（未接入原因 / 已加载后端名） */
const char* detection_ncnn_status(void);

#endif /* VISION_DETECTION_ENGINE_H */