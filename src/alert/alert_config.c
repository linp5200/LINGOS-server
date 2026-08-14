/**
 * @file    src/alert/alert_config.c
 * @brief   预警配置文件读写
 * @version LN-B-4.3.0.0
 * @changes 添加 city、mqtt_enabled、sound_enabled 的读写支持
 * @par     核心协议：防御性编程（配置验证）
 */

#include "alert_config.h"
#include "../common/safe_string.h"
#include "../common/data_path.h"
#include "../lib/log_extra.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <sys/stat.h>

#define CONFIG_PATH "/system/config/alert.conf"

static char g_config_path[512] = {0};

static const char* get_config_path(void) {
    if (g_config_path[0] == '\0') {
        const char *root = lingos_data_root();
        safe_snprintf(g_config_path, sizeof(g_config_path), "%s%s", root, CONFIG_PATH);
    }
    return g_config_path;
}

void alert_config_set_defaults(alert_config_t *cfg) {
    if (!cfg) return;
    memset(cfg, 0, sizeof(alert_config_t));

    cfg->base_interval = 3600;          /* 1 小时 */
    cfg->exception_interval = 600;       /* 10 分钟 */
    cfg->realtime_interval = 2;          /* 2 秒 */
    cfg->earthquake_realtime_interval = 1; /* 1 秒 */

    /* R7: 健康/安全默认阈值 */
    cfg->cpu_threshold = 80;
    cfg->memory_threshold = 85;
    cfg->disk_threshold = 90;
    cfg->login_fail_threshold = 5;

    cfg->typhoon_distance_threshold = 500; /* km */
    cfg->typhoon_level_threshold = 4;      /* 强台风 */
    cfg->earthquake_magnitude_threshold = 4.5;
    cfg->rainfall_threshold = 50.0;        /* mm/24h */

    cfg->max_restart_attempts = 3;
    cfg->restart_window_seconds = 60;
    cfg->fallback_to_offline = 1;
    cfg->enable_core_dump = 0;

    /* ====== 新增字段默认值 ====== */
    safe_strncpy(cfg->city, "Beijing", sizeof(cfg->city));
    cfg->mqtt_enabled = 0;
    cfg->sound_enabled = 1;

    cfg->custom_exception_check = NULL;
}

int alert_config_load(alert_config_t *cfg) {
    LOG_DEBUG_T("AlertConfig", "Load", "Enter", "loading config from %s", get_config_path());
    if (!cfg) return -1;

    alert_config_set_defaults(cfg);

    FILE *fp = fopen(get_config_path(), "r");
    if (!fp) {
        LOG_WARN_T("AlertConfig", "Load", "NotFound", "config not found, using defaults");
        return 0;
    }

    char line[256];
    while (fgets(line, sizeof(line), fp)) {
        if (line[0] == '#' || line[0] == '\n') continue;
        char key[64], val[64];
        if (sscanf(line, "%63[^=]=%63s", key, val) == 2) {
            if (strcmp(key, "base_interval") == 0) cfg->base_interval = atoi(val);
            else if (strcmp(key, "exception_interval") == 0) cfg->exception_interval = atoi(val);
            else if (strcmp(key, "realtime_interval") == 0) cfg->realtime_interval = atoi(val);
            else if (strcmp(key, "earthquake_realtime_interval") == 0) cfg->earthquake_realtime_interval = atoi(val);
            else if (strcmp(key, "typhoon_distance_threshold") == 0) cfg->typhoon_distance_threshold = atoi(val);
            else if (strcmp(key, "typhoon_level_threshold") == 0) cfg->typhoon_level_threshold = atoi(val);
            else if (strcmp(key, "earthquake_magnitude_threshold") == 0) cfg->earthquake_magnitude_threshold = atof(val);
            else if (strcmp(key, "rainfall_threshold") == 0) cfg->rainfall_threshold = atof(val);
            else if (strcmp(key, "max_restart_attempts") == 0) cfg->max_restart_attempts = atoi(val);
            else if (strcmp(key, "restart_window_seconds") == 0) cfg->restart_window_seconds = atoi(val);
            else if (strcmp(key, "fallback_to_offline") == 0) cfg->fallback_to_offline = atoi(val);
            else if (strcmp(key, "enable_core_dump") == 0) cfg->enable_core_dump = atoi(val);
            /* ====== 新增字段读取 ====== */
            else if (strcmp(key, "city") == 0) {
                safe_strncpy(cfg->city, val, sizeof(cfg->city));
            } else if (strcmp(key, "mqtt_enabled") == 0) {
                cfg->mqtt_enabled = atoi(val);
            } else if (strcmp(key, "sound_enabled") == 0) {
                cfg->sound_enabled = atoi(val);
            }
        }
    }
    fclose(fp);

    LOG_INFO_T("AlertConfig", "Load", "OK", "config loaded successfully");
    return 0;
}

