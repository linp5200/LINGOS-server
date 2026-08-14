/**
 * @file    health_trend.c
 * @brief   系统健康趋势分析实现
 * @version 2.0.0.0
 */

#include "health_trend.h"
#include "../common/data_path.h"
#include "log_extra.h"
#include "../common/lang.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <errno.h>
#include <dirent.h>
#include <time.h>

#define TREND_DIR "/data/health_trend"
#define TREND_FILE_PREFIX "health_"

static const char* get_trend_dir(void) {
    static char path[512] = {0};
    if (path[0] == '\0') {
        const char *root = lingos_data_root();
        snprintf(path, sizeof(path), "%s%s", root, TREND_DIR);
    }
    return path;
}

static int ensure_dir(void) {
    const char *dir = get_trend_dir();
    if (access(dir, F_OK) != 0) {
        if (mkdir(dir, 0755) != 0) {
            LOG_ERROR_T("HealthTrend", "Mkdir", "Fail", "cannot create %s: %s", dir, strerror(errno));
            return -1;
        }
    }
    return 0;
}

int health_trend_init(void) {
    if (ensure_dir() != 0) return -1;
    health_trend_cleanup();
    LOG_INFO_T("HealthTrend", "Init", "OK", "trend system ready");
    return 0;
}

static int get_today_filename(char *filename, size_t size) {
    time_t now = time(NULL);
    struct tm *tm = localtime(&now);
    if (!tm) return -1;
    snprintf(filename, size, "%s/%s%04d%02d%02d.json",
             get_trend_dir(), TREND_FILE_PREFIX,
             tm->tm_year + 1900, tm->tm_mon + 1, tm->tm_mday);
    return 0;
}

int health_trend_record(const health_record_t *record) {
    if (!record) return -1;
    if (ensure_dir() != 0) return -1;
    char filename[512];
    if (get_today_filename(filename, sizeof(filename)) != 0) return -1;
    FILE *fp = fopen(filename, "a");
    if (!fp) {
        LOG_ERROR_T("HealthTrend", "Record", "OpenFail", "cannot open %s", filename);
        return -1;
    }
    fprintf(fp, "{\"ts\":%lld,\"mem\":%d,\"disk\":%d,\"load\":%.2f,\"python\":%d,\"ai\":%d,\"net\":%d}\n",
            (long long)record->timestamp, record->mem_usage, record->disk_usage,
            record->load_avg, record->python_ok, record->ai_backend_ok, record->net_ok);
    fclose(fp);
    LOG_DEBUG_T("HealthTrend", "Record", "OK", "saved to %s", filename);
    return 0;
}

int health_trend_get_history(health_record_t *out, int max_days) {
    if (!out || max_days <= 0) return -1;
    if (max_days > HEALTH_HISTORY_MAX_DAYS) max_days = HEALTH_HISTORY_MAX_DAYS;
    const char *dir = get_trend_dir();
    DIR *d = opendir(dir);
    if (!d) return 0;
    struct dirent *ent;
    char files[HEALTH_HISTORY_MAX_DAYS][512];
    int file_count = 0;
    while ((ent = readdir(d)) != NULL && file_count < max_days) {
        if (strncmp(ent->d_name, TREND_FILE_PREFIX, strlen(TREND_FILE_PREFIX)) != 0) continue;
        snprintf(files[file_count], sizeof(files[0]), "%s/%s", dir, ent->d_name);
        file_count++;
    }
    closedir(d);
    for (int i = 0; i < file_count - 1; i++) {
        for (int j = i+1; j < file_count; j++) {
            if (strcmp(files[i], files[j]) < 0) {
                char tmp[512];
                strcpy(tmp, files[i]);
                strcpy(files[i], files[j]);
                strcpy(files[j], tmp);
            }
        }
    }
    int out_idx = 0;
    for (int i = 0; i < file_count && out_idx < max_days; i++) {
        FILE *fp = fopen(files[i], "r");
        if (!fp) continue;
        char line[512];
        if (fgets(line, sizeof(line), fp)) {
            health_record_t rec;
            memset(&rec, 0, sizeof(rec));
            long long ts = 0;
            int mem = 0, disk = 0, python = 0, ai = 0, net = 0;
            double load = 0.0;
            if (sscanf(line, "{\"ts\":%lld,\"mem\":%d,\"disk\":%d,\"load\":%lf,\"python\":%d,\"ai\":%d,\"net\":%d",
                       &ts, &mem, &disk, &load, &python, &ai, &net) >= 5) {
                rec.timestamp = (time_t)ts;
                rec.mem_usage = mem;
                rec.disk_usage = disk;
                rec.load_avg = load;
                rec.python_ok = python;
                rec.ai_backend_ok = ai;
                rec.net_ok = net;
                out[out_idx++] = rec;
            }
        }
        fclose(fp);
    }
    return out_idx;
}

