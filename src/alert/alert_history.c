/**
 * @file    alert_history.c
 * @brief   预警历史记录存储（JSON + 30 天清理）
 * @version LN-B-4.3.0.0
 * @par     核心协议：防弹编程（写入失败不影响主流程）
 */

#include "alert_history.h"
#include "../common/safe_string.h"
#include "../common/data_path.h"
#include "../lib/log_extra.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>
#include <time.h>
#include <errno.h>

#define HISTORY_DIR "/data/alerts/"
#define MAX_HISTORY_DAYS 30

static char g_history_dir[512] = {0};

static const char* get_history_dir(void) {
    if (g_history_dir[0] == '\0') {
        const char *root = lingos_data_root();
        safe_snprintf(g_history_dir, sizeof(g_history_dir), "%s%s", root, HISTORY_DIR);
        mkdir(g_history_dir, 0755);
    }
    return g_history_dir;
}

/* ============================================================
 * 保存事件
 * ============================================================ */

int alert_history_save(const alert_event_t *event) {
    if (!event) return -1;

    const char *dir = get_history_dir();
    time_t now = time(NULL);
    struct tm *tm = localtime(&now);

    char date_dir[512];
    safe_snprintf(date_dir, sizeof(date_dir), "%s/%04d-%02d-%02d",
                  dir, tm->tm_year + 1900, tm->tm_mon + 1, tm->tm_mday);
    mkdir(date_dir, 0755);

    char file_path[512];
    safe_snprintf(file_path, sizeof(file_path), "%s/%ld_%d.json",
                  date_dir, (long)now, event->type);

    FILE *fp = fopen(file_path, "w");
    if (!fp) {
        LOG_WARN_T("AlertHistory", "Save", "OpenFail", "cannot write %s", file_path);
        return -1;
    }

    fprintf(fp, "{\n");
    fprintf(fp, "  \"type\": %d,\n", event->type);
    fprintf(fp, "  \"level\": %d,\n", event->level);
    fprintf(fp, "  \"source\": \"%s\",\n", event->source);
    fprintf(fp, "  \"location\": \"%s\",\n", event->location);
    fprintf(fp, "  \"description\": \"%s\",\n", event->description);
    fprintf(fp, "  \"latitude\": %.6f,\n", event->latitude);
    fprintf(fp, "  \"longitude\": %.6f,\n", event->longitude);
    fprintf(fp, "  \"distance_km\": %d,\n", event->distance_km);
    fprintf(fp, "  \"timestamp\": %ld,\n", (long)event->timestamp);
    fprintf(fp, "  \"expire_time\": %ld\n", (long)event->expire_time);
    fprintf(fp, "}\n");
    fclose(fp);

    LOG_DEBUG_T("AlertHistory", "Save", "OK", "saved to %s", file_path);
    return 0;
}

/* ============================================================
 * 查询历史
 * ============================================================ */

