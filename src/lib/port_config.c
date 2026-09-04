/**
 * @file    port_config.c
 * @brief   端口配置（2026-08-22 先生裁决：端口不可在配置向导中更改——
 *          使用特定指令 port 指令族；本模块从 ports.json 读取覆盖默认宏）
 * @version LN-0.4.3
 * @par     核心协议：C-C 防弹/容错/跛脚 + C1 分级日志
 */

#include "port_config.h"
#include "../common/safe_string.h"
#include "../common/data_path.h"
#include "../lib/log_extra.h"
#include "../lib/cJSON/cJSON.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PORTS_FILE "ports.json"

/* 缓存：0=未加载 */
static int g_ws_port = 0;
static int g_http_port = 0;
static int g_tcp_port = 0;

static const char* ports_path(void) {
    static char path[512];
    const char *root = lingos_data_root();
    safe_snprintf(path, sizeof(path), "%s/system/config/%s", root, PORTS_FILE);
    return path;
}

static void port_config_load(void) {
    const char *path = ports_path();
    FILE *fp = fopen(path, "r");
    if (!fp) {
        g_ws_port = WEBSOCKET_PORT_DEFAULT;
        g_http_port = HTTP_PORT_DEFAULT;
        g_tcp_port = TCP_PORT_DEFAULT;
        return;
    }
    char buf[2048];
    size_t n = fread(buf, 1, sizeof(buf) - 1, fp);
    buf[n] = '\0';
    fclose(fp);

    cJSON *root = cJSON_Parse(buf);
    if (!root) {
        g_ws_port = WEBSOCKET_PORT_DEFAULT;
        g_http_port = HTTP_PORT_DEFAULT;
        g_tcp_port = TCP_PORT_DEFAULT;
        return;
    }
    cJSON *w = cJSON_GetObjectItem(root, "ws");
    cJSON *h = cJSON_GetObjectItem(root, "http");
    cJSON *t = cJSON_GetObjectItem(root, "tcp");
    g_ws_port = (cJSON_IsNumber(w)) ? w->valueint : WEBSOCKET_PORT_DEFAULT;
    g_http_port = (cJSON_IsNumber(h)) ? h->valueint : HTTP_PORT_DEFAULT;
    g_tcp_port = (cJSON_IsNumber(t)) ? t->valueint : TCP_PORT_DEFAULT;
    /* 合法性（防恶意配置） */
    if (g_ws_port < 1024 || g_ws_port > 65535) g_ws_port = WEBSOCKET_PORT_DEFAULT;
    if (g_http_port < 1024 || g_http_port > 65535) g_http_port = HTTP_PORT_DEFAULT;
    if (g_tcp_port < 1024 || g_tcp_port > 65535) g_tcp_port = TCP_PORT_DEFAULT;
    cJSON_Delete(root);
    LOG_INFO_T("PortCfg", "Load", "OK", "ports ws=%d http=%d tcp=%d", g_ws_port, g_http_port, g_tcp_port);
}

int port_config_get(port_type_t type) {
    if (g_ws_port == 0) port_config_load();
    switch (type) {
        case PORT_WS: return g_ws_port;
        case PORT_HTTP: return g_http_port;
        case PORT_TCP: return g_tcp_port;
    }
    return 0;
}

int port_config_set(port_type_t type, int port) {
    if (port < 1024 || port > 65535) return -1;
    if (g_ws_port == 0) port_config_load();
    switch (type) {
        case PORT_WS: g_ws_port = port; break;
        case PORT_HTTP: g_http_port = port; break;
        case PORT_TCP: g_tcp_port = port; break;
    }
    /* 写回 ports.json */
    char path[512];
    const char *root = lingos_data_root();
    safe_snprintf(path, sizeof(path), "%s/system/config/%s", root, PORTS_FILE);
    char dir[512];
    safe_snprintf(dir, sizeof(dir), "%s/system/config", root);
    char mk[600];
    safe_snprintf(mk, sizeof(mk), "mkdir -p \"%s\" 2>/dev/null", dir);
    (void)system(mk);

    cJSON *rootj = cJSON_CreateObject();
    cJSON_AddNumberToObject(rootj, "ws", g_ws_port);
    cJSON_AddNumberToObject(rootj, "http", g_http_port);
    cJSON_AddNumberToObject(rootj, "tcp", g_tcp_port);
    char *json = cJSON_PrintUnformatted(rootj);
    cJSON_Delete(rootj);
    if (!json) return -1;
    FILE *fp = fopen(path, "w");
    if (!fp) { free(json); return -1; }
    fputs(json, fp);
    fclose(fp);
    free(json);
    LOG_INFO_T("PortCfg", "Set", "OK", "port type=%d -> %d (restart to apply)", type, port);
    return 0;
}

const char* port_config_name(port_type_t type) {
    switch (type) {
        case PORT_WS: return "ws";
        case PORT_HTTP: return "http";
        case PORT_TCP: return "tcp";
    }
    return "?";
}