int health_trend_analyze(char *output, size_t output_len) {
    if (!output || output_len == 0) return -1;
    health_record_t records[HEALTH_HISTORY_MAX_DAYS];
    int count = health_trend_get_history(records, HEALTH_HISTORY_MAX_DAYS);
    if (count <= 0) {
        snprintf(output, output_len, "%s", tr("No historical health data available.\n", "没有可用的历史健康数据。\n"));
        return 0;
    }
    double mem_avg = 0, disk_avg = 0;
    int mem_trend = 0, disk_trend = 0;
    for (int i = 0; i < count; i++) {
        mem_avg += records[i].mem_usage;
        disk_avg += records[i].disk_usage;
        if (i > 0) {
            if (records[i].mem_usage > records[i-1].mem_usage) mem_trend++;
            else if (records[i].mem_usage < records[i-1].mem_usage) mem_trend--;
            if (records[i].disk_usage > records[i-1].disk_usage) disk_trend++;
            else if (records[i].disk_usage < records[i-1].disk_usage) disk_trend--;
        }
    }
    mem_avg /= count;
    disk_avg /= count;
    int total = 0;
    total += snprintf(output + total, output_len - total,
                      tr("Health trend analysis (last %d days):\n", "健康趋势分析（最近 %d 天）：\n"), count);
    total += snprintf(output + total, output_len - total,
                      tr("  Average memory usage: %.1f%%\n", "  平均内存使用率: %.1f%%\n"), mem_avg);
    total += snprintf(output + total, output_len - total,
                      tr("  Average disk usage: %.1f%%\n", "  平均磁盘使用率: %.1f%%\n"), disk_avg);
    if (mem_trend > 0)
        total += snprintf(output + total, output_len - total,
                          tr("  Memory usage is increasing (trend score %d)\n", "  内存使用率呈上升趋势（趋势分 %d）\n"), mem_trend);
    else if (mem_trend < 0)
        total += snprintf(output + total, output_len - total,
                          tr("  Memory usage is decreasing (trend score %d)\n", "  内存使用率呈下降趋势（趋势分 %d）\n"), -mem_trend);
    if (disk_trend > 0)
        total += snprintf(output + total, output_len - total,
                          tr("  Disk usage is increasing (trend score %d)\n", "  磁盘使用率呈上升趋势（趋势分 %d）\n"), disk_trend);
    return 0;
}

int health_trend_cleanup(void) {
    const char *dir = get_trend_dir();
    DIR *d = opendir(dir);
    if (!d) return 0;
    struct dirent *ent;
    char files[HEALTH_HISTORY_MAX_DAYS * 2][512];
    int file_count = 0;
    while ((ent = readdir(d)) != NULL) {
        if (strncmp(ent->d_name, TREND_FILE_PREFIX, strlen(TREND_FILE_PREFIX)) != 0) continue;
        snprintf(files[file_count], sizeof(files[0]), "%s/%s", dir, ent->d_name);
        file_count++;
    }
    closedir(d);
    if (file_count <= HEALTH_HISTORY_MAX_DAYS) return 0;
    for (int i = 0; i < file_count - 1; i++) {
        for (int j = i+1; j < file_count; j++) {
            if (strcmp(files[i], files[j]) > 0) {
                char tmp[512];
                strcpy(tmp, files[i]);
                strcpy(files[i], files[j]);
                strcpy(files[j], tmp);
            }
        }
    }
    int to_delete = file_count - HEALTH_HISTORY_MAX_DAYS;
    for (int i = 0; i < to_delete; i++) {
        unlink(files[i]);
    }
    return 0;
}