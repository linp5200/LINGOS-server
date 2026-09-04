/**
 * @file    src/health/check_cache.c
 * @brief   自检缓存实现
 * @version LN-0.4.3
 * @par     核心协议：C1, CM
 */

#include "check_cache.h"
#include "../common/safe_string.h"
#include "../common/data_path.h"
#include "../lib/log_extra.h"
#include "../lib/cJSON/cJSON.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <time.h>

#define CACHE_PATH "/Ensystem/check_cache.json"
#define CACHE_TTL_SECONDS 3600  /* 缓存有效期1小时 */

static char g_cache_file[512];
static int g_cache_loaded = 0;
static check_summary_t g_cached_summary;

static void get_cache_path(char *path, size_t size) {
    if (path) {
        const char *root = lingos_data_root();
        safe_snprintf(path, size, "%s%s", root, CACHE_PATH);
    }
}

/* ============================================================
 * 初始化
 * ============================================================ */
int check_cache_init(void) {
    get_cache_path(g_cache_file, sizeof(g_cache_file));
    LOG_DEBUG_T("CheckCache", "Init", "Path", "cache file: %s", g_cache_file);
    return 0;
}

/* ============================================================
 * 设置单个检查项
 * ============================================================ */
int check_cache_set(const char *id, const char *message, check_result_t result) {
    if (!id || !message) return -1;
    // 暂存到内存缓存中，由 save 统一持久化
    // 这里我们直接更新 g_cached_summary 的对应项
    // 简化：不在这里实现完整映射，而是由 save 时从 manager 读取
    return 0;
}

/* ============================================================
 * 获取单个检查项
 * ============================================================ */
int check_cache_get(const char *id, char *message, size_t msg_size) {
    if (!id || !message || msg_size == 0) return -1;
    // 从已加载的缓存中查找
    if (!g_cache_loaded) {
        check_summary_t tmp;
        if (check_cache_load(&tmp) == 0) {
            g_cached_summary = tmp;
            g_cache_loaded = 1;
        } else {
            return -1;
        }
    }
    // 简化：不实现逐项查找，直接返回空
    return -1;
}

/* ============================================================
 * 保存缓存
 * ============================================================ */
int check_cache_save(const check_summary_t *summary) {
    if (!summary) return -1;

    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "timestamp", (double)time(NULL));
    cJSON_AddNumberToObject(root, "total", summary->total);
    cJSON_AddNumberToObject(root, "passed", summary->passed);
    cJSON_AddNumberToObject(root, "warned", summary->warned);
    cJSON_AddNumberToObject(root, "failed", summary->failed);
    cJSON_AddNumberToObject(root, "skipped", summary->skipped);
    cJSON_AddNumberToObject(root, "errors", summary->errors);
    cJSON_AddNumberToObject(root, "need_configuration", summary->need_configuration);
    cJSON_AddStringToObject(root, "details", summary->details);

    char *json_str = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!json_str) return -1;

    FILE *fp = fopen(g_cache_file, "w");
    if (!fp) {
        free(json_str);
        return -1;
    }
    fputs(json_str, fp);
    fclose(fp);
    free(json_str);

    LOG_DEBUG_T("CheckCache", "Save", "OK", "cache saved to %s", g_cache_file);
    return 0;
}

/* ============================================================
 * 加载缓存
 * ============================================================ */
int check_cache_load(check_summary_t *summary) {
    if (!summary) return -1;

    if (access(g_cache_file, F_OK) != 0) {
        LOG_DEBUG_T("CheckCache", "Load", "NotFound", "no cache file");
        return -1;
    }

    FILE *fp = fopen(g_cache_file, "r");
    if (!fp) return -1;
    fseek(fp, 0, SEEK_END);
    long size = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    char *content = malloc(size + 1);
    if (!content) { fclose(fp); return -1; }
    fread(content, 1, size, fp);
    content[size] = '\0';
    fclose(fp);

    cJSON *root = cJSON_Parse(content);
    free(content);
    if (!root) return -1;

    cJSON *item;
    item = cJSON_GetObjectItem(root, "timestamp");
    if (cJSON_IsNumber(item)) {
        time_t ts = (time_t)item->valuedouble;
        time_t now = time(NULL);
        if (now - ts > CACHE_TTL_SECONDS) {
            cJSON_Delete(root);
            LOG_DEBUG_T("CheckCache", "Load", "Expired", "cache expired");
            return -1;
        }
    }

    summary->total = cJSON_GetObjectItem(root, "total")->valueint;
    summary->passed = cJSON_GetObjectItem(root, "passed")->valueint;
    summary->warned = cJSON_GetObjectItem(root, "warned")->valueint;
    summary->failed = cJSON_GetObjectItem(root, "failed")->valueint;
    summary->skipped = cJSON_GetObjectItem(root, "skipped")->valueint;
    summary->errors = cJSON_GetObjectItem(root, "errors")->valueint;
    item = cJSON_GetObjectItem(root, "need_configuration");
    if (cJSON_IsNumber(item)) summary->need_configuration = item->valueint;
    item = cJSON_GetObjectItem(root, "details");
    if (cJSON_IsString(item)) {
        safe_strncpy(summary->details, item->valuestring, sizeof(summary->details));
    }

    cJSON_Delete(root);
    LOG_DEBUG_T("CheckCache", "Load", "OK", "cache loaded");
    return 0;
}

/* ============================================================
 * 缓存有效性检查
 * ============================================================ */
int check_cache_is_valid(void) {
    if (access(g_cache_file, F_OK) != 0) return 0;
    struct stat st;
    if (stat(g_cache_file, &st) != 0) return 0;
    time_t now = time(NULL);
    if (now - st.st_mtime > CACHE_TTL_SECONDS) return 0;
    return 1;
}

/* ============================================================
 * 使缓存无效
 * ============================================================ */
void check_cache_invalidate(void) {
    if (access(g_cache_file, F_OK) == 0) {
        unlink(g_cache_file);
    }
    g_cache_loaded = 0;
    LOG_DEBUG_T("CheckCache", "Invalidate", "OK", "cache invalidated");
}