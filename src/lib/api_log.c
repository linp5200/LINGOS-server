/**
 * @file    src/lib/api_log.c
 * @brief   API 日志 + 设备修改日志实现（P2-B）
 * @version LN-0.7.0
 */

#include "api_log.h"
#include "../common/data_path.h"
#include "../common/safe_string.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <pthread.h>

/* options_get 弱符号（无 options 的二进制 → 默认开启） */
extern int options_get(const char *key) __attribute__((weak));

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;

static const char *log_dir_path(void) {
    static char p[512];
    if (p[0] == '\0') {
        safe_snprintf(p, sizeof(p), "%s/log", lingos_data_root());
    }
    return p;
}

static void now_str(char *buf, size_t sz) {
    time_t t = time(NULL);
    struct tm tmv;
    localtime_r(&t, &tmv);
    strftime(buf, sz, "%Y-%m-%d %H:%M:%S", &tmv);
}

static void append_line(const char *file, const char *line) {
    char path[600];
    safe_snprintf(path, sizeof(path), "%s/%s", log_dir_path(), file);
    pthread_mutex_lock(&g_lock);
    FILE *fp = fopen(path, "a");
    if (fp) {
        fputs(line, fp);
        fputc('\n', fp);
        fflush(fp);   /* 防关机丢失（先生设定："不被清除"之防关机） */
        fclose(fp);
    }
    pthread_mutex_unlock(&g_lock);
}

void api_log(const char *channel, const char *direction, const char *op,
             int status, long dur_ms, long bytes, const char *summary) {
    /* 开关：dev.api_log（0.x 默认开；显式 0 → 关闭） */
    if (options_get && options_get("dev.api_log") == 0) return;

    char ts[32];
    now_str(ts, sizeof(ts));

    char line[1024];
    safe_snprintf(line, sizeof(line),
        "%s [API] %-7s %-3s %-24s status=%-3d %ldms %ldB %s",
        ts,
        channel ? channel : "-",
        direction ? direction : "-",
        op ? op : "-",
        status, dur_ms, bytes,
        (summary && *summary) ? summary : "");
    append_line("api.log", line);
}

static unsigned int g_req_seq = 0;

void devmod_new_req_id(char *out, unsigned int out_sz) {
    if (!out || out_sz == 0) return;
    unsigned int seq;
    pthread_mutex_lock(&g_lock);
    seq = ++g_req_seq;
    pthread_mutex_unlock(&g_lock);
    safe_snprintf(out, out_sz, "dm%lx%x", (unsigned long)time(NULL), seq & 0xffff);
}

void devmod_log(const char *op, const char *target, const char *req_id,
                int phase, long size, int sensitive, const char *result) {
    char ts[32];
    now_str(ts, sizeof(ts));

    /* 隐私：敏感 → 仅记长度（"摘要或长度"——不记原文） */
    char tgt[420];
    if (sensitive) {
        safe_snprintf(tgt, sizeof(tgt), "(敏感——长度 %ldB)", size);
    } else {
        safe_snprintf(tgt, sizeof(tgt), "%s", (target && *target) ? target : "-");
    }

    char line[1024];
    if (phase == 0) {
        safe_snprintf(line, sizeof(line),
            "%s [DEVMOD] op=%-6s phase=request req=%s target=%s size=%ldB",
            ts, (op && *op) ? op : "MOD", (req_id && *req_id) ? req_id : "-", tgt, size);
    } else {
        safe_snprintf(line, sizeof(line),
            "%s [DEVMOD] op=%-6s phase=done    req=%s result=%s size=%ldB",
            ts, (op && *op) ? op : "MOD", (req_id && *req_id) ? req_id : "-",
            (result && *result) ? result : "ok", size);
    }
    append_line("device_mod.log", line);
}

int log_path_protected(const char *path) {
    if (!path || !*path) return 0;

    /* 规范化前缀匹配：{root}/log 下的一切 + 直接写 log 目录名 */
    char prefix[512];
    safe_snprintf(prefix, sizeof(prefix), "%s/log", lingos_data_root());

    if (strncmp(path, prefix, strlen(prefix)) == 0) return 1;
    if (strstr(path, "/LINGOS/log") == path || strcmp(path, "/LINGOS/log") == 0) return 1;
    /* 相对/间接写法（/LINGOS/log/../log）——保守：包含 "/log/" 且以 .log 结尾时也保护 */
    if (strstr(path, "/LINGOS/log/") != NULL) return 1;

    return 0;
}
