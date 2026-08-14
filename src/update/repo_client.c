/**
 * @file    repo_client.c
 * @brief   远程仓库客户端（HTTP 下载、JSON 解析）
 * @version LN-B-5.0.0.0
 * @changes HTTPS 支持框架；安全字符串替换；双文支持
 */

#include "repo_client.h"
#include "../common/data_path.h"
#include "../common/safe_string.h"
#include "../common/lang.h"
#include "../lib/log_extra.h"
#include "../net/tcp_client.h"
#include "../lib/cJSON/cJSON.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <netdb.h>
#include <errno.h>

static char repo_url[256] = "repo.lingos.local";
static int repo_port = 80;
static int repo_use_https = 0;
static int repo_initialized = 0;

int repo_client_init(void) {
    if (repo_initialized) return 0;

    const char *root = lingos_data_root();
    char cfg_path[512];
    safe_snprintf(cfg_path, sizeof(cfg_path), "%s%s", root, REPO_CONFIG_PATH);

    FILE *fp = fopen(cfg_path, "r");
    if (fp) {
        char line[256];
        if (fgets(line, sizeof(line), fp)) {
            char *nl = strchr(line, '\n');
            if (nl) *nl = '\0';
            /* 检查是否为 HTTPS URL */
            if (strncmp(line, "https://", 8) == 0) {
                repo_use_https = 1;
                repo_port = 443;
                safe_strncpy(repo_url, line + 8, sizeof(repo_url));
            } else if (strncmp(line, "http://", 7) == 0) {
                repo_use_https = 0;
                repo_port = 80;
                safe_strncpy(repo_url, line + 7, sizeof(repo_url));
            } else {
                safe_strncpy(repo_url, line, sizeof(repo_url));
            }
            /* 提取端口（如有） */
            char *colon = strchr(repo_url, ':');
            if (colon) {
                *colon = '\0';
                repo_port = atoi(colon + 1);
            }
        }
        fclose(fp);
    } else {
        LOG_WARN_T("RepoClient", "Init", "NoConfig", "using default repo url");
    }

    repo_initialized = 1;
    LOG_INFO_T("RepoClient", "Init", "OK", "repo_url=%s:%d (https=%d)", repo_url, repo_port, repo_use_https);
    return 0;
}

/* ============================================================
 * HTTP GET 请求（支持 HTTPS 框架）
 * ============================================================ */
static char *http_get(const char *path) {
    LOG_DEBUG_T("RepoClient", "HTTPGet", "Enter", "path=%s", path);

    char request[1024];
    safe_snprintf(request, sizeof(request),
        "GET %s HTTP/1.0\r\n"
        "Host: %s\r\n"
        "Connection: close\r\n"
        "\r\n",
        path, repo_url);

    char response[65536];
    int ret = tcp_send_recv(repo_url, repo_port, request, response, sizeof(response), 10000);

    if (ret != 0) {
        LOG_ERROR_T("RepoClient", "HTTPGet", "Fail", "tcp_send_recv error (use_https=%d)", repo_use_https);
        if (repo_use_https) {
            LOG_WARN_T("RepoClient", "HTTPGet", "HTTPS", "HTTPS support requires OpenSSL integration");
        }
        return NULL;
    }

    char *body = strstr(response, "\r\n\r\n");
    if (!body) {
        LOG_ERROR_T("RepoClient", "HTTPGet", "ParseFail", "no HTTP body");
        return NULL;
    }
    body += 4;
    return strdup(body);
}

char *repo_download_index(void) {
    if (!repo_initialized) repo_client_init();
    char path[512];
    safe_snprintf(path, sizeof(path), "/%s", REPO_INDEX_FILE);
    LOG_DEBUG_T("RepoClient", "DownloadIndex", "Start", "path=%s", path);
    return http_get(path);
}

char *repo_search_app(const char *keyword) {
    char *index_json = repo_download_index();
    if (!index_json) return NULL;

    cJSON *root = cJSON_Parse(index_json);
    free(index_json);
    if (!root) {
        LOG_ERROR_T("RepoClient", "ParseIndex", "Fail", "invalid JSON");
        return NULL;
    }

    cJSON *apps = cJSON_GetObjectItem(root, "apps");
    if (!apps || !cJSON_IsArray(apps)) {
        cJSON_Delete(root);
        return NULL;
    }

    cJSON *result_array = cJSON_CreateArray();
    int size = cJSON_GetArraySize(apps);
    for (int i = 0; i < size; i++) {
        cJSON *app = cJSON_GetArrayItem(apps, i);
        cJSON *name = cJSON_GetObjectItem(app, "name");
        if (name && name->valuestring && strstr(name->valuestring, keyword)) {
            cJSON_AddItemToArray(result_array, cJSON_Duplicate(app, 1));
        }
    }

    char *result_str = cJSON_PrintUnformatted(result_array);
    cJSON_Delete(result_array);
    cJSON_Delete(root);
    return result_str;
}

char *repo_get_latest_version(const char *app_name) {
    char *index_json = repo_download_index();
    if (!index_json) return NULL;

    cJSON *root = cJSON_Parse(index_json);
    free(index_json);
    if (!root) return NULL;

    cJSON *apps = cJSON_GetObjectItem(root, "apps");
    if (!apps || !cJSON_IsArray(apps)) {
        cJSON_Delete(root);
        return NULL;
    }

    char *version = NULL;
    int size = cJSON_GetArraySize(apps);
    for (int i = 0; i < size; i++) {
        cJSON *app = cJSON_GetArrayItem(apps, i);
        cJSON *name = cJSON_GetObjectItem(app, "name");
        if (name && name->valuestring && strcmp(name->valuestring, app_name) == 0) {
            cJSON *ver = cJSON_GetObjectItem(app, "version");
            if (ver && ver->valuestring) {
                version = strdup(ver->valuestring);
                break;
            }
        }
    }
    cJSON_Delete(root);
    return version;
}

int repo_download_app(const char *app_name, const char *target_path) {
    if (!repo_initialized) repo_client_init();

    char path[512];
    safe_snprintf(path, sizeof(path), "/apps/%s.lapt", app_name);
    char *data = http_get(path);
    if (!data) return -1;

    FILE *fp = fopen(target_path, "wb");
    if (!fp) {
        free(data);
        return -1;
    }
    fwrite(data, 1, strlen(data), fp);
    fclose(fp);
    free(data);

    LOG_INFO_T("RepoClient", "DownloadApp", "OK", "%s -> %s", app_name, target_path);
    return 0;
}