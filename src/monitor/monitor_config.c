/**
 * @file    src/monitor/monitor_config.c
 * @brief   监控系统配置加载实现
 * @version LN-0.9.0
 */

#include "monitor_config.h"
#include "../common/safe_string.h"
#include "../common/data_path.h"
#include "../lib/log_extra.h"
#include "../lib/cJSON/cJSON.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <errno.h>

/* ============================================================
 * 默认值
 * ============================================================ */

void monitor_config_set_defaults(monitor_config_t *cfg) {
    if (!cfg) return;
    memset(cfg, 0, sizeof(*cfg));
    cfg->camera_device = 0;
    safe_strncpy(cfg->device_path, "/dev/video0", sizeof(cfg->device_path));
    safe_strncpy(cfg->camera_source, "v4l2", sizeof(cfg->camera_source));
    safe_strncpy(cfg->rtsp_url, "", sizeof(cfg->rtsp_url));
    cfg->rtsp_frame_port = 8890;
    cfg->rtsp_http_port = 8891;
    cfg->width = 640;
    cfg->height = 480;
    cfg->fps = 10;
    safe_strncpy(cfg->camera_id, "cam0", sizeof(cfg->camera_id));

    safe_strncpy(cfg->frame_channel_path, "/LINGOS/run/frame.sock", sizeof(cfg->frame_channel_path));
    cfg->frame_channel_max_clients = 4;

    safe_strncpy(cfg->snapshot_dir, "/LINGOS/data/monitor/snapshots", sizeof(cfg->snapshot_dir));
    safe_strncpy(cfg->record_dir, "/LINGOS/data/monitor/recordings", sizeof(cfg->record_dir));
    cfg->record_segment_min = 10;
    cfg->free_space_reserve_mb = 500;

    safe_strncpy(cfg->control_socket_path, "/LINGOS/run/monitor.sock", sizeof(cfg->control_socket_path));

    cfg->preview_port = 8891;
    cfg->preview_quality = 85;
    cfg->multi_camera = 0;

    /* OSD 默认开（给人看的画面带时间戳）；IR 默认 keep = 不主动改硬件 */
    cfg->osd_enabled = 1;
    cfg->osd_scale = 1;
    safe_strncpy(cfg->ir_mode, "keep", sizeof(cfg->ir_mode));
}

/* ============================================================
 * 历史 vision.conf（采集键回退——不减少功能）
 * ============================================================ */

static void apply_legacy_vision_conf(monitor_config_t *cfg) {
    const char *root = lingos_data_root();
    char path[512];
    safe_snprintf(path, sizeof(path), "%s/system/config/vision.conf", root);

    FILE *fp = fopen(path, "r");
    if (!fp) {
        LOG_DEBUG_T("MonitorConfig", "Load", "NoLegacy", "vision.conf not found (%s)", path);
        return;
    }

    char line[256];
    while (fgets(line, sizeof(line), fp)) {
        if (line[0] == '#' || line[0] == '\n' || line[0] == '\r') continue;
        char key[64], val[64];
        if (sscanf(line, "%63[^=]=%63s", key, val) != 2) continue;
        if      (strcmp(key, "camera_device") == 0) cfg->camera_device = atoi(val);
        else if (strcmp(key, "device_path") == 0)   safe_strncpy(cfg->device_path, val, sizeof(cfg->device_path));
        else if (strcmp(key, "camera_source") == 0) safe_strncpy(cfg->camera_source, val, sizeof(cfg->camera_source));
        else if (strcmp(key, "rtsp_url") == 0)      safe_strncpy(cfg->rtsp_url, val, sizeof(cfg->rtsp_url));
        else if (strcmp(key, "rtsp_frame_port") == 0) cfg->rtsp_frame_port = atoi(val);
        else if (strcmp(key, "rtsp_http_port") == 0)  cfg->rtsp_http_port = atoi(val);
        else if (strcmp(key, "width") == 0)  cfg->width = atoi(val);
        else if (strcmp(key, "height") == 0) cfg->height = atoi(val);
        else if (strcmp(key, "fps") == 0)    cfg->fps = atoi(val);
    }
    fclose(fp);
    LOG_INFO_T("MonitorConfig", "Load", "LegacyOK", "legacy vision.conf capture keys applied");
}

/* ============================================================
 * monitor.json（主配置——与 Python monitor_service.py 同源）
 * ============================================================ */

