/**
 * @file    mqtt_ha.c
 * @brief   Home Assistant 联动（Discovery + 命令订阅 + 状态上报）
 * @version LN-B-5.0.0.0
 * @changes 安全字符串替换（strncpy → safe_strncpy）；双文支持
 */

#include "mqtt_client.h"
#include "mqtt_sync.h"
#include "../common/safe_string.h"
#include "../common/lang.h"
#include "../lib/log_extra.h"
#include "../lib/cJSON/cJSON.h"
#include "../common/data_path.h"
#include "../core/version.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>

#define HA_DISCOVERY_PREFIX "homeassistant"
#define HA_COMMAND_TOPIC "lingos/command"

/* ============================================================
 * HA 设备信息
 * ============================================================ */

static char g_ha_device_id[64] = {0};
static int g_ha_initialized = 0;
static int g_ha_discovery_sent = 0;

/* ============================================================
 * 内部辅助：生成唯一设备 ID
 * ============================================================ */

static void generate_device_id(void) {
    if (g_ha_device_id[0] != '\0') return;

    char hostname[64];
    if (gethostname(hostname, sizeof(hostname)) == 0) {
        safe_snprintf(g_ha_device_id, sizeof(g_ha_device_id), "lingos_%s", hostname);
    } else {
        safe_snprintf(g_ha_device_id, sizeof(g_ha_device_id), "lingos_%d", getpid());
    }
}

/* ============================================================
 * 内部辅助：发送 HA Discovery 消息
 * ============================================================ */

static int send_discovery(const char *sensor_name, const char *unit,
                          const char *state_topic, const char *icon) {
    char topic[256];
    safe_snprintf(topic, sizeof(topic), "%s/sensor/%s/%s/config",
                  HA_DISCOVERY_PREFIX, g_ha_device_id, sensor_name);

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "name", sensor_name);
    cJSON_AddStringToObject(root, "stat_t", state_topic);
    if (unit) {
        cJSON_AddStringToObject(root, "unit_of_meas", unit);
    }
    if (icon) {
        cJSON_AddStringToObject(root, "icon", icon);
    }

    cJSON *device = cJSON_CreateObject();
    cJSON_AddStringToObject(device, "identifiers", g_ha_device_id);
    cJSON_AddStringToObject(device, "name", "LING OS");
    cJSON_AddStringToObject(device, "sw_version", version_get());
    cJSON_AddStringToObject(device, "model", tr("方向系统", "Direction System"));
    cJSON_AddItemToObject(root, "device", device);

    cJSON_AddStringToObject(root, "stat_cls", "measurement");

    char *json_str = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);

    if (!json_str) {
        LOG_ERROR_T("MQTTHA", "Discovery", "JSONFail", "cJSON_PrintUnformatted failed");
        return -1;
    }

    int ret = mqtt_client_publish(topic, json_str, strlen(json_str), 1, 1);
    free(json_str);

    if (ret == 0) {
        LOG_INFO_T("MQTTHA", "Discovery", "OK", "sent discovery for %s", sensor_name);
    } else {
        LOG_ERROR_T("MQTTHA", "Discovery", "Fail", "failed to send %s", sensor_name);
    }
    return ret;
}

/* ============================================================
 * 内部辅助：上报状态到 HA
 * ============================================================ */

static int report_state(const char *sensor_name, const char *value) {
    char topic[256];
    safe_snprintf(topic, sizeof(topic), "lingos/state/%s", sensor_name);

    int ret = mqtt_client_publish(topic, value, strlen(value), 1, 0);
    if (ret == 0) {
        LOG_DEBUG_T("MQTTHA", "State", "OK", "%s=%s", sensor_name, value);
    } else {
        LOG_ERROR_T("MQTTHA", "State", "Fail", "failed to report %s", sensor_name);
    }
    return ret;
}

/* ============================================================
 * 内部辅助：处理 HA 命令
 * ============================================================ */