int alert_history_query(const char *location, const char *type_str, int time_range_hours,
                        alert_event_t *out, int max_count) {
    if (!out || max_count <= 0) return 0;

    const char *dir = get_history_dir();
    DIR *d = opendir(dir);
    if (!d) {
        LOG_DEBUG_T("AlertHistory", "Query", "NoDir", "history dir not found");
        return 0;
    }

    /* 暂存所有匹配事件 */
    alert_event_t temp[64];
    int temp_count = 0;

    struct dirent *entry;
    time_t cutoff = time(NULL) - time_range_hours * 3600;

    while ((entry = readdir(d)) != NULL && temp_count < 64) {
        if (entry->d_name[0] == '.') continue;
        if (!S_ISDIR(entry->d_type)) continue;

        char subdir[512];
        safe_snprintf(subdir, sizeof(subdir), "%s/%s", dir, entry->d_name);

        DIR *sd = opendir(subdir);
        if (!sd) continue;

        struct dirent *file_entry;
        while ((file_entry = readdir(sd)) != NULL && temp_count < 64) {
            if (file_entry->d_name[0] == '.') continue;
            if (!strstr(file_entry->d_name, ".json")) continue;

            char file_path[512];
            safe_snprintf(file_path, sizeof(file_path), "%s/%s", subdir, file_entry->d_name);

            FILE *fp = fopen(file_path, "r");
            if (!fp) continue;

            alert_event_t ev;
            memset(&ev, 0, sizeof(ev));
            char line[256];
            while (fgets(line, sizeof(line), fp)) {
                if (strstr(line, "\"type\"")) {
                    sscanf(line, "  \"type\": %d,", &ev.type);
                } else if (strstr(line, "\"level\"")) {
                    sscanf(line, "  \"level\": %d,", &ev.level);
                } else if (strstr(line, "\"source\"")) {
                    char *p = strchr(line, '"');
                    if (p) {
                        p++;
                        char *q = strchr(p, '"');
                        if (q) {
                            int len = q - p;
                            if (len < (int)sizeof(ev.source)) {
                                memcpy(ev.source, p, len);
                                ev.source[len] = '\0';
                            }
                        }
                    }
                } else if (strstr(line, "\"location\"")) {
                    char *p = strchr(line, '"');
                    if (p) {
                        p++;
                        char *q = strchr(p, '"');
                        if (q) {
                            int len = q - p;
                            if (len < (int)sizeof(ev.location)) {
                                memcpy(ev.location, p, len);
                                ev.location[len] = '\0';
                            }
                        }
                    }
                } else if (strstr(line, "\"description\"")) {
                    char *p = strchr(line, '"');
                    if (p) {
                        p++;
                        char *q = strchr(p, '"');
                        if (q) {
                            int len = q - p;
                            if (len < (int)sizeof(ev.description)) {
                                memcpy(ev.description, p, len);
                                ev.description[len] = '\0';
                            }
                        }
                    }
                } else if (strstr(line, "\"timestamp\"")) {
                    long ts;
                    sscanf(line, "  \"timestamp\": %ld,", &ts);
                    ev.timestamp = ts;
                }
            }
            fclose(fp);

            /* 过滤 */
            if (ev.timestamp < cutoff) continue;
            if (location && location[0] && strstr(ev.location, location) == NULL) continue;
            if (type_str && type_str[0]) {
                /* 简单类型匹配 */
                if (strstr(type_str, "typhoon") && ev.type != 1) continue;
                if (strstr(type_str, "earthquake") && ev.type != 2) continue;
                if (strstr(type_str, "rain") && ev.type != 3) continue;
            }

            temp[temp_count++] = ev;
        }
        closedir(sd);
    }
    closedir(d);

    /* 按时间排序（最新的在前） */
    for (int i = 0; i < temp_count - 1; i++) {
        for (int j = i + 1; j < temp_count; j++) {
            if (temp[i].timestamp < temp[j].timestamp) {
                alert_event_t tmp = temp[i];
                temp[i] = temp[j];
                temp[j] = tmp;
            }
        }
    }

    int count = temp_count < max_count ? temp_count : max_count;
    for (int i = 0; i < count; i++) {
        out[i] = temp[i];
    }

    LOG_DEBUG_T("AlertHistory", "Query", "Result", "returned %d events", count);
    return count;
}

/* ============================================================
 * 清理过期记录
 * ============================================================ */

void alert_history_cleanup(int keep_days) {
    LOG_DEBUG_T("AlertHistory", "Cleanup", "Enter", "keeping %d days", keep_days);
    const char *dir = get_history_dir();
    DIR *d = opendir(dir);
    if (!d) return;

    time_t now = time(NULL);
    time_t cutoff = now - keep_days * 86400;

    struct dirent *entry;
    while ((entry = readdir(d)) != NULL) {
        if (entry->d_name[0] == '.') continue;
        char subdir[512];
        safe_snprintf(subdir, sizeof(subdir), "%s/%s", dir, entry->d_name);
        struct stat st;
        if (stat(subdir, &st) != 0) continue;
        if (!S_ISDIR(st.st_mode)) continue;

        if (st.st_mtime < cutoff) {
            char cmd[1024];
            safe_snprintf(cmd, sizeof(cmd), "rm -rf '%s'", subdir);
            system(cmd);
            LOG_DEBUG_T("AlertHistory", "Cleanup", "Removed", "deleted %s", subdir);
        }
    }
    closedir(d);
    LOG_INFO_T("AlertHistory", "Cleanup", "OK", "cleanup completed");
}
/**
 * @brief 初始化预警历史系统
 */
void alert_history_init(void) {
    /* 创建历史目录（已在 get_history_dir 中隐式创建） */
    const char *dir = get_history_dir();
    LOG_DEBUG_T("AlertHistory", "Init", "OK", "history directory: %s", dir);
    /* 首次启动清理过期记录（保留30天） */
    alert_history_cleanup(30);
}

/* ============================================================
 * 完全清理（进程退出时）
 * ============================================================ */

void alert_history_cleanup_all(void) {
    /* 无须额外清理 */
    LOG_DEBUG_T("AlertHistory", "CleanupAll", "OK", "history system cleaned up");
}