static char *read_text_file(const char *path, long *out_len) {
    FILE *fp = fopen(path, "rb");
    if (!fp) return NULL;
    if (fseek(fp, 0, SEEK_END) != 0) { fclose(fp); return NULL; }
    long sz = ftell(fp);
    if (sz <= 0 || sz > 1024 * 1024) { fclose(fp); return NULL; }
    rewind(fp);
    char *buf = malloc((size_t)sz + 1);
    if (!buf) { fclose(fp); return NULL; }
    size_t got = fread(buf, 1, (size_t)sz, fp);
    fclose(fp);
    buf[got] = '\0';
    if (out_len) *out_len = (long)got;
    return buf;
}

static const char *json_str(const cJSON *obj, const char *key, const char *fallback) {
    const cJSON *it = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (cJSON_IsString(it) && it->valuestring && it->valuestring[0]) return it->valuestring;
    return fallback;
}

static int json_int(const cJSON *obj, const char *key, int fallback) {
    const cJSON *it = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (cJSON_IsNumber(it)) return it->valueint;
    return fallback;
}

static int json_bool(const cJSON *obj, const char *key, int fallback) {
    const cJSON *it = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (cJSON_IsBool(it)) return cJSON_IsTrue(it) ? 1 : 0;
    if (cJSON_IsNumber(it)) return it->valueint ? 1 : 0;
    return fallback;
}

static void apply_monitor_json(monitor_config_t *cfg, const cJSON *root) {
    if (!cJSON_IsObject(root)) return;

    safe_strncpy(cfg->snapshot_dir, json_str(root, "snapshot_dir", cfg->snapshot_dir), sizeof(cfg->snapshot_dir));
    safe_strncpy(cfg->record_dir,   json_str(root, "record_dir",   cfg->record_dir),   sizeof(cfg->record_dir));
    cfg->record_segment_min    = json_int(root, "record_segment_min", cfg->record_segment_min);
    cfg->free_space_reserve_mb = json_int(root, "free_space_reserve_mb", cfg->free_space_reserve_mb);
    cfg->fps                   = json_int(root, "fps", cfg->fps);
    cfg->multi_camera          = json_int(root, "multi_camera", cfg->multi_camera);

    const cJSON *ch = cJSON_GetObjectItemCaseSensitive(root, "frame_channel");
    if (cJSON_IsObject(ch)) {
        safe_strncpy(cfg->frame_channel_path, json_str(ch, "path", cfg->frame_channel_path),
                     sizeof(cfg->frame_channel_path));
        cfg->frame_channel_max_clients = json_int(ch, "max_clients", cfg->frame_channel_max_clients);
    }

    const cJSON *ctl = cJSON_GetObjectItemCaseSensitive(root, "control");
    if (cJSON_IsObject(ctl)) {
        safe_strncpy(cfg->control_socket_path, json_str(ctl, "path", cfg->control_socket_path),
                     sizeof(cfg->control_socket_path));
    }

    const cJSON *pv = cJSON_GetObjectItemCaseSensitive(root, "preview");
    if (cJSON_IsObject(pv)) {
        cfg->preview_port    = json_int(pv, "port", cfg->preview_port);
        cfg->preview_quality = json_int(pv, "quality", cfg->preview_quality);
    }

    /* OSD（时间戳叠加）：{"osd":{"enabled":true,"scale":1}} */
    const cJSON *osd = cJSON_GetObjectItemCaseSensitive(root, "osd");
    if (cJSON_IsObject(osd)) {
        cfg->osd_enabled = json_bool(osd, "enabled", cfg->osd_enabled);
        cfg->osd_scale   = json_int(osd, "scale", cfg->osd_scale);
    }

    /* 红外：{"ir":{"mode":"keep|off|on|auto"}} —— 仅真实 V4L2 控制面，无该项则诚实失败 */
    const cJSON *ir = cJSON_GetObjectItemCaseSensitive(root, "ir");
    if (cJSON_IsObject(ir)) {
        safe_strncpy(cfg->ir_mode, json_str(ir, "mode", cfg->ir_mode), sizeof(cfg->ir_mode));
    }

    /* 主摄像头 = cameras[] 中第一路 enabled（未标 enabled 视为启用）——多路为后续扩展 */
    const cJSON *cams = cJSON_GetObjectItemCaseSensitive(root, "cameras");
    if (cJSON_IsArray(cams)) {
        const cJSON *cam = NULL;
        cJSON_ArrayForEach(cam, cams) {
            if (!cJSON_IsObject(cam)) continue;
            if (!json_bool(cam, "enabled", 1)) continue;

            safe_strncpy(cfg->camera_id, json_str(cam, "id", cfg->camera_id), sizeof(cfg->camera_id));
            const char *type = json_str(cam, "type", "v4l2");
            safe_strncpy(cfg->camera_source, type, sizeof(cfg->camera_source));
            if (strcmp(type, "rtsp") == 0 || strcmp(type, "mjpeg") == 0) {
                safe_strncpy(cfg->rtsp_url, json_str(cam, "url", cfg->rtsp_url), sizeof(cfg->rtsp_url));
                cfg->rtsp_frame_port = json_int(cam, "frame_port", cfg->rtsp_frame_port);
                cfg->rtsp_http_port  = json_int(cam, "http_port",  cfg->rtsp_http_port);
            } else {
                safe_strncpy(cfg->device_path, json_str(cam, "device", cfg->device_path),
                             sizeof(cfg->device_path));
            }
            cfg->width  = json_int(cam, "width",  cfg->width);
            cfg->height = json_int(cam, "height", cfg->height);
            cfg->fps    = json_int(cam, "fps",    cfg->fps);
            break;   /* 首路 = 主实例；多路留待 multi_camera 扩展 */
        }
    }
}

