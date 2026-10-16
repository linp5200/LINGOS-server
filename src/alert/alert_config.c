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
            /* 【第9期·静默失败修复】R7 健康/安全阈值：原先只写不读（save 有 load 无）
             *   → 用户改过的 CPU/内存/磁盘/登录失败阈值在每次重启后静默回退默认值。 */
            else if (strcmp(key, "cpu_threshold") == 0) cfg->cpu_threshold = atoi(val);
            else if (strcmp(key, "memory_threshold") == 0) cfg->memory_threshold = atoi(val);
            else if (strcmp(key, "disk_threshold") == 0) cfg->disk_threshold = atoi(val);
            else if (strcmp(key, "login_fail_threshold") == 0) cfg->login_fail_threshold = atoi(val);
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

    /* 【第9期·生命线】R7 阈值范围校验
     *   原实现写"新增字段无需验证范围"——而 check_local_health 用 (threshold > 0) 做开关，
     *   配置文件里一个 0/-1 就能让 CPU/内存/磁盘预警**静默关闭**（用户毫无提示）。
     *   现：越界一律夹回默认值并打 WARN（预警能力不可被配置静默阉割）。 */
    if (cfg->cpu_threshold < 10 || cfg->cpu_threshold > 100) {
        LOG_WARN_T("AlertConfig", "Validate", "CpuThreshold",
                   "cpu_threshold=%d out of range [10,100] — restored default 80 (health alert was about to be disabled)",
                   cfg->cpu_threshold);
        cfg->cpu_threshold = 80;
    }
    if (cfg->memory_threshold < 10 || cfg->memory_threshold > 100) {
        LOG_WARN_T("AlertConfig", "Validate", "MemoryThreshold",
                   "memory_threshold=%d out of range [10,100] — restored default 85", cfg->memory_threshold);
        cfg->memory_threshold = 85;
    }
    if (cfg->disk_threshold < 10 || cfg->disk_threshold > 100) {
        LOG_WARN_T("AlertConfig", "Validate", "DiskThreshold",
                   "disk_threshold=%d out of range [10,100] — restored default 90", cfg->disk_threshold);
        cfg->disk_threshold = 90;
    }
    if (cfg->login_fail_threshold < 1) {
        LOG_WARN_T("AlertConfig", "Validate", "LoginFailThreshold",
                   "login_fail_threshold=%d invalid — restored default 5", cfg->login_fail_threshold);
        cfg->login_fail_threshold = 5;
    }
    /* 【第9期】生命线通道开关留痕：用户可关声音/MQTT，但排障时必须能区分"没关"与"被关了" */
    LOG_INFO_T("AlertConfig", "Validate", "Channels", "sound_enabled=%d mqtt_enabled=%d city=%s",
               cfg->sound_enabled, cfg->mqtt_enabled, cfg->city);

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

    /* 【第9期】R7 健康/安全阈值写盘（与 load 对称——消除"改了不生效/重启回退"） */
    fprintf(fp, "cpu_threshold = %d\n", cfg->cpu_threshold);
    fprintf(fp, "memory_threshold = %d\n", cfg->memory_threshold);
    fprintf(fp, "disk_threshold = %d\n", cfg->disk_threshold);
    fprintf(fp, "login_fail_threshold = %d\n", cfg->login_fail_threshold);

    fclose(fp);
    LOG_INFO_T("AlertConfig", "Save", "OK", "config saved");
    return 0;
}