/**
 * @file    init_cache.h
 * @brief   初始化缓存管理头文件
 * @version LN-B-4.3.0.0
 */

#ifndef COMMON_INIT_CACHE_H
#define COMMON_INIT_CACHE_H

#include <stdint.h>
#include <time.h>

typedef struct {
    int version;
    int python_ok;
    int libcurl_ok;
    int microhttpd_ok;
    int notcurses_ok;
    int sqlite3_ok;
    int mosquitto_ok;
    int configs_ok;
    time_t timestamp;
} init_cache_t;

int init_cache_load(init_cache_t *cache);
int init_cache_save(const init_cache_t *cache);
int init_cache_is_valid(const init_cache_t *cache);
void init_cache_set_defaults(init_cache_t *cache);

#endif /* COMMON_INIT_CACHE_H */