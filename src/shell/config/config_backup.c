/**
 * @file    config_backup.c
 * @brief   配置备份与回滚
 * @version LN-B-3.8.0.0
 */

#include "config_wizard_common.h"
#include "../../common/lang.h"
#include "../../common/data_path.h"
#include "../../lib/log_extra.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <dirent.h>
#include <time.h>

#define CONFIG_DIR "/LINGOS/system/config"
#define BACKUP_BASE "/LINGOS/backups/config_"
#define BACKUP_PREFIX "config_"

/* ============================================================
 * 辅助函数
 * ============================================================ */

static int create_directory(const char *path) {
    if (access(path, F_OK) == 0) return 0;
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "mkdir -p '%s'", path);
    return system(cmd);
}

static int tar_directory(const char *src, const char *dst) {
    char cmd[1024];
    snprintf(cmd, sizeof(cmd), "tar -czf '%s' -C '%s' . 2>/dev/null", dst, src);
    return system(cmd);
}

static int untar_file(const char *src, const char *dst) {
    char cmd[1024];
    snprintf(cmd, sizeof(cmd), "tar -xzf '%s' -C '%s' 2>/dev/null", src, dst);
    return system(cmd);
}

static char* get_timestamp(void) {
    time_t now = time(NULL);
    struct tm *tm = localtime(&now);
    char *buf = malloc(32);
    if (!buf) return NULL;
    strftime(buf, 32, "%Y%m%d_%H%M%S", tm);
    return buf;
}

/* ============================================================
 * 公共 API
 * ============================================================ */

int backup_config(void) {
    const char *root = lingos_data_root();
    char *timestamp = get_timestamp();
    if (!timestamp) return -1;

    char backup_dir[512];
    snprintf(backup_dir, sizeof(backup_dir), "%s%s%s", root, BACKUP_PREFIX, timestamp);
    free(timestamp);

    if (create_directory(backup_dir) != 0) {
        LOG_ERROR_T("Backup", "Create", "Fail", "Failed to create backup directory: %s", backup_dir);
        return -1;
    }

    char backup_file[512];
    snprintf(backup_file, sizeof(backup_file), "%s/config_backup.tar.gz", backup_dir);

    char config_path[512];
    snprintf(config_path, sizeof(config_path), "%s/system/config", root);

    if (tar_directory(config_path, backup_file) != 0) {
        LOG_ERROR_T("Backup", "Tar", "Fail", "Failed to tar config directory");
        return -1;
    }

    LOG_INFO_T("Backup", "Complete", "OK", "Config backup created: %s", backup_file);
    return 0;
}

int restore_config(const char *backup_path) {
    if (!backup_path || access(backup_path, F_OK) != 0) {
        LOG_ERROR_T("Restore", "Path", "Fail", "Backup path not found: %s", backup_path ? backup_path : "NULL");
        return -1;
    }

    const char *root = lingos_data_root();
    char config_path[512];
    snprintf(config_path, sizeof(config_path), "%s/system/config", root);

    /* 先删除旧配置 */
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "rm -rf '%s'/* 2>/dev/null", config_path);
    system(cmd);

    char backup_file[512];
    snprintf(backup_file, sizeof(backup_file), "%s/config_backup.tar.gz", backup_path);

    if (untar_file(backup_file, config_path) != 0) {
        LOG_ERROR_T("Restore", "Untar", "Fail", "Failed to untar backup");
        return -1;
    }

    LOG_INFO_T("Restore", "Complete", "OK", "Config restored from: %s", backup_path);
    return 0;
}

char** list_config_backups(void) {
    const char *root = lingos_data_root();
    char backups_dir[512];
    snprintf(backups_dir, sizeof(backups_dir), "%s/backups", root);

    if (access(backups_dir, F_OK) != 0) {
        char **empty = malloc(sizeof(char*));
        if (empty) *empty = NULL;
        return empty;
    }

    DIR *d = opendir(backups_dir);
    if (!d) {
        char **empty = malloc(sizeof(char*));
        if (empty) *empty = NULL;
        return empty;
    }

    /* 收集备份目录 */
    char **list = NULL;
    int count = 0;
    struct dirent *entry;

    while ((entry = readdir(d)) != NULL) {
        if (entry->d_name[0] == '.') continue;
        if (strncmp(entry->d_name, BACKUP_PREFIX, strlen(BACKUP_PREFIX)) != 0) continue;

        char full_path[512];
        snprintf(full_path, sizeof(full_path), "%s/%s", backups_dir, entry->d_name);
        struct stat st;
        if (stat(full_path, &st) != 0 || !S_ISDIR(st.st_mode)) continue;

        char **new_list = realloc(list, (count + 2) * sizeof(char*));
        if (!new_list) break;
        list = new_list;
        list[count] = malloc(strlen(full_path) + 1);
        if (list[count]) {
            strcpy(list[count], full_path);
            count++;
        }
    }
    closedir(d);

    /* 排序（最新的在前） */
    if (count > 1) {
        for (int i = 0; i < count - 1; i++) {
            for (int j = i + 1; j < count; j++) {
                if (strcmp(list[i], list[j]) < 0) {
                    char *tmp = list[i];
                    list[i] = list[j];
                    list[j] = tmp;
                }
            }
        }
    }

    if (list) {
        list[count] = NULL;
    } else {
        list = malloc(sizeof(char*));
        if (list) *list = NULL;
    }

    return list;
}