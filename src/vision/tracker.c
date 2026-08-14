/**
 * @file    tracker.c
 * @brief   物体追踪（IoU + Kalman滤波）
 * @version LN-B-4.3.0.0
 */

#include "tracker.h"
#include "../lib/log_extra.h"
#include "../common/safe_string.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>

#define MAX_TRACKS 32
#define MAX_AGE 10

typedef struct {
    int id;
    char label[32];
    double x, y;
    double width, height;
    int age;
    int active;
    double kalman_x, kalman_y;
    double kalman_vx, kalman_vy;
} track_t;

static track_t g_tracks[MAX_TRACKS];
static int g_next_id = 1;

/* ============================================================
 * 初始化
 * ============================================================ */

void tracker_init(void) {
    memset(g_tracks, 0, sizeof(g_tracks));
    g_next_id = 1;
    LOG_DEBUG_T("Tracker", "Init", "OK", "tracker initialized");
}

/* ============================================================
 * 计算IoU
 * ============================================================ */

static double compute_iou(double x1, double y1, double w1, double h1,
                          double x2, double y2, double w2, double h2) {
    double xa = fmax(x1, x2);
    double ya = fmax(y1, y2);
    double xb = fmin(x1 + w1, x2 + w2);
    double yb = fmin(y1 + h1, y2 + h2);
    double inter = fmax(0, xb - xa) * fmax(0, yb - ya);
    double area1 = w1 * h1;
    double area2 = w2 * h2;
    double union_area = area1 + area2 - inter;
    return inter / (union_area + 1e-10);
}

/* ============================================================
 * 更新追踪器
 * ============================================================ */

void tracker_update(detection_result_t *results, int count) {
    /* 1. 预测：更新所有活跃轨迹（Kalman预测） */
    for (int i = 0; i < MAX_TRACKS; i++) {
        if (g_tracks[i].active) {
            g_tracks[i].age++;
            g_tracks[i].x += g_tracks[i].kalman_vx;
            g_tracks[i].y += g_tracks[i].kalman_vy;
            g_tracks[i].kalman_x = g_tracks[i].x;
            g_tracks[i].kalman_y = g_tracks[i].y;
        }
    }

    /* 2. 关联：匹配检测到追踪 */
    for (int i = 0; i < count; i++) {
        detection_result_t *det = &results[i];
        int best_track = -1;
        double best_iou = 0.3;

        for (int j = 0; j < MAX_TRACKS; j++) {
            if (!g_tracks[j].active) continue;
            if (strcmp(g_tracks[j].label, det->label) != 0) continue;

            double iou = compute_iou(g_tracks[j].x, g_tracks[j].y,
                                     g_tracks[j].width, g_tracks[j].height,
                                     det->x, det->y, det->width, det->height);
            if (iou > best_iou) {
                best_iou = iou;
                best_track = j;
            }
        }

        if (best_track >= 0) {
            /* 更新已有轨迹 */
            track_t *track = &g_tracks[best_track];
            track->x = det->x;
            track->y = det->y;
            track->width = det->width;
            track->height = det->height;
            track->age = 0;
            track->kalman_vx = (det->x - track->kalman_x) * 0.3;
            track->kalman_vy = (det->y - track->kalman_y) * 0.3;
            track->kalman_x = det->x;
            track->kalman_y = det->y;
            det->track_id = track->id;
        } else {
            /* 创建新轨迹 */
            for (int j = 0; j < MAX_TRACKS; j++) {
                if (!g_tracks[j].active) {
                    g_tracks[j].id = g_next_id++;
                    safe_strncpy(g_tracks[j].label, det->label, sizeof(g_tracks[j].label));
                    g_tracks[j].x = det->x;
                    g_tracks[j].y = det->y;
                    g_tracks[j].width = det->width;
                    g_tracks[j].height = det->height;
                    g_tracks[j].age = 0;
                    g_tracks[j].active = 1;
                    g_tracks[j].kalman_x = det->x;
                    g_tracks[j].kalman_y = det->y;
                    g_tracks[j].kalman_vx = 0;
                    g_tracks[j].kalman_vy = 0;
                    det->track_id = g_tracks[j].id;
                    break;
                }
            }
        }
    }

    /* 3. 清理超时轨迹 */
    for (int i = 0; i < MAX_TRACKS; i++) {
        if (g_tracks[i].active && g_tracks[i].age > MAX_AGE) {
            g_tracks[i].active = 0;
            LOG_DEBUG_T("Tracker", "Cleanup", "Removed", "track %d timed out", g_tracks[i].id);
        }
    }
}

/* ============================================================
 * 获取活跃轨迹
 * ============================================================ */

int tracker_get_active(track_info_t *out, int max_count) {
    int count = 0;
    for (int i = 0; i < MAX_TRACKS && count < max_count; i++) {
        if (g_tracks[i].active) {
            out[count].id = g_tracks[i].id;
            safe_strncpy(out[count].label, g_tracks[i].label, sizeof(out[count].label));
            out[count].x = g_tracks[i].x;
            out[count].y = g_tracks[i].y;
            out[count].width = g_tracks[i].width;
            out[count].height = g_tracks[i].height;
            count++;
        }
    }
    return count;
}

/* ============================================================
 * 查找轨迹
 * ============================================================ */

int tracker_find_by_id(int id, track_info_t *out) {
    for (int i = 0; i < MAX_TRACKS; i++) {
        if (g_tracks[i].active && g_tracks[i].id == id) {
            if (out) {
                out->id = g_tracks[i].id;
                safe_strncpy(out->label, g_tracks[i].label, sizeof(out->label));
                out->x = g_tracks[i].x;
                out->y = g_tracks[i].y;
                out->width = g_tracks[i].width;
                out->height = g_tracks[i].height;
            }
            return 1;
        }
    }
    return 0;
}

/* ============================================================
 * 清理
 * ============================================================ */

void tracker_cleanup(void) {
    memset(g_tracks, 0, sizeof(g_tracks));
    g_next_id = 1;
    LOG_DEBUG_T("Tracker", "Cleanup", "OK", "tracker cleaned up");
}