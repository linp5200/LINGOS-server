/**
 * @file    mqtt_ha.c
 * @brief   Home Assistant 联动（Discovery + 命令订阅 + 状态上报）
 * @version LN-B-5.0.0.0
 * @changes 安全字符串替换（strncpy → safe_strncpy）；双文支持
 */

#include "mqtt_ha.h"      /* 本模块接口（此前未自包含 → 原型/类型失联） */
#include "mqtt_client.h"
#include "mqtt_sync.h"
#include "../common/safe_string.h"
#include "../common/lang.h"
#include "../lib/log_extra.h"
#include "../lib/cJSON/cJSON.h"
#include "../common/data_path.h"
#include "../core/version.h"
#include "../health/system_health.h"     /* 真实指标：get_memory_usage/get_disk_usage/get_load_avg */
#include "../ai/ai_server_protocol.h"    /* AI_SOCKET_PATH：真实 AI 服务探活 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/select.h>

#define HA_DISCOVERY_PREFIX "homeassistant"
#define HA_COMMAND_TOPIC "lingos/command"
#define HA_REJECT_TOPIC "lingos/command/rejected"
#define HA_AVAILABILITY_TOPIC "lingos/state/availability"

/* ============================================================
 * HA 设备信息
 * ============================================================ */

static char g_ha_device_id[64] = {0};
static int g_ha_initialized = 0;
static int g_ha_discovery_sent = 0;
static mqtt_ha_review_fn g_ha_review_hook = NULL;
static pthread_mutex_t g_ha_hook_lock = PTHREAD_MUTEX_INITIALIZER;
static int g_ha_cmd_handler = -1;

/* 最近一次真实采样值（-1 = 本次不可用；用于对外提供真实指标快照） */
static int g_last_cpu = -1;
static int g_last_mem = -1;
static int g_last_disk = -1;
static int g_last_ai_online = 0;
static double g_last_load = -1.0;

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

    char uniq_id[160];
    safe_snprintf(uniq_id, sizeof(uniq_id), "%s_%s", g_ha_device_id, sensor_name);

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "name", sensor_name);
    cJSON_AddStringToObject(root, "uniq_id", uniq_id);
    cJSON_AddStringToObject(root, "stat_t", state_topic);
    cJSON_AddStringToObject(root, "avty_t", HA_AVAILABILITY_TOPIC);
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
    /* 修复：tr(en, zh) 参数顺序写反（中文环境显示英文、英文环境显示中文） */
    cJSON_AddStringToObject(device, "model", tr("Direction System", "方向系统"));
    cJSON_AddItemToObject(root, "device", device);

    /* HA 传感器发现键：state_class（此前误写 stat_cls → 传感器无状态类别） */
    cJSON_AddStringToObject(root, "state_class", "measurement");

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
 * 内部辅助：命令审查门（真实审查结果，未注册则 fail-closed）
 * ============================================================ */

static mqtt_ha_review_fn get_review_hook(void) {
    pthread_mutex_lock(&g_ha_hook_lock);
    mqtt_ha_review_fn fn = g_ha_review_hook;
    pthread_mutex_unlock(&g_ha_hook_lock);
    return fn;
}

/* 清洗并构造拒绝回执的 JSON（引号/反斜杠/换行替换为空格） */
static void sanitize_json_value(const char *src, char *dst, size_t dst_len) {
    size_t di = 0;
    if (!dst || dst_len == 0) return;
    for (const char *p = src; p && *p && di + 1 < dst_len; p++) {
        char c = *p;
        if (c == '"' || c == '\\' || c == '\n' || c == '\r' || c == '\t') c = ' ';
        dst[di++] = c;
    }
    dst[di] = '\0';
}

static void publish_command_rejection(const char *cmd, const char *reason) {
    char safe_reason[192];
    char payload[384];

    sanitize_json_value(reason ? reason : "rejected", safe_reason, sizeof(safe_reason));
    safe_snprintf(payload, sizeof(payload),
                  "{\"command\":\"%s\",\"status\":\"rejected\",\"reason\":\"%s\",\"ts\":%ld}",
                  cmd ? cmd : "", safe_reason, (long)time(NULL));
    if (mqtt_client_publish(HA_REJECT_TOPIC, payload, (int)strlen(payload), 1, 0) != 0) {
        LOG_WARN_T("MQTTHA", "Command", "RejectPublishFail",
                   "could not publish rejection for '%s'", cmd ? cmd : "");
    }
}

