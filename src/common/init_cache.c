/**
 * @file    init_cache.c
 * @brief   初始化缓存管理
 * @version LN-B-4.3.0.0
 * @par     核心协议：防弹编程
 */

#include "init_cache.h"
#include "data_path.h"
#include "safe_string.h"
#include "../lib/log_extra.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>
#include <unistd.h>

#define CACHE_FILE "/Ensystem/init_cache.json"
#define CACHE_VERSION 1
#define CACHE_VALID_HOURS 24

static const char* get_cache_path(void) {
    static char path[512];
    if (path[0] == '\0') {
        const char *root = lingos_data_root();
        safe_snprintf(path, sizeof(path), "%s%s", root, CACHE_FILE);
    }
    return path;
}

int init_cache_load(init_cache_t *cache) {
    LOG_DEBUG_T("InitCache", "Load", "Enter", "loading cache");

    if (!cache) return -1;
    memset(cache, 0, sizeof(init_cache_t));

    const char *path = get_cache_path();
    FILE *fp = fopen(path, "r");
    if (!fp) {
        LOG_DEBUG_T("InitCache", "Load", "NotFound", "cache file not found");
        return -1;
    }

    fseek(fp, 0, SEEK_END);
    long size = ftell(fp);
    fseek(fp, 0, SEEK_SET);

    char *buf = malloc(size + 1);
    if (!buf) {
        fclose(fp);
        return -1;
    }
    fread(buf, 1, size, fp);
    buf[size] = '\0';
    fclose(fp);

    /* 简化解析：手动提取字段 */
    char *p;

    if ((p = strstr(buf, "\"version\""))) {
        sscanf(p, "\"version\": %d,", &cache->version);
    }
    if ((p = strstr(buf, "\"python_ok\""))) {
        sscanf(p, "\"python_ok\": %d,", &cache->python_ok);
    }
    if ((p = strstr(buf, "\"libcurl_ok\""))) {
        sscanf(p, "\"libcurl_ok\": %d,", &cache->libcurl_ok);
    }
    if ((p = strstr(buf, "\"microhttpd_ok\""))) {
        sscanf(p, "\"microhttpd_ok\": %d,", &cache->microhttpd_ok);
    }
    if ((p = strstr(buf, "\"notcurses_ok\""))) {
        sscanf(p, "\"notcurses_ok\": %d,", &cache->notcurses_ok);
    }
    if ((p = strstr(buf, "\"sqlite3_ok\""))) {
        sscanf(p, "\"sqlite3_ok\": %d,", &cache->sqlite3_ok);
    }
    if ((p = strstr(buf, "\"mosquitto_ok\""))) {
        sscanf(p, "\"mosquitto_ok\": %d,", &cache->mosquitto_ok);
    }
    if ((p = strstr(buf, "\"configs_ok\""))) {
        sscanf(p, "\"configs_ok\": %d,", &cache->configs_ok);
    }
    if ((p = strstr(buf, "\"timestamp\""))) {
        long ts;
        sscanf(p, "\"timestamp\": %ld,", &ts);
        cache->timestamp = (time_t)ts;
    }

    free(buf);
    LOG_DEBUG_T("InitCache", "Load", "OK", "cache loaded, version=%d", cache->version);
    return 0;
}

int init_cache_save(const init_cache_t *cache) {
    LOG_DEBUG_T("InitCache", "Save", "Enter", "saving cache");

    if (!cache) return -1;

    const char *path = get_cache_path();
    char dir[512];
    const char *root = lingos_data_root();
    safe_snprintf(dir, sizeof(dir), "%s/Ensystem", root);
    mkdir(dir, 0755);

    FILE *fp = fopen(path, "w");
    if (!fp) {
        LOG_ERROR_T("InitCache", "Save", "OpenFail", "cannot write %s", path);
        return -1;
    }

    fprintf(fp, "{\n");
    fprintf(fp, "  \"version\": %d,\n", CACHE_VERSION);
    fprintf(fp, "  \"python_ok\": %d,\n", cache->python_ok);
    fprintf(fp, "  \"libcurl_ok\": %d,\n", cache->libcurl_ok);
    fprintf(fp, "  \"microhttpd_ok\": %d,\n", cache->microhttpd_ok);
    fprintf(fp, "  \"notcurses_ok\": %d,\n", cache->notcurses_ok);
    fprintf(fp, "  \"sqlite3_ok\": %d,\n", cache->sqlite3_ok);
    fprintf(fp, "  \"mosquitto_ok\": %d,\n", cache->mosquitto_ok);
    fprintf(fp, "  \"configs_ok\": %d,\n", cache->configs_ok);
    fprintf(fp, "  \"timestamp\": %ld\n", (long)cache->timestamp);
    fprintf(fp, "}\n");
    fclose(fp);

    LOG_INFO_T("InitCache", "Save", "OK", "cache saved");
    return 0;
}

int init_cache_is_valid(const init_cache_t *cache) {
    if (!cache) return 0;
    if (cache->version != CACHE_VERSION) return 0;
    if (cache->timestamp == 0) return 0;

    time_t now = time(NULL);
    time_t age = now - cache->timestamp;
    int valid = (age < CACHE_VALID_HOURS * 3600);
    LOG_DEBUG_T("InitCache", "IsValid", "Check", "age=%ld hours, valid=%d", age / 3600, valid);
    return valid;
}

void init_cache_set_defaults(init_cache_t *cache) {
    if (!cache) return;
    memset(cache, 0, sizeof(init_cache_t));
    cache->version = CACHE_VERSION;
    cache->timestamp = time(NULL);
    cache->python_ok = 0;
    cache->libcurl_ok = 0;
    cache->microhttpd_ok = 0;
    cache->notcurses_ok = 0;
    cache->sqlite3_ok = 0;
    cache->mosquitto_ok = 0;
    cache->configs_ok = 0;
}