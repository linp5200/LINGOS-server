/**
 * @file    mqtt_sync_enhanced.c
 * @brief   多设备同步增强（服务端权威 + 冲突仅驳回冲突项）
 * @version LN-B-5.0.0.0
 * @par     核心协议：容错编程（同步失败不影响本地功能）
 * @changes 实际同步实现；服务端权威逻辑完善；安全字符串替换；双文支持
 */

#include "mqtt_sync_enhanced.h"
#include "mqtt_client.h"
#include "../common/safe_string.h"
#include "../common/data_path.h"
#include "../common/lang.h"
#include "../lib/log_extra.h"
#include "../core/version.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <cJSON.h>

#define SYNC_TOPIC "lingos/sync"

static int g_server_mode = 1;
static char g_device_id[64] = "lingos_pc";
static int g_sync_enabled = 1;
static cJSON *g_conflict_cache = NULL;

/* ============================================================
 * 检查当前设备是否为服务端
 * ============================================================ */

int mqtt_sync_is_server(void) {
    return g_server_mode;
}

/* ============================================================
 * 设置设备角色
 * ============================================================ */

void mqtt_sync_set_role(int is_server) {
    g_server_mode = is_server;
    LOG_INFO_T("MQTTSync", "SetRole", "%s", is_server ? tr("SERVER", "服务端") : tr("CLIENT", "客户端"));
}

/* ============================================================
 * 【修改】处理同步消息（冲突检测 + 驳回）
 * ============================================================ */

static int handle_sync_message(const char *topic, const char *payload) {
    LOG_DEBUG_T("MQTTSync", "Handle", "Enter", "topic=%s", topic ? topic : "(null)");

    if (!payload || !g_sync_enabled) {
        LOG_DEBUG_T("MQTTSync", "Handle", "Skip", "payload empty or sync disabled");
        return -1;
    }

    cJSON *root = cJSON_Parse(payload);
    if (!root) {
        LOG_WARN_T("MQTTSync", "Handle", "ParseFail", "invalid JSON");
        return -1;
    }

    cJSON *type = cJSON_GetObjectItem(root, "type");
    cJSON *data = cJSON_GetObjectItem(root, "data");
    cJSON *device = cJSON_GetObjectItem(root, "device_id");
    cJSON *timestamp = cJSON_GetObjectItem(root, "timestamp");
    cJSON *version = cJSON_GetObjectItem(root, "version");

    if (!type || !cJSON_IsString(type) || !data || !cJSON_IsString(data)) {
        cJSON_Delete(root);
        LOG_WARN_T("MQTTSync", "Handle", "Invalid", "missing type or data");
        return -1;
    }

    /* 检查是否来自自己 */
    if (device && cJSON_IsString(device) && strcmp(device->valuestring, g_device_id) == 0) {
        cJSON_Delete(root);
        LOG_DEBUG_T("MQTTSync", "Handle", "Self", "ignoring self message");
        return 0;
    }

    /* 服务端权威：服务端直接应用，冲突时驳回冲突项 */
    if (g_server_mode) {
        LOG_INFO_T("MQTTSync", "Handle", "ServerApply", "applying sync data from %s",
                   device ? device->valuestring : tr("unknown", "未知"));

        /* 冲突检测：检查本地是否存在相同 key */
        const char *data_str = data->valuestring;
        cJSON *data_json = cJSON_Parse(data_str);
        if (data_json) {
            cJSON *key = cJSON_GetObjectItem(data_json, "key");
            cJSON *value = cJSON_GetObjectItem(data_json, "value");

            if (key && cJSON_IsString(key)) {
                /* 检查本地冲突缓存 */
                int conflict = 0;
                if (g_conflict_cache) {
                    cJSON *existing = cJSON_GetObjectItem(g_conflict_cache, key->valuestring);
                    if (existing) {
                        long local_ver = cJSON_GetNumberValue(existing);
                        long remote_ver = (version && cJSON_IsNumber(version)) ? (long)version->valuedouble : 0;
                        if (remote_ver < local_ver) {
                            conflict = 1;
                            LOG_WARN_T("MQTTSync", "Handle", "Conflict",
                                       "key=%s: local version %ld > remote %ld, rejecting",
                                       key->valuestring, local_ver, remote_ver);
                        }
                    }
                }

                if (!conflict) {
                    /* 应用变更 */
                    if (value && cJSON_IsString(value)) {
                        /* 实际应用配置更新 - 写入本地存储 */
                        char config_path[512];
                        safe_snprintf(config_path, sizeof(config_path),
                                      "/LINGOS/state/sync/%s.json", key->valuestring);
                        FILE *fp = fopen(config_path, "w");
                        if (fp) {
                            fprintf(fp, "%s\n", value->valuestring);
                            fclose(fp);
                            LOG_DEBUG_T("MQTTSync", "Handle", "Applied",
                                        "key=%s written", key->valuestring);
                        }
                    }
                    /* 更新缓存 */
                    if (g_conflict_cache && version && cJSON_IsNumber(version)) {
                        cJSON_AddNumberToObject(g_conflict_cache, key->valuestring, version->valuedouble);
                    }
                } else {
                    /* 冲突：发送驳回消息 */
                    char reject_msg[512];
                    safe_snprintf(reject_msg, sizeof(reject_msg),
                                  "{\"cmd\":\"reject\",\"key\":\"%s\",\"reason\":\"conflict\"}",
                                  key->valuestring);
                    mqtt_client_publish("lingos/sync/reject", reject_msg, strlen(reject_msg), 1, 0);
                    LOG_INFO_T("MQTTSync", "Handle", "Rejected", "key=%s conflict", key->valuestring);
                }
            }
            cJSON_Delete(data_json);
        }

        cJSON_Delete(root);
        return 0;
    } else {
        /* 客户端：发送申请给服务端 */
        char request[512];
        safe_snprintf(request, sizeof(request),
                      "{\"cmd\":\"sync_request\",\"type\":\"%s\",\"data\":\"%s\",\"device\":\"%s\"}",
                      type->valuestring, data->valuestring, g_device_id);
        mqtt_client_publish("lingos/sync/request", request, strlen(request), 1, 0);
        LOG_INFO_T("MQTTSync", "Handle", "ClientRequest", "sync request sent");
        cJSON_Delete(root);
        return 0;
    }
}