static void on_ha_command(const mqtt_message_t *msg, void *user_data) {
    (void)user_data;

    if (!msg) return;

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

    /* 参数原文（供审查器判断；最多 512B） */
    char args_json[512];
    args_json[0] = '\0';
    if (args) {
        char *printed = cJSON_PrintUnformatted(args);
        if (printed) {
            safe_strncpy(args_json, printed, sizeof(args_json));
            free(printed);
        }
    }

    /* 【真实审查门】MQTT 命令来自网络（不可信）→ 必须经注册审查器
     *   （ai_master Guard 审查；见 ai_master_init → mqtt_ha_set_review_hook）。
     *   未注册审查器时 fail-closed：不执行，并回执拒绝原因。 */
    char reason[256];
    reason[0] = '\0';
    mqtt_ha_review_fn hook = get_review_hook();
    int verdict;
    if (!hook) {
        safe_strncpy(reason,
                     tr("no command reviewer registered (fail-closed)",
                        "未注册命令审查器（默认拒绝）"),
                     sizeof(reason));
        verdict = -1;
    } else {
        verdict = hook(cmd->valuestring, args_json, reason, sizeof(reason));
    }

    if (verdict != 0) {
        if (reason[0] == '\0') {
            safe_strncpy(reason, tr("rejected by reviewer", "审查未通过"), sizeof(reason));
        }
        LOG_WARN_T("MQTTHA", "Command", "Rejected", "cmd='%s', reason='%s'",
                   cmd->valuestring, reason);
        publish_command_rejection(cmd->valuestring, reason);
        cJSON_Delete(root);
        return;
    }

    LOG_INFO_T("MQTTHA", "Command", "Execute", "cmd='%s'", cmd->valuestring);

    if (strcmp(cmd->valuestring, "nook_ask") == 0) {
        const char *prompt = args && cJSON_IsString(args) ? args->valuestring : "";
        const char *sid = session && cJSON_IsString(session) ? session->valuestring : "ha";
        LOG_INFO_T("MQTTHA", "Command", "NookAsk", "prompt='%s', session='%s'", prompt, sid);
        /* 【0.7.0-hf2 安全】prompt 来自 MQTT（网络输入，不可信）——
         *   过滤 shell 元字符（此前可经 " 或 ' 逃逸单双引号 → 远程命令注入 RCE！） */
        char safe_prompt[400];
        size_t pi = 0;
        for (const char *p = prompt; p && *p && pi < sizeof(safe_prompt) - 1; p++) {
            char c = *p;
            if (c == '"' || c == '\'' || c == '`' || c == '$' || c == '\\' ||
                c == ';' || c == '|' || c == '&' || c == '<' || c == '>' ||
                c == '\n' || c == '\r' || c == '(' || c == ')')
                continue;   /* 跳过危险字符（防注入） */
            safe_prompt[pi++] = c;
        }
        safe_prompt[pi] = '\0';
        if (pi == 0) {
            /* 修复：此前此处直接 return → cJSON 树泄漏（root 未 Delete） */
            LOG_WARN_T("MQTTHA", "Command", "NookAskEmpty", "prompt empty after sanitize — skipped");
        } else {
            char sys_cmd[512];
            safe_snprintf(sys_cmd, sizeof(sys_cmd),
                          "lingos_linux -c 'nook ask \"%s\"' > /tmp/ha_response.log 2>&1 &",
                          safe_prompt);
            int rc = system(sys_cmd);
            if (rc != 0) {
                LOG_WARN_T("MQTTHA", "Command", "NookAskSpawn", "system() returned %d", rc);
            }
        }
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

    /* 订阅命令主题（未连接时登记，连接/重连后由 mqtt_client 自动重订阅） */
    int sub_rc = mqtt_client_subscribe(HA_COMMAND_TOPIC, 1);
    if (sub_rc != 0) {
        LOG_WARN_T("MQTTHA", "Init", "SubscribeFail",
                   "command topic subscribe failed (rc=%d), will retry on connect", sub_rc);
    }

    /* 用多处理器注册（不再用全局 set_callback 覆盖同步模块的回调） */
    if (g_ha_cmd_handler < 0) {
        g_ha_cmd_handler = mqtt_client_add_message_handler(HA_COMMAND_TOPIC, on_ha_command, NULL);
        if (g_ha_cmd_handler < 0) {
            LOG_ERROR_T("MQTTHA", "Init", "HandlerFail", "could not register command handler");
            return -1;
        }
    }

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

    if (!mqtt_client_is_connected()) {
        LOG_WARN_T("MQTTHA", "SendDiscovery", "NotConnected",
                   "MQTT not connected, discovery deferred");
        return -1;
    }

    int ok = 0;
    if (send_discovery("CPU_Usage", "%", "lingos/state/cpu", "mdi:cpu-64-bit") == 0) ok++;
    if (send_discovery("Memory_Usage", "%", "lingos/state/memory", "mdi:memory") == 0) ok++;
    if (send_discovery("Disk_Usage", "%", "lingos/state/disk", "mdi:harddisk") == 0) ok++;
    if (send_discovery("Load_Avg", "", "lingos/state/load", "mdi:chart-line") == 0) ok++;
    if (send_discovery("AI_Status", "", "lingos/state/ai_status", "mdi:robot") == 0) ok++;

    if (ok < 5) {
        /* 诚实：部分/全部失败 → 不置"已发送"，由服务监督线程重试 */
        g_ha_discovery_sent = 0;
        LOG_WARN_T("MQTTHA", "SendDiscovery", "Partial",
                   "%d/5 discovery messages published, will retry", ok);
        return -1;
    }

    g_ha_discovery_sent = 1;
    LOG_INFO_T("MQTTHA", "SendDiscovery", "OK", "all 5 discovery messages published");
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

    if (g_ha_cmd_handler >= 0) {
        mqtt_client_remove_message_handler(g_ha_cmd_handler);
        g_ha_cmd_handler = -1;
    }
    mqtt_client_unsubscribe(HA_COMMAND_TOPIC);

    g_ha_initialized = 0;
    g_ha_discovery_sent = 0;

    LOG_INFO_T("MQTTHA", "Cleanup", "OK", "HA integration cleaned up");
}

/* ============================================================
 * 公共 API：命令审查钩子
 * ============================================================ */

int mqtt_ha_set_review_hook(mqtt_ha_review_fn fn) {
    pthread_mutex_lock(&g_ha_hook_lock);
    g_ha_review_hook = fn;
    pthread_mutex_unlock(&g_ha_hook_lock);

    if (fn) {
        LOG_INFO_T("MQTTHA", "ReviewHook", "Set", "command reviewer registered");
    } else {
        LOG_WARN_T("MQTTHA", "ReviewHook", "Cleared",
                   "command reviewer cleared (commands will be fail-closed)");
    }
    return 0;
}

int mqtt_ha_has_review_hook(void) {
    return get_review_hook() != NULL ? 1 : 0;
}

const char *mqtt_ha_availability_topic(void) {
    return HA_AVAILABILITY_TOPIC;
}

int mqtt_ha_publish_availability(int online) {
    const char *payload = online ? "online" : "offline";
    int ret = mqtt_client_publish(HA_AVAILABILITY_TOPIC, payload, (int)strlen(payload), 1, 1);
    if (ret == 0) {
        LOG_INFO_T("MQTTHA", "Availability", "OK", "%s", payload);
    } else {
        LOG_WARN_T("MQTTHA", "Availability", "Fail", "could not publish '%s'", payload);
    }
    return ret;
}

/* ============================================================
 * 真实系统指标采集（CPU/内存/磁盘/负载/AI 探活）
 * ============================================================ */

/* /proc/stat 首行 → 累计 idle / total 时钟滴答 */
static int read_cpu_ticks(unsigned long long *idle_out, unsigned long long *total_out) {
    FILE *fp = fopen("/proc/stat", "r");
    if (!fp) return -1;

    char line[256];
    if (!fgets(line, sizeof(line), fp)) {
        fclose(fp);
        return -1;
    }
    fclose(fp);

    unsigned long long v[8] = {0, 0, 0, 0, 0, 0, 0, 0};
    int n = sscanf(line, "cpu %llu %llu %llu %llu %llu %llu %llu %llu",
                   &v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &v[6], &v[7]);
    if (n < 4) return -1;

    *idle_out = v[3] + v[4];   /* idle + iowait */
    *total_out = v[0] + v[1] + v[2] + v[3] + v[4] + v[5] + v[6] + v[7];
    return 0;
}

static int read_cpu_percent(int *out) {
    static unsigned long long prev_idle = 0, prev_total = 0;
    unsigned long long idle = 0, total = 0;

    if (!out) return -1;
    if (read_cpu_ticks(&idle, &total) != 0) return -1;

    if (prev_total == 0) {
        /* 首次调用无基线：现场采两次（间隔 200ms）得到真实差分 */
        prev_idle = idle;
        prev_total = total;
        usleep(200 * 1000);
        if (read_cpu_ticks(&idle, &total) != 0) return -1;
    }

    if (total <= prev_total) {
        prev_idle = idle;
        prev_total = total;
        return -1;
    }

    unsigned long long dtotal = total - prev_total;
    unsigned long long didle = idle - prev_idle;
    prev_idle = idle;
    prev_total = total;

    if (dtotal == 0) return -1;
    long pct = (long)((dtotal - didle) * 100 / dtotal);
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    *out = (int)pct;
    return 0;
}

/* 真实 AI 服务探活：AF_UNIX 连接 ai_server.py 的 socket（200ms 超时） */
static int probe_ai_online(void) {
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return 0;

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    safe_strncpy(addr.sun_path, AI_SOCKET_PATH, sizeof(addr.sun_path));

    int flags = fcntl(fd, F_GETFL, 0);
    if (flags >= 0) {
        fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    }

    int online = 0;
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) == 0) {
        online = 1;
    } else if (errno == EINPROGRESS) {
        fd_set wf;
        FD_ZERO(&wf);
        FD_SET(fd, &wf);
        struct timeval tv;
        tv.tv_sec = 0;
        tv.tv_usec = 200 * 1000;
        if (select(fd + 1, NULL, &wf, NULL, &tv) > 0) {
            int so_err = 0;
            socklen_t slen = sizeof(so_err);
            if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &so_err, &slen) == 0 && so_err == 0) {
                online = 1;
            }
        }
    }

    close(fd);
    return online;
}