int alert_config_validate(alert_config_t *cfg) {
    LOG_DEBUG_T("AlertConfig", "Validate", "Enter", "validating config");
    if (!cfg) return -1;

    if (cfg->base_interval < 1) cfg->base_interval = 3600;
    if (cfg->exception_interval < 1) cfg->exception_interval = 600;
    if (cfg->realtime_interval < 1) cfg->realtime_interval = 2;
    if (cfg->earthquake_realtime_interval < 1) cfg->earthquake_realtime_interval = 1;
    if (cfg->typhoon_distance_threshold < 10) cfg->typhoon_distance_threshold = 500;
    if (cfg->typhoon_level_threshold < 1) cfg->typhoon_level_threshold = 4;
    if (cfg->earthquake_magnitude_threshold < 0.5) cfg->earthquake_magnitude_threshold = 4.5;
    if (cfg->rainfall_threshold < 1.0) cfg->rainfall_threshold = 50.0;
    if (cfg->max_restart_attempts < 1) cfg->max_restart_attempts = 3;
    if (cfg->restart_window_seconds < 10) cfg->restart_window_seconds = 60;
    /* 新增字段无需验证范围 */

    LOG_INFO_T("AlertConfig", "Validate", "OK", "config validated");
    return 0;
}

int alert_config_save(const alert_config_t *cfg) {
    LOG_DEBUG_T("AlertConfig", "Save", "Enter", "saving config to %s", get_config_path());
    if (!cfg) return -1;

    /* 确保目录存在 */
    char dir[512];
    const char *root = lingos_data_root();
    safe_snprintf(dir, sizeof(dir), "%s/system/config", root);
    mkdir(dir, 0755);

    FILE *fp = fopen(get_config_path(), "w");
    if (!fp) {
        LOG_ERROR_T("AlertConfig", "Save", "OpenFail", "cannot write config");
        return -1;
    }

    fprintf(fp, "# LING OS Alert Configuration\n");
    fprintf(fp, "# Auto-generated\n\n");
    fprintf(fp, "base_interval = %d\n", cfg->base_interval);
    fprintf(fp, "exception_interval = %d\n", cfg->exception_interval);
    fprintf(fp, "realtime_interval = %d\n", cfg->realtime_interval);
    fprintf(fp, "earthquake_realtime_interval = %d\n", cfg->earthquake_realtime_interval);
    fprintf(fp, "typhoon_distance_threshold = %d\n", cfg->typhoon_distance_threshold);
    fprintf(fp, "typhoon_level_threshold = %d\n", cfg->typhoon_level_threshold);
    fprintf(fp, "earthquake_magnitude_threshold = %.1f\n", cfg->earthquake_magnitude_threshold);
    fprintf(fp, "rainfall_threshold = %.1f\n", cfg->rainfall_threshold);
    fprintf(fp, "max_restart_attempts = %d\n", cfg->max_restart_attempts);
    fprintf(fp, "restart_window_seconds = %d\n", cfg->restart_window_seconds);
    fprintf(fp, "fallback_to_offline = %d\n", cfg->fallback_to_offline);
    fprintf(fp, "enable_core_dump = %d\n", cfg->enable_core_dump);

    /* ====== 新增字段写入 ====== */
    fprintf(fp, "city = %s\n", cfg->city);
    fprintf(fp, "mqtt_enabled = %d\n", cfg->mqtt_enabled);
    fprintf(fp, "sound_enabled = %d\n", cfg->sound_enabled);

    fclose(fp);
    LOG_INFO_T("AlertConfig", "Save", "OK", "config saved");
    return 0;
}