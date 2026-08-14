/**
 * @file    scan_config.c
 * @brief   扫描配置管理（间隔、启用状态、上次完成时间，自动创建文件）
 * @version 2.1.0.0
 */

#include "scan_config.h"
#include "../common/data_path.h"
#include "log_extra.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

#define CONFIG_FILE "/system/config/scan.conf"
#define STATE_FILE "/Ensystem/scan_state.json"

static int scan_interval = 60;
static int scan_enabled = 1;
static uint64_t last_completed = 0;

static const char *get_config_path(void) {
    static char path[512];
    if (path[0] == '\0') {
        const char *root = lingos_data_root();
        snprintf(path, sizeof(path), "%s%s", root, CONFIG_FILE);
    }
    return path;
}

static const char *get_state_path(void) {
    static char path[512];
    if (path[0] == '\0') {
        const char *root = lingos_data_root();
        snprintf(path, sizeof(path), "%s%s", root, STATE_FILE);
    }
    return path;
}

static void create_default_config(void) {
    const char *path = get_config_path();
    if (access(path, F_OK) == 0) return;
    char dir[512];
    const char *root = lingos_data_root();
    snprintf(dir, sizeof(dir), "%s/system/config", root);
    mkdir(dir, 0755);
    FILE *fp = fopen(path, "w");
    if (!fp) {
        LOG_ERROR_T("ScanConfig", "CreateConfig", "Fail", "cannot create %s", path);
        return;
    }
    fprintf(fp, "60\n");
    fclose(fp);
    LOG_INFO_T("ScanConfig", "CreateConfig", "OK", "created %s", path);
}

static void create_default_state(void) {
    const char *path = get_state_path();
    if (access(path, F_OK) == 0) return;
    char dir[512];
    const char *root = lingos_data_root();
    snprintf(dir, sizeof(dir), "%s/Ensystem", root);
    mkdir(dir, 0755);
    FILE *fp = fopen(path, "w");
    if (!fp) {
        LOG_ERROR_T("ScanConfig", "CreateState", "Fail", "cannot create %s", path);
        return;
    }
    fprintf(fp, "0\n");
    fclose(fp);
    LOG_INFO_T("ScanConfig", "CreateState", "OK", "created %s", path);
}

void scan_config_load(void) {
    create_default_config();
    create_default_state();
    const char *path = get_config_path();
    FILE *fp = fopen(path, "r");
    if (fp) {
        if (fscanf(fp, "%d", &scan_interval) == 1) {
            if (scan_interval < 10) scan_interval = 10;
        }
        fclose(fp);
    }
    fp = fopen(get_state_path(), "r");
    if (fp) {
        fscanf(fp, "%llu", (unsigned long long*)&last_completed);
        fclose(fp);
    }
    LOG_DEBUG_T("ScanConfig", "Load", "OK", "interval=%d, enabled=%d", scan_interval, scan_enabled);
}

void scan_config_save(void) {
    const char *path = get_config_path();
    FILE *fp = fopen(path, "w");
    if (fp) {
        fprintf(fp, "%d\n", scan_interval);
        fclose(fp);
    }
    fp = fopen(get_state_path(), "w");
    if (fp) {
        fprintf(fp, "%llu\n", (unsigned long long)last_completed);
        fclose(fp);
    }
}

int scan_get_interval(void) { return scan_interval; }

int scan_set_interval(int seconds) {
    if (seconds < 10) seconds = 10;
    scan_interval = seconds;
    scan_config_save();
    LOG_INFO_T("ScanConfig", "Set", "OK", "interval=%d", seconds);
    return 0;
}

int scan_is_enabled(void) { return scan_enabled; }

void scan_set_enabled(int enabled) {
    scan_enabled = enabled;
    scan_config_save();
    LOG_INFO_T("ScanConfig", "SetEnabled", "%s", enabled ? "ON" : "OFF");
}

uint64_t scan_get_last_completed(void) { return last_completed; }

void scan_set_last_completed(uint64_t ts) {
    last_completed = ts;
    scan_config_save();
}