int mqtt_ha_report_system(void) {
    if (!g_ha_initialized) {
        LOG_ERROR_T("MQTTHA", "Report", "NotInit", "HA not initialized");
        return -1;
    }
    if (!mqtt_client_is_connected()) {
        /* 降级：未连接时跳过本轮（不伪造、不缓存、不重试风暴），由监督线程下轮再报 */
        LOG_WARN_T("MQTTHA", "Report", "Skip", "MQTT not connected, metrics cycle skipped");
        return -1;
    }

    int published = 0;

    g_last_cpu = -1;
    g_last_mem = -1;
    g_last_disk = -1;
    g_last_load = -1.0;

    int cpu = 0;
    if (read_cpu_percent(&cpu) == 0) {
        g_last_cpu = cpu;
        if (mqtt_ha_report_cpu(cpu) == 0) published++;
    } else {
        LOG_WARN_T("MQTTHA", "Report", "CpuUnavailable", "/proc/stat unreadable, cpu not reported");
    }

    int mem = get_memory_usage();
    if (mem >= 0) {
        g_last_mem = mem;
        if (mqtt_ha_report_memory(mem) == 0) published++;
    } else {
        LOG_WARN_T("MQTTHA", "Report", "MemUnavailable", "sysinfo failed, memory not reported");
    }

    int disk = get_disk_usage("/");
    if (disk >= 0) {
        g_last_disk = disk;
        if (mqtt_ha_report_disk(disk) == 0) published++;
    } else {
        LOG_WARN_T("MQTTHA", "Report", "DiskUnavailable", "statvfs failed, disk not reported");
    }

    double load1 = -1.0, load5 = -1.0, load15 = -1.0;
    get_load_avg(&load1, &load5, &load15);
    if (load1 >= 0.0) {
        g_last_load = load1;
        if (mqtt_ha_report_load(load1) == 0) published++;
    } else {
        LOG_WARN_T("MQTTHA", "Report", "LoadUnavailable", "/proc/loadavg unreadable, load not reported");
    }

    int ai_online = probe_ai_online();
    g_last_ai_online = ai_online;
    if (mqtt_ha_report_ai_status(ai_online) == 0) published++;

    LOG_INFO_T("MQTTHA", "Report", "System", "published=%d/5, ai_online=%d", published, ai_online);
    return published;
}

int mqtt_ha_get_last_metrics(int *cpu, int *mem, int *disk, double *load, int *ai_online) {
    int sampled = 0;

    if (g_last_cpu >= 0 || g_last_mem >= 0 || g_last_disk >= 0 ||
        g_last_load >= 0.0 || g_last_ai_online) {
        sampled = 1;
    }

    if (cpu) *cpu = g_last_cpu;
    if (mem) *mem = g_last_mem;
    if (disk) *disk = g_last_disk;
    if (load) *load = g_last_load;
    if (ai_online) *ai_online = g_last_ai_online;
    return sampled;
}