int monitor_config_load(monitor_config_t *cfg) {
    if (!cfg) return -1;
    monitor_config_set_defaults(cfg);

    /* ① 历史键（vision.conf 采集段） */
    apply_legacy_vision_conf(cfg);

    /* ② 主配置 monitor.json（覆盖历史键） */
    const char *root = lingos_data_root();
    char path[512];
    safe_snprintf(path, sizeof(path), "%s/system/config/monitor.json", root);

    long len = 0;
    char *text = read_text_file(path, &len);
    if (!text) {
        LOG_WARN_T("MonitorConfig", "Load", "NoJSON",
                   "monitor.json not found (%s) — using legacy/default capture config", path);
    } else {
        cJSON *root_json = cJSON_Parse(text);
        free(text);
        if (!root_json) {
            LOG_ERROR_T("MonitorConfig", "Load", "BadJSON", "monitor.json parse failed (%s)", path);
        } else {
            apply_monitor_json(cfg, root_json);
            cJSON_Delete(root_json);
            LOG_INFO_T("MonitorConfig", "Load", "JSONOK", "monitor.json applied (%s)", path);
        }
    }

    /* 采集参数合法性守卫（错配置不崩溃：回到安全默认） */
    if (cfg->width <= 0 || cfg->width > 7680) cfg->width = 640;
    if (cfg->height <= 0 || cfg->height > 4320) cfg->height = 480;
    if (cfg->fps <= 0 || cfg->fps > 60) cfg->fps = 10;
    if (cfg->frame_channel_max_clients <= 0 || cfg->frame_channel_max_clients > 8)
        cfg->frame_channel_max_clients = 4;
    if (cfg->osd_scale < 1 || cfg->osd_scale > 4) cfg->osd_scale = 1;
    if (strcmp(cfg->ir_mode, "off") != 0 && strcmp(cfg->ir_mode, "on") != 0 &&
        strcmp(cfg->ir_mode, "auto") != 0) {
        safe_strncpy(cfg->ir_mode, "keep", sizeof(cfg->ir_mode));   /* 未知值 → 不干预硬件 */
    }

    LOG_INFO_T("MonitorConfig", "Load", "OK",
               "camera=%s source=%s %dx%d@%d frame_channel=%s control=%s",
               cfg->camera_id, cfg->camera_source, cfg->width, cfg->height, cfg->fps,
               cfg->frame_channel_path, cfg->control_socket_path);
    return 0;
}

/* ============================================================
 * 目录准备
 * ============================================================ */

static int mkdir_p(const char *path) {
    if (!path || !path[0]) return -1;
    char tmp[256];
    safe_strncpy(tmp, path, sizeof(tmp));
    size_t len = strlen(tmp);
    while (len > 1 && tmp[len - 1] == '/') tmp[--len] = '\0';
    for (char *p = tmp + 1; *p; p++) {
        if (*p != '/') continue;
        *p = '\0';
        if (mkdir(tmp, 0755) != 0 && errno != EEXIST) {
            LOG_WARN_T("MonitorConfig", "Mkdir", "Fail", "mkdir %s failed: %s", tmp, strerror(errno));
            return -1;
        }
        *p = '/';
    }
    if (mkdir(tmp, 0755) != 0 && errno != EEXIST) {
        LOG_WARN_T("MonitorConfig", "Mkdir", "Fail", "mkdir %s failed: %s", tmp, strerror(errno));
        return -1;
    }
    return 0;
}

int monitor_config_ensure_dirs(const monitor_config_t *cfg) {
    if (!cfg) return -1;
    int rc = 0;
    if (mkdir_p(cfg->snapshot_dir) != 0) rc = -1;
    if (mkdir_p(cfg->record_dir) != 0) rc = -1;
    return rc;
}
