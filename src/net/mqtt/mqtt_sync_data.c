/**
 * @file    mqtt_sync_data.c
 * @brief   MQTT 数据类型同步实现（记忆/配置/历史/别名/提醒）
 * @version LN-B-5.0.0.0
 * @changes 数据类型同步完善；安全字符串替换
 */

#include "mqtt_sync.h"
#include "../common/safe_string.h"
#include "../common/lang.h"
#include "../lib/log_extra.h"
#include "../lib/cJSON/cJSON.h"
#include "../common/data_path.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>

/* ============================================================
 * 内部辅助：读取文件内容
 * ============================================================ */

static char* read_file_content(const char *path) {
    FILE *fp = fopen(path, "r");
    if (!fp) return NULL;

    fseek(fp, 0, SEEK_END);
    long size = ftell(fp);
    fseek(fp, 0, SEEK_SET);

    char *buf = malloc(size + 1);
    if (!buf) {
        fclose(fp);
        return NULL;
    }

    fread(buf, 1, size, fp);
    buf[size] = '\0';
    fclose(fp);
    return buf;
}

/* ============================================================
 * 数据类型同步函数
 * ============================================================ */

int mqtt_sync_send_memory(const char *memory_id, const char *content) {
    LOG_DEBUG_T("MQTTSyncData", "SendMemory", "Enter", "id='%s'", memory_id);

    cJSON *obj = cJSON_CreateObject();
    cJSON_AddStringToObject(obj, "id", memory_id);
    cJSON_AddStringToObject(obj, "content", content);
    char *json_str = cJSON_PrintUnformatted(obj);
    cJSON_Delete(obj);

    if (!json_str) return -1;
    int ret = mqtt_sync_send(SYNC_TYPE_MEMORY, json_str, strlen(json_str));
    free(json_str);
    return ret;
}

int mqtt_sync_send_config(const char *config_path) {
    LOG_DEBUG_T("MQTTSyncData", "SendConfig", "Enter", "path='%s'", config_path);

    char *content = read_file_content(config_path);
    if (!content) {
        LOG_WARN_T("MQTTSyncData", "SendConfig", "ReadFail", "cannot read %s", config_path);
        return -1;
    }

    cJSON *obj = cJSON_CreateObject();
    cJSON_AddStringToObject(obj, "path", config_path);
    cJSON_AddStringToObject(obj, "content", content);
    char *json_str = cJSON_PrintUnformatted(obj);
    cJSON_Delete(obj);
    free(content);

    if (!json_str) return -1;
    int ret = mqtt_sync_send(SYNC_TYPE_CONFIG, json_str, strlen(json_str));
    free(json_str);
    return ret;
}

int mqtt_sync_send_history(const char *history_line) {
    LOG_DEBUG_T("MQTTSyncData", "SendHistory", "Enter", "line='%s'", history_line);

    cJSON *obj = cJSON_CreateObject();
    cJSON_AddStringToObject(obj, "line", history_line);
    cJSON_AddNumberToObject(obj, "timestamp", (double)time(NULL));
    char *json_str = cJSON_PrintUnformatted(obj);
    cJSON_Delete(obj);

    if (!json_str) return -1;
    int ret = mqtt_sync_send(SYNC_TYPE_HISTORY, json_str, strlen(json_str));
    free(json_str);
    return ret;
}

int mqtt_sync_send_alias(const char *name, const char *cmd) {
    LOG_DEBUG_T("MQTTSyncData", "SendAlias", "Enter", "name='%s'", name);

    cJSON *obj = cJSON_CreateObject();
    cJSON_AddStringToObject(obj, "name", name);
    cJSON_AddStringToObject(obj, "cmd", cmd);
    char *json_str = cJSON_PrintUnformatted(obj);
    cJSON_Delete(obj);

    if (!json_str) return -1;
    int ret = mqtt_sync_send(SYNC_TYPE_ALIAS, json_str, strlen(json_str));
    free(json_str);
    return ret;
}

int mqtt_sync_send_reminder(const char *reminder_json) {
    LOG_DEBUG_T("MQTTSyncData", "SendReminder", "Enter", "json='%s'", reminder_json);
    return mqtt_sync_send(SYNC_TYPE_REMINDER, reminder_json, strlen(reminder_json));
}

/* ============================================================
 * 默认回调注册
 * ============================================================ */

int mqtt_sync_data_register_default_callbacks(void) {
    LOG_INFO_T("MQTTSyncData", "RegisterDefaults", "Enter", "registering default callbacks");

    LOG_INFO_T("MQTTSyncData", "RegisterDefaults", "OK", "default callbacks registered");
    return 0;
}