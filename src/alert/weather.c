/**
 * @file    weather.c
 * @brief   天气预警获取（使用 wttr.in 免费 API）
 * @version 2.0.0.0
 */

#include "weather.h"
#include "../common/data_path.h"
#include "log_extra.h"
#include "../net/tcp_client.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define WEATHER_CONFIG "/system/config/weather.conf"
#define DEFAULT_CITY "Shanghai"

static char current_city[128] = {0};

static void load_city(void) {
    if (current_city[0] != '\0') return;
    const char *root = lingos_data_root();
    char cfg_path[512];
    snprintf(cfg_path, sizeof(cfg_path), "%s%s", root, WEATHER_CONFIG);
    FILE *fp = fopen(cfg_path, "r");
    if (fp) {
        if (fgets(current_city, sizeof(current_city), fp)) {
            char *nl = strchr(current_city, '\n');
            if (nl) *nl = '\0';
        }
        fclose(fp);
    }
    if (current_city[0] == '\0') {
        strcpy(current_city, DEFAULT_CITY);
    }
}

void weather_set_city(const char *city) {
    if (!city) return;
    strncpy(current_city, city, sizeof(current_city)-1);
    const char *root = lingos_data_root();
    char cfg_path[512];
    snprintf(cfg_path, sizeof(cfg_path), "%s%s", root, WEATHER_CONFIG);
    FILE *fp = fopen(cfg_path, "w");
    if (fp) {
        fprintf(fp, "%s\n", city);
        fclose(fp);
    }
    LOG_INFO_T("Weather", "SetCity", "OK", "%s", city);
}

const char *weather_get_city(void) {
    load_city();
    return current_city;
}

char *weather_get_alert(void) {
    load_city();
    /* 使用 wttr.in 获取天气（简化输出）*/
    char host[64] = "wttr.in";
    char path[256];
    snprintf(path, sizeof(path), "/%s?format=%%C+%%t+%%w+%%h", current_city);
    char request[512];
    snprintf(request, sizeof(request),
        "GET %s HTTP/1.0\r\n"
        "Host: %s\r\n"
        "Connection: close\r\n"
        "\r\n",
        path, host);
    char response[8192];
    int ret = tcp_send_recv(host, 80, request, response, sizeof(response), 10000);
    if (ret != 0) {
        LOG_ERROR_T("Weather", "Get", "Fail", "tcp_send_recv error");
        return strdup("Failed to get weather data.");
    }
    char *body = strstr(response, "\r\n\r\n");
    if (!body) return strdup("Invalid response.");
    body += 4;
    /* 去除末尾换行 */
    char *nl = strchr(body, '\n');
    if (nl) *nl = '\0';
    char *result = malloc(strlen(body) + 64);
    sprintf(result, "Weather in %s: %s", current_city, body);
    return result;
}