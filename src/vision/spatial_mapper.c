/**
 * @file    spatial_mapper.c
 * @brief   空间映射器（像素 → 世界坐标 + 区域匹配）
 * @version LN-B-5.0.0.0
 * @par     核心协议：防御性编程（校准失败时使用近似值）
 * @changes 安全字符串替换；双文支持
 */

#include "spatial_mapper.h"
#include "../common/safe_string.h"
#include "../common/lang.h"
#include "../lib/log_extra.h"
#include "../common/data_path.h"
#include "../lib/cJSON/cJSON.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

/* ============================================================
 * 全局状态
 * ============================================================ */

static double g_h_matrix[9] = {1,0,0, 0,1,0, 0,0,1};
static int g_calibrated = 0;
static zone_t g_zones[8];
static int g_zone_count = 0;

/* ============================================================
 * 初始化
 * ============================================================ */

void spatial_init(const vision_config_t *config) {
    LOG_DEBUG_T("Spatial", "Init", "Enter", "calibration_path=%s", config->calibration_path);

    if (config->calibration_path[0]) {
        spatial_load_calibration(config->calibration_path);
    }

    LOG_INFO_T("Spatial", "Init", "OK", "spatial mapper initialized, calibrated=%d", g_calibrated);
}

int spatial_load_calibration(const char *path) {
    LOG_DEBUG_T("Spatial", "LoadCalib", "Enter", "path=%s", path);

    FILE *fp = fopen(path, "r");
    if (!fp) {
        LOG_WARN_T("Spatial", "LoadCalib", "NotFound", "calibration file not found");
        return -1;
    }

    fseek(fp, 0, SEEK_END);
    long size = ftell(fp);
    fseek(fp, 0, SEEK_SET);

    char *buf = malloc(size + 1);
    if (!buf) {
        fclose(fp);
        return -1;
    }
    fread(buf, 1, size, fp);
    buf[size] = '\0';
    fclose(fp);

    cJSON *root = cJSON_Parse(buf);
    free(buf);
    if (!root) {
        LOG_ERROR_T("Spatial", "LoadCalib", "ParseFail", "invalid JSON");
        return -1;
    }

    cJSON *matrix = cJSON_GetObjectItem(root, "homography");
    if (matrix && cJSON_IsArray(matrix)) {
        int size_arr = cJSON_GetArraySize(matrix);
        for (int i = 0; i < size_arr && i < 9; i++) {
            cJSON *item = cJSON_GetArrayItem(matrix, i);
            if (item && cJSON_IsNumber(item)) {
                g_h_matrix[i] = item->valuedouble;
            }
        }
        g_calibrated = 1;
    }

    cJSON *zones = cJSON_GetObjectItem(root, "zones");
    if (zones && cJSON_IsArray(zones)) {
        int size_arr = cJSON_GetArraySize(zones);
        g_zone_count = 0;
        for (int i = 0; i < size_arr && i < 8; i++) {
            cJSON *item = cJSON_GetArrayItem(zones, i);
            if (!item) continue;
            cJSON *name = cJSON_GetObjectItem(item, "name");
            cJSON *points = cJSON_GetObjectItem(item, "points");
            if (name && cJSON_IsString(name) && points && cJSON_IsArray(points)) {
                safe_strncpy(g_zones[g_zone_count].name, name->valuestring,
                             sizeof(g_zones[g_zone_count].name));
                int pt_count = cJSON_GetArraySize(points);
                for (int j = 0; j < pt_count && j < 8; j++) {
                    cJSON *pt = cJSON_GetArrayItem(points, j);
                    if (pt && cJSON_IsArray(pt)) {
                        cJSON *x = cJSON_GetArrayItem(pt, 0);
                        cJSON *y = cJSON_GetArrayItem(pt, 1);
                        if (x && y && cJSON_IsNumber(x) && cJSON_IsNumber(y)) {
                            g_zones[g_zone_count].points[j][0] = x->valuedouble;
                            g_zones[g_zone_count].points[j][1] = y->valuedouble;
                            g_zones[g_zone_count].point_count++;
                        }
                    }
                }
                g_zone_count++;
            }
        }
    }

    cJSON_Delete(root);
    LOG_INFO_T("Spatial", "LoadCalib", "OK", "calibration loaded, zones=%d", g_zone_count);
    return 0;
}

int spatial_map(detection_result_t *result) {
    if (!result) return -1;

    if (!g_calibrated) {
        result->world_x = result->x * 0.1;
        result->world_y = result->y * 0.1;
        return 0;
    }

    double x = result->x + result->width / 2.0;
    double y = result->y + result->height / 2.0;

    double denom = g_h_matrix[6] * x + g_h_matrix[7] * y + g_h_matrix[8];
    if (fabs(denom) < 1e-10) {
        result->world_x = 0;
        result->world_y = 0;
        return -1;
    }

    result->world_x = (g_h_matrix[0] * x + g_h_matrix[1] * y + g_h_matrix[2]) / denom;
    result->world_y = (g_h_matrix[3] * x + g_h_matrix[4] * y + g_h_matrix[5]) / denom;

    return 0;
}

const char* spatial_match_zone(double wx, double wy) {
    for (int i = 0; i < g_zone_count; i++) {
        if (g_zones[i].point_count < 3) continue;

        int inside = 0;
        for (int j = 0; j < g_zones[i].point_count; j++) {
            int k = (j + 1) % g_zones[i].point_count;
            double xj = g_zones[i].points[j][0];
            double yj = g_zones[i].points[j][1];
            double xk = g_zones[i].points[k][0];
            double yk = g_zones[i].points[k][1];

            if (((yj > wy) != (yk > wy)) &&
                (wx < (xk - xj) * (wy - yj) / (yk - yj) + xj)) {
                inside = !inside;
            }
        }
        if (inside) {
            return g_zones[i].name;
        }
    }
    return NULL;
}

void spatial_cleanup(void) {
    g_calibrated = 0;
    g_zone_count = 0;
    LOG_DEBUG_T("Spatial", "Cleanup", "OK", "spatial mapper cleaned up");
}