static void on_ha_command(const mqtt_message_t *msg, void *user_data) {
    (void)user_data;

    LOG_INFO_T("MQTTHA", "Command", "Received", "payload='%s'", msg->payload);

    cJSON *root = cJSON_Parse(msg->payload);
    if (!root) {
        LOG_WARN_T("MQTTHA", "Command", "ParseFail", "invalid JSON");
        return;
    }

    cJSON *cmd = cJSON_GetObjectItem(root, "command");
    cJSON *args = cJSON_GetObjectItem(root, "args");
    cJSON *session = cJSON_GetObjectItem(root, "session_id");

    if (!cmd || !cJSON_IsString(cmd)) {
        cJSON_Delete(root);
        LOG_WARN_T("MQTTHA", "Command", "NoCmd", "missing command");
        return;
    }

    LOG_INFO_T("MQTTHA", "Command", "Execute", "cmd='%s'", cmd->valuestring);

    if (strcmp(cmd->valuestring, "nook_ask") == 0) {
        const char *prompt = args && cJSON_IsString(args) ? args->valuestring : "";
        const char *sid = session && cJSON_IsString(session) ? session->valuestring : "ha";
        LOG_INFO_T("MQTTHA", "Command", "NookAsk", "prompt='%s', session='%s'", prompt, sid);
        char sys_cmd[512];
        safe_snprintf(sys_cmd, sizeof(sys_cmd),
                      "lingos_linux -c 'nook ask \"%s\"' > /tmp/ha_response.log 2>&1 &",
                      prompt);
        system(sys_cmd);
    } else if (strcmp(cmd->valuestring, "system_status") == 0) {
        char status[256];
        safe_snprintf(status, sizeof(status),
                      "{\"status\":\"ok\",\"uptime\":%ld,\"version\":\"%s\"}",
                      time(NULL), version_get());
        mqtt_client_publish("lingos/state/status", status, strlen(status), 1, 0);
    } else {
        LOG_WARN_T("MQTTHA", "Command", "Unknown", "unknown command: %s", cmd->valuestring);
    }

    cJSON_Delete(root);
}

/* ============================================================
 * 公共 API
 * ============================================================ */

int mqtt_ha_init(void) {
    LOG_INFO_T("MQTTHA", "Init", "Enter", "initializing HA integration");

    generate_device_id();

    if (g_ha_initialized) {
        LOG_WARN_T("MQTTHA", "Init", "Already", "already initialized");
        return 0;
    }

    mqtt_client_subscribe(HA_COMMAND_TOPIC, 1);
    mqtt_client_set_callback(on_ha_command, NULL);

    g_ha_initialized = 1;
    LOG_INFO_T("MQTTHA", "Init", "OK", "HA integration initialized, device='%s'", g_ha_device_id);
    return 0;
}

int mqtt_ha_send_discovery(void) {
    LOG_INFO_T("MQTTHA", "SendDiscovery", "Enter", "sending discovery messages");

    if (!g_ha_initialized) {
        LOG_ERROR_T("MQTTHA", "SendDiscovery", "NotInit", "HA not initialized");
        return -1;
    }

    send_discovery("CPU_Usage", "%", "lingos/state/cpu", "mdi:cpu-64-bit");
    send_discovery("Memory_Usage", "%", "lingos/state/memory", "mdi:memory");
    send_discovery("Disk_Usage", "%", "lingos/state/disk", "mdi:harddisk");
    send_discovery("Load_Avg", "", "lingos/state/load", "mdi:chart-line");
    send_discovery("AI_Status", "", "lingos/state/ai_status", "mdi:robot");

    g_ha_discovery_sent = 1;
    LOG_INFO_T("MQTTHA", "SendDiscovery", "OK", "discovery messages sent");
    return 0;
}

int mqtt_ha_report_cpu(int percent) {
    char buf[16];
    safe_snprintf(buf, sizeof(buf), "%d", percent);
    return report_state("cpu", buf);
}

int mqtt_ha_report_memory(int percent) {
    char buf[16];
    safe_snprintf(buf, sizeof(buf), "%d", percent);
    return report_state("memory", buf);
}

int mqtt_ha_report_disk(int percent) {
    char buf[16];
    safe_snprintf(buf, sizeof(buf), "%d", percent);
    return report_state("disk", buf);
}

int mqtt_ha_report_load(double load) {
    char buf[32];
    safe_snprintf(buf, sizeof(buf), "%.2f", load);
    return report_state("load", buf);
}

int mqtt_ha_report_ai_status(int online) {
    const char *status = online ? tr("online", "在线") : tr("offline", "离线");
    return report_state("ai_status", status);
}

int mqtt_ha_is_discovery_sent(void) {
    return g_ha_discovery_sent;
}

void mqtt_ha_cleanup(void) {
    LOG_INFO_T("MQTTHA", "Cleanup", "Enter", "cleaning up HA integration");

    g_ha_initialized = 0;
    g_ha_discovery_sent = 0;

    LOG_INFO_T("MQTTHA", "Cleanup", "OK", "HA integration cleaned up");
}