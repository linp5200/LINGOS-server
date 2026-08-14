/**
 * @file    src/install/install_cache.c
 * @brief   离线缓存实现
 * @version LN-B-5.1.2.6-rc
 * @par     核心协议：C1, CM
 */

#include "install_cache.h"
#include "../common/safe_string.h"
#include "../common/data_path.h"
#include "../lib/log_extra.h"
#include "../lib/cJSON/cJSON.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

#define CACHE_PATH "/Ensystem/install_cache.json"

static cJSON *g_cache_root = NULL;
static int g_cache_loaded = 0;

static const char* get_cache_path(void) {
    static char path[512];
    if (path[0] == '\0') {
        const char *root = lingos_data_root();
        safe_snprintf(path, sizeof(path), "%s%s", root, CACHE_PATH);
    }
    return path;
}

/* ============================================================
 * 加载缓存
 * ============================================================ */
int install_cache_load(void) {
    if (g_cache_loaded) return 0;

    const char *path = get_cache_path();
    if (access(path, F_OK) != 0) {
        g_cache_root = cJSON_CreateObject();
        g_cache_loaded = 1;
        return 0;
    }

    FILE *fp = fopen(path, "r");
    if (!fp) {
        g_cache_root = cJSON_CreateObject();
        g_cache_loaded = 1;
        return -1;
    }
    fseek(fp, 0, SEEK_END);
    long size = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    char *content = malloc(size + 1);
    if (!content) {
        fclose(fp);
        g_cache_root = cJSON_CreateObject();
        g_cache_loaded = 1;
        return -1;
    }
    fread(content, 1, size, fp);
    content[size] = '\0';
    fclose(fp);

    g_cache_root = cJSON_Parse(content);
    free(content);
    if (!g_cache_root) {
        g_cache_root = cJSON_CreateObject();
    }
    g_cache_loaded = 1;
    return 0;
}

/* ============================================================
 * 保存缓存
 * ============================================================ */
int install_cache_save(void) {
    if (!g_cache_loaded || !g_cache_root) return -1;

    const char *path = get_cache_path();
    char *json_str = cJSON_PrintUnformatted(g_cache_root);
    if (!json_str) return -1;

    FILE *fp = fopen(path, "w");
    if (!fp) {
        free(json_str);
        return -1;
    }
    fputs(json_str, fp);
    fclose(fp);
    free(json_str);

    LOG_DEBUG_T("InstallCache", "Save", "OK", "cache saved to %s", path);
    return 0;
}

/* ============================================================
 * 检查缓存
 * ============================================================ */
int install_cache_has(const char *pkg_name, const char *type) {
    if (!pkg_name || !type) return 0;
    if (install_cache_load() != 0) return 0;

    cJSON *type_obj = cJSON_GetObjectItem(g_cache_root, type);
    if (!type_obj) return 0;

    for (int i = 0; i < cJSON_GetArraySize(type_obj); i++) {
        cJSON *item = cJSON_GetArrayItem(type_obj, i);
        if (cJSON_IsString(item) && strcmp(item->valuestring, pkg_name) == 0) {
            return 1;
        }
    }
    return 0;
}

/* ============================================================
 * 添加到缓存
 * ============================================================ */
int install_cache_add(const char *pkg_name, const char *type) {
    if (!pkg_name || !type) return -1;
    if (install_cache_load() != 0) return -1;

    cJSON *type_obj = cJSON_GetObjectItem(g_cache_root, type);
    if (!type_obj) {
        type_obj = cJSON_CreateArray();
        cJSON_AddItemToObject(g_cache_root, type, type_obj);
    }

    // 检查是否已存在
    for (int i = 0; i < cJSON_GetArraySize(type_obj); i++) {
        cJSON *item = cJSON_GetArrayItem(type_obj, i);
        if (cJSON_IsString(item) && strcmp(item->valuestring, pkg_name) == 0) {
            return 0;  // 已存在
        }
    }

    cJSON_AddItemToArray(type_obj, cJSON_CreateString(pkg_name));
    return install_cache_save();
}

/* ============================================================
 * 从缓存移除
 * ============================================================ */
int install_cache_remove(const char *pkg_name, const char *type) {
    if (!pkg_name || !type) return -1;
    if (install_cache_load() != 0) return -1;

    cJSON *type_obj = cJSON_GetObjectItem(g_cache_root, type);
    if (!type_obj) return -1;

    for (int i = 0; i < cJSON_GetArraySize(type_obj); i++) {
        cJSON *item = cJSON_GetArrayItem(type_obj, i);
        if (cJSON_IsString(item) && strcmp(item->valuestring, pkg_name) == 0) {
            cJSON_DeleteItemFromArray(type_obj, i);
            return install_cache_save();
        }
    }
    return -1;
}

/* ============================================================
 * 清空所有缓存
 * ============================================================ */
void install_cache_clear_all(void) {
    if (g_cache_root) {
        cJSON_Delete(g_cache_root);
    }
    g_cache_root = cJSON_CreateObject();
    g_cache_loaded = 1;
    const char *path = get_cache_path();
    if (access(path, F_OK) == 0) {
        unlink(path);
    }
    LOG_DEBUG_T("InstallCache", "ClearAll", "OK", "all cache cleared");
}