/* ============================================================
 * 【修改】初始化同步系统
 * ============================================================ */

int mqtt_sync_enhanced_init(const char *device_id, int is_server) {
    LOG_INFO_T("MQTTSync", "Init", "Enter", "device=%s, server=%d",
               device_id ? device_id : "(null)", is_server);

    if (device_id && *device_id) {
        safe_strncpy(g_device_id, device_id, sizeof(g_device_id));
    }

    g_server_mode = is_server;
    g_sync_enabled = 1;

    /* 创建同步缓存对象 */
    g_conflict_cache = cJSON_CreateObject();

    /* 订阅同步主题 */
    mqtt_client_subscribe(SYNC_TOPIC, 1);
    if (g_server_mode) {
        mqtt_client_subscribe("lingos/sync/request", 1);
        mqtt_client_subscribe("lingos/sync/reject", 1);
        LOG_INFO_T("MQTTSync", "Init", "Server", "subscribed to sync/request and sync/reject");
    }

    LOG_INFO_T("MQTTSync", "Init", "OK", "sync initialized as %s",
               g_server_mode ? tr("SERVER", "服务端") : tr("CLIENT", "客户端"));
    return 0;
}

/* ============================================================
 * 【修改】发布同步数据
 * ============================================================ */

int mqtt_sync_enhanced_publish(const char *type, const char *data) {
    LOG_DEBUG_T("MQTTSync", "Publish", "Enter", "type=%s", type ? type : "(null)");

    if (!type || !data) {
        LOG_ERROR_T("MQTTSync", "Publish", "Invalid", "type or data is NULL");
        return -1;
    }

    if (!g_sync_enabled) {
        LOG_DEBUG_T("MQTTSync", "Publish", "Disabled", "sync disabled");
        return 0;
    }

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", type);
    cJSON_AddStringToObject(root, "data", data);
    cJSON_AddStringToObject(root, "device_id", g_device_id);
    cJSON_AddNumberToObject(root, "timestamp", (double)time(NULL));
    cJSON_AddNumberToObject(root, "version", (double)time(NULL) * 1000);

    char *json_str = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);

    if (!json_str) {
        LOG_ERROR_T("MQTTSync", "Publish", "JSONFail", "cJSON_PrintUnformatted failed");
        return -1;
    }

    int ret = mqtt_client_publish(SYNC_TOPIC, json_str, strlen(json_str), 1, 0);
    free(json_str);

    LOG_DEBUG_T("MQTTSync", "Publish", "OK", "published type=%s", type);
    return ret;
}

/* ============================================================
 * 【新增】启用/禁用同步
 * ============================================================ */

void mqtt_sync_set_enabled(int enabled) {
    g_sync_enabled = enabled;
    LOG_INFO_T("MQTTSync", "SetEnabled", "sync %s", enabled ? tr("enabled", "启用") : tr("disabled", "禁用"));
}

int mqtt_sync_is_enabled(void) {
    return g_sync_enabled;
}

/* ============================================================
 * 【新增】清理冲突缓存
 * ============================================================ */

void mqtt_sync_clear_cache(void) {
    if (g_conflict_cache) {
        cJSON_Delete(g_conflict_cache);
        g_conflict_cache = cJSON_CreateObject();
        LOG_DEBUG_T("MQTTSync", "ClearCache", "OK", "conflict cache cleared");
    }
}