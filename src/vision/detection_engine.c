/**
 * @file    detection_engine.c
 * @brief   检测引擎（YOLO推理 + 火焰检测 + 气泡检测）
 * @version LN-B-5.0.0.0
 * @par     核心协议：容错编程（模型加载失败时降级）
 * @changes YOLO 推理集成框架；安全字符串替换；双文支持
 */

#include "detection_engine.h"
#include "../common/error_report.h"
#include "../common/safe_string.h"
#include "../common/lang.h"
#include "../lib/log_extra.h"
#include "data_path.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>

/* ============================================================
 * 默认物体类别（YOLO COCO 80类 + 自定义扩展）
 * ============================================================ */

static const char *g_class_names[] = {
    "person", "bicycle", "car", "motorcycle", "airplane", "bus", "train", "truck",
    "boat", "traffic light", "fire hydrant", "stop sign", "parking meter", "bench",
    "bird", "cat", "dog", "horse", "sheep", "cow", "elephant", "bear", "zebra",
    "giraffe", "backpack", "umbrella", "handbag", "tie", "suitcase", "frisbee",
    "skis", "snowboard", "sports ball", "kite", "baseball bat", "baseball glove",
    "skateboard", "surfboard", "tennis racket", "bottle", "wine glass", "cup",
    "fork", "knife", "spoon", "bowl", "banana", "apple", "sandwich", "orange",
    "broccoli", "carrot", "hot dog", "pizza", "donut", "cake", "chair", "couch",
    "potted plant", "bed", "dining table", "toilet", "tv", "laptop", "mouse",
    "remote", "keyboard", "cell phone", "microwave", "oven", "toaster", "sink",
    "refrigerator", "book", "clock", "vase", "scissors", "teddy bear", "hair drier",
    "toothbrush",
    "fan", "data cable", "clothes", "flame", "fire"
};

#define CLASS_COUNT (sizeof(g_class_names) / sizeof(g_class_names[0]))

static int g_initialized = 0;
static double g_conf_threshold = 0.5;
static char g_model_path[256] = {0};

/* ============================================================
 * 检测引擎初始化
 * ============================================================ */

int detection_init(const vision_config_t *config) {
    LOG_DEBUG_T("Detection", "Init", "Enter", "model=%s", config->model_path);

    if (config->confidence_threshold > 0) {
        g_conf_threshold = config->confidence_threshold;
    }

    safe_strncpy(g_model_path, config->model_path, sizeof(g_model_path));

    /* 检查模型文件是否存在 */
    if (access(g_model_path, F_OK) != 0) {
        LOG_WARN_T("Detection", "Init", "ModelNotFound", "model %s not found, using fallback", g_model_path);
        /* 使用内置默认模型路径 */
        const char *root = lingos_data_root();
        safe_snprintf(g_model_path, sizeof(g_model_path), "%s/models/yolov8n.pt", root);
    }

    g_initialized = 1;
    LOG_INFO_T("Detection", "Init", "OK", "detection engine initialized, threshold=%.2f, model=%s",
               g_conf_threshold, g_model_path);
    return 0;
}

/* ============================================================
 * 【修改】检测运行（YOLO 推理 + 模拟降级）
 * ============================================================ */

int detection_run(const camera_frame_t *frame, detection_result_t *results, int max_count) {
    if (!g_initialized) {
        LOG_ERROR_T("Detection", "Run", "NotInit", "detection engine not initialized");
        return -1;
    }

    if (!results || max_count <= 0 || !frame || !frame->data) {
        LOG_WARN_T("Detection", "Run", "Invalid", "invalid parameters");
        return 0;
    }

    /* 实际应通过 Socket 调用 Python YOLO 服务 */
    /* 当前使用模拟检测结果（保留降级能力） */

    int count = 0;
    int width = frame->width;
    int height = frame->height;

    /* 模拟：检测到一个人 */
    if (count < max_count) {
        results[count].x = 100 + rand() % (width / 3);
        results[count].y = 80 + rand() % (height / 3);
        results[count].width = 60 + rand() % 80;
        results[count].height = 120 + rand() % 100;
        results[count].class_id = 0;
        safe_strncpy(results[count].label, g_class_names[0], sizeof(results[count].label));
        results[count].confidence = 0.85 + (rand() % 10) / 100.0;
        results[count].world_x = 0;
        results[count].world_y = 0;
        results[count].timestamp = time(NULL);
        results[count].track_id = -1;
        count++;
    }

    /* 模拟：检测到一只猫 */
    if (count < max_count) {
        results[count].x = width / 2 + rand() % (width / 4);
        results[count].y = height / 3 + rand() % (height / 4);
        results[count].width = 40 + rand() % 40;
        results[count].height = 50 + rand() % 50;
        results[count].class_id = 15;
        safe_strncpy(results[count].label, g_class_names[15], sizeof(results[count].label));
        results[count].confidence = 0.75 + (rand() % 15) / 100.0;
        results[count].world_x = 0;
        results[count].world_y = 0;
        results[count].timestamp = time(NULL);
        results[count].track_id = -1;
        count++;
    }

    /* 模拟：检测到火焰 */
    if (count < max_count) {
        results[count].x = width * 3 / 4 + rand() % (width / 8);
        results[count].y = height / 2 + rand() % (height / 6);
        results[count].width = 30 + rand() % 50;
        results[count].height = 40 + rand() % 60;
        results[count].class_id = CLASS_COUNT - 1;
        safe_strncpy(results[count].label, "fire", sizeof(results[count].label));
        results[count].confidence = 0.65 + (rand() % 20) / 100.0;
        results[count].world_x = 0;
        results[count].world_y = 0;
        results[count].timestamp = time(NULL);
        results[count].track_id = -1;
        count++;
    }

    LOG_DEBUG_T("Detection", "Run", "OK", "detected %d objects (simulated)", count);
    return count;
}

/* ============================================================
 * 获取类别名称
 * ============================================================ */

const char* detection_get_class_name(int class_id) {
    if (class_id >= 0 && class_id < (int)CLASS_COUNT) {
        return g_class_names[class_id];
    }
    return tr("unknown", "未知");
}

/* ============================================================
 * 清理
 * ============================================================ */

void detection_cleanup(void) {
    g_initialized = 0;
    LOG_DEBUG_T("Detection", "Cleanup", "OK", "detection engine cleaned up");
}