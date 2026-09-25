/**
 * @file    detection_engine.c
 * @brief   检测引擎 —— YOLO 真实推理（yolo.sock 接线）
 * @version LN-0.7.0
 * @par     核心协议：容错编程（后端不可用时诚实降级——不伪造数据）
 *
 * 【0.7.0-hf2 重写】原实现为"模拟检测（每次假报 人+猫+火焰 固定数据）"：
 *   · 违反诚实红线（监控假报——先生审计 2026-09-25）
 *   · "火焰"假框会误导用户（安全隐患）
 * 现实现：
 *   · JPEG 帧（RTSP）→ 经 /LINGOS/run/yolo.sock 调用 Python YOLO 真实推理
 *   · 后端不可用/非 JPEG 帧 → 返回 0（无检测）+ DEBUG 日志（诚实降级，绝不伪造）
 *   · Python yolo_service.py 协议：{"cmd":"detect","image":"<base64>","threshold":0.5}
 *     → {"status":"ok","detections":[{"x","y","width","height","class_id","label","confidence"}]}
 */

#include "detection_engine.h"
#include "../common/error_report.h"
#include "../common/safe_string.h"
#include "../common/lang.h"
#include "../lib/log_extra.h"
#include "../lib/cJSON/cJSON.h"
#include "data_path.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include <unistd.h>
#include <errno.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/time.h>

#define YOLO_SOCK_PATH          "/LINGOS/run/yolo.sock"
#define YOLO_READ_TIMEOUT_S     15
#define YOLO_MAX_RESP           (4 * 1024 * 1024)
#define YOLO_JPEG_MAX           (4 * 1024 * 1024)

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

    /* 检查模型文件是否存在（仅供参考——真实推理在 Python 侧） */
    if (access(g_model_path, F_OK) != 0) {
        LOG_WARN_T("Detection", "Init", "ModelNotFound", "model %s not found (python side owns model)", g_model_path);
        const char *root = lingos_data_root();
        safe_snprintf(g_model_path, sizeof(g_model_path), "%s/models/yolov8n.pt", root);
    }

    g_initialized = 1;
    LOG_INFO_T("Detection", "Init", "OK", "detection engine initialized (yolo.sock backend), threshold=%.2f",
               g_conf_threshold);
    return 0;
}

/* ============================================================
 * base64 编码（无外部依赖）
 * ============================================================ */
static const char B64_TAB[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static char *b64_encode_alloc(const unsigned char *d, size_t n) {
    if (!d || n == 0 || n > YOLO_JPEG_MAX) return NULL;
    size_t olen = (n + 2) / 3 * 4;
    char *out = malloc(olen + 1);
    if (!out) return NULL;
    size_t i, o = 0;
    for (i = 0; i + 2 < n; i += 3) {
        unsigned int v = ((unsigned int)d[i] << 16) | ((unsigned int)d[i + 1] << 8) | d[i + 2];
        out[o++] = B64_TAB[(v >> 18) & 63];
        out[o++] = B64_TAB[(v >> 12) & 63];
        out[o++] = B64_TAB[(v >> 6) & 63];
        out[o++] = B64_TAB[v & 63];
    }
    if (i < n) {
        unsigned int v = (unsigned int)d[i] << 16;
        int rem = (int)(n - i);
        if (rem == 2) v |= (unsigned int)d[i + 1] << 8;
        out[o++] = B64_TAB[(v >> 18) & 63];
        out[o++] = B64_TAB[(v >> 12) & 63];
        out[o++] = (rem == 2) ? B64_TAB[(v >> 6) & 63] : '=';
        out[o++] = '=';
    }
    out[o] = '\0';
    return out;
}

/* ============================================================
 * yolo.sock 客户端（真实推理）
 * ============================================================ */

/* 全量发送辅助 */
static int send_all(int fd, const char *buf, size_t len) {
    size_t sent = 0;
    while (sent < len) {
        ssize_t r = send(fd, buf + sent, len - sent, 0);
        if (r <= 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        sent += (size_t)r;
    }
    return 0;
}

/**
 * 调用 yolo.sock 检测。
 * @param out_count 输出：检测到的对象数（成功时）
 * @return 0 = 成功（结果已填）；-1 = 后端不可用/失败；-2 = 帧格式不支持（非 JPEG）
 */
static int yolo_socket_detect(const camera_frame_t *frame,
                              detection_result_t *results, int max_count,
                              int *out_count) {
    if (out_count) *out_count = 0;
    if (!frame || !frame->data || frame->size <= 0) return -1;

    /* 仅 JPEG 帧可送检（YUYV 原始帧需转码——暂不支持，诚实降级） */
    if (frame->size < 4 || frame->data[0] != 0xFF || frame->data[1] != 0xD8) {
        return -2;
    }
    if (frame->size > YOLO_JPEG_MAX) return -1;

    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return -1;

    struct timeval tv;
    tv.tv_sec = YOLO_READ_TIMEOUT_S;
    tv.tv_usec = 0;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    safe_strncpy(addr.sun_path, YOLO_SOCK_PATH, sizeof(addr.sun_path));

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        LOG_DEBUG_T("Detection", "Yolo", "ConnectFail", "yolo.sock unavailable: %s", strerror(errno));
        close(fd);
        return -1;
    }

    char *b64 = b64_encode_alloc(frame->data, (size_t)frame->size);
    if (!b64) {
        close(fd);
        return -1;
    }

    /* 分片发送：头 + base64 + 尾 */
    char head[160];
    int hn = safe_snprintf(head, sizeof(head),
                           "{\"cmd\":\"detect\",\"threshold\":%.2f,\"image\":\"", g_conf_threshold);
    if (hn <= 0 ||
        send_all(fd, head, (size_t)hn) != 0 ||
        send_all(fd, b64, strlen(b64)) != 0 ||
        send_all(fd, "\"}\n", 3) != 0) {
        free(b64);
        close(fd);
        return -1;
    }
    free(b64);

    /* 读响应（至换行或上限） */
    char *resp = malloc(YOLO_MAX_RESP);
    if (!resp) {
        close(fd);
        return -1;
    }
    size_t total = 0;
    int got_nl = 0;
    while (total < YOLO_MAX_RESP - 1 && !got_nl) {
        ssize_t r = recv(fd, resp + total, YOLO_MAX_RESP - 1 - total, 0);
        if (r <= 0) break;
        if (memchr(resp + total, '\n', (size_t)r) != NULL) got_nl = 1;
        total += (size_t)r;
    }
    close(fd);
    resp[total] = '\0';

    if (total == 0) {
        free(resp);
        return -1;
    }

    /* 解析响应 */
    cJSON *root = cJSON_Parse(resp);
    free(resp);
    if (!root) {
        LOG_DEBUG_T("Detection", "Yolo", "ParseFail", "invalid JSON response");
        return -1;
    }

    cJSON *st = cJSON_GetObjectItem(root, "status");
    if (!cJSON_IsString(st) || strcmp(st->valuestring, "ok") != 0) {
        cJSON *err = cJSON_GetObjectItem(root, "error");
        LOG_DEBUG_T("Detection", "Yolo", "BackendErr", "status!=ok: %s",
                    (err && cJSON_IsString(err)) ? err->valuestring : "?");
        cJSON_Delete(root);
        return -1;
    }

    int count = 0;
    cJSON *dets = cJSON_GetObjectItem(root, "detections");
    if (cJSON_IsArray(dets)) {
        cJSON *item = NULL;
        cJSON_ArrayForEach(item, dets) {
            if (count >= max_count) break;
            cJSON *jx = cJSON_GetObjectItem(item, "x");
            cJSON *jy = cJSON_GetObjectItem(item, "y");
            cJSON *jw = cJSON_GetObjectItem(item, "width");
            cJSON *jh = cJSON_GetObjectItem(item, "height");
            cJSON *jc = cJSON_GetObjectItem(item, "class_id");
            cJSON *jl = cJSON_GetObjectItem(item, "label");
            cJSON *jf = cJSON_GetObjectItem(item, "confidence");

            detection_result_t *r = &results[count];
            memset(r, 0, sizeof(*r));
            r->x = (jx && cJSON_IsNumber(jx)) ? jx->valueint : 0;
            r->y = (jy && cJSON_IsNumber(jy)) ? jy->valueint : 0;
            r->width = (jw && cJSON_IsNumber(jw)) ? jw->valueint : 0;
            r->height = (jh && cJSON_IsNumber(jh)) ? jh->valueint : 0;
            r->class_id = (jc && cJSON_IsNumber(jc)) ? jc->valueint : -1;
            if (jl && cJSON_IsString(jl)) {
                safe_strncpy(r->label, jl->valuestring, sizeof(r->label));
            } else if (r->class_id >= 0 && r->class_id < (int)CLASS_COUNT) {
                safe_strncpy(r->label, g_class_names[r->class_id], sizeof(r->label));
            }
            r->confidence = (jf && cJSON_IsNumber(jf)) ? jf->valuedouble : 0.0;
            r->world_x = 0;
            r->world_y = 0;
            r->timestamp = time(NULL);
            r->track_id = -1;
            count++;
        }
    }
    cJSON_Delete(root);
    LOG_DEBUG_T("Detection", "Yolo", "OK", "real detections: %d", count);
    if (out_count) *out_count = count;
    return 0;
}

/* ============================================================
 * 检测运行（真实推理 —— 诚实降级）
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

    int n = 0;
    int rc = yolo_socket_detect(frame, results, max_count, &n);
    if (rc == 0) {
        return n;   /* 真实检测成功 */
    }
    if (rc == -2) {
        /* 非 JPEG（如 V4L2 YUYV）——转码未实装：诚实空结果 */
        LOG_DEBUG_T("Detection", "Run", "NonJpeg",
                    "frame not JPEG (V4L2 raw?) — detection skipped (honest empty)");
        return 0;
    }

    /* 后端不可用 → 诚实空结果（绝不伪造——先生审计红线的执行） */
    LOG_DEBUG_T("Detection", "Run", "BackendDown",
                "yolo backend unavailable — returning empty (honest, no fake data)");
    return 0;
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
