/**
 * @file    security_config.c
 * @brief   安全配置读写实现（security.json）
 * @version LN-0.4.3
 * @changes 修复 self-deadlock：拆分 security_config_set_defaults 为内部/外部版本
 */

#include "security_config.h"
#include "../common/data_path.h"
#include "../common/safe_string.h"
#include "../lib/log_extra.h"
#include "../lib/cJSON/cJSON.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <errno.h>
#include <pthread.h>

#define CONFIG_PATH "/system/config/security.json"

static security_config_t g_config;
static int g_loaded = 0;
static pthread_mutex_t g_config_lock = PTHREAD_MUTEX_INITIALIZER;

static const char* get_config_path(void) {
    static char path[512];
    if (path[0] == '\0') {
        const char *root = lingos_data_root();
        safe_snprintf(path, sizeof(path), "%s%s", root, CONFIG_PATH);
    }
    return path;
}

static void ensure_config_dir(void) {
    const char *root = lingos_data_root();
    char dir[512];
    safe_snprintf(dir, sizeof(dir), "%s/system/config", root);
    if (access(dir, F_OK) != 0) {
        mkdir(dir, 0755);
    }
}

/* ============================================================
 * 内部函数：假定调用者已持有 g_config_lock
 * ============================================================ */
static int security_config_set_defaults_locked(void) {
    LOG_DEBUG_T("SecurityConfig", "SetDefaults", "Enter", "setting defaults (locked)");

    memset(&g_config, 0, sizeof(security_config_t));
    safe_strncpy(g_config.version, "3.0", sizeof(g_config.version));
    safe_strncpy(g_config.input_mode, "balanced", sizeof(g_config.input_mode));

    /* 影子模式（默认启用） */
    g_config.shadow_enabled = 1;
    g_config.shadow_perm_count = 0;
    safe_strncpy(g_config.shadow_permissions[g_config.shadow_perm_count++], "camera", 32);
    safe_strncpy(g_config.shadow_permissions[g_config.shadow_perm_count++], "microphone", 32);
    safe_strncpy(g_config.shadow_permissions[g_config.shadow_perm_count++], "location", 32);
    safe_strncpy(g_config.shadow_permissions[g_config.shadow_perm_count++], "photos", 32);
    safe_strncpy(g_config.shadow_permissions[g_config.shadow_perm_count++], "installed_apps", 32);
    safe_strncpy(g_config.shadow_excluded_apps[0], "lingos_system", 64);
    safe_strncpy(g_config.shadow_excluded_apps[1], "nook_core", 64);
    g_config.shadow_excluded_count = 2;

    /* 暗影模式（默认禁用） */
    g_config.dark_enabled = 0;
    g_config.dark_simulate_hardware_disable = 1;
    g_config.dark_blocked_count = 0;
    safe_strncpy(g_config.dark_blocked_features[g_config.dark_blocked_count++], "package_install", 32);
    safe_strncpy(g_config.dark_blocked_features[g_config.dark_blocked_count++], "package_remove", 32);
    safe_strncpy(g_config.dark_blocked_features[g_config.dark_blocked_count++], "service_restart", 32);
    safe_strncpy(g_config.dark_blocked_features[g_config.dark_blocked_count++], "system_reboot", 32);
    safe_strncpy(g_config.dark_blocked_features[g_config.dark_blocked_count++], "system_update", 32);
    safe_strncpy(g_config.dark_blocked_features[g_config.dark_blocked_count++], "process_kill", 32);

    /* 绝对保护（默认禁用） */
    g_config.absolute_enabled = 0;
    safe_strncpy(g_config.absolute_trigger, "auto", sizeof(g_config.absolute_trigger));
    g_config.absolute_auto_close_interval = 30;
    g_config.absolute_block_all_external_input = 1;
    g_config.absolute_block_infected_internal_input = 1;

    /* 行为监控（默认启用） */
    g_config.behavior_enabled = 1;
    g_config.behavior_window_size = 100;
    g_config.behavior_threshold = 70;
    g_config.behavior_auto_escalate = 1;

    LOG_INFO_T("SecurityConfig", "SetDefaults", "OK", "default config set");
    return 0;
}

/* ============================================================
 * 外部 API：自动加锁（供外部调用）
 * ============================================================ */
int security_config_set_defaults(void) {
    LOG_DEBUG_T("SecurityConfig", "SetDefaults", "Enter", "external call, acquiring lock");
    pthread_mutex_lock(&g_config_lock);
    int ret = security_config_set_defaults_locked();
    pthread_mutex_unlock(&g_config_lock);
    return ret;
}

int security_config_validate(security_config_t *cfg) {
    if (!cfg) return -1;

    LOG_DEBUG_T("SecurityConfig", "Validate", "Enter", "validating config");

    int valid = 1;

    /* 验证 input_mode */
    if (strcmp(cfg->input_mode, "strict") != 0 &&
        strcmp(cfg->input_mode, "balanced") != 0 &&
        strcmp(cfg->input_mode, "permissive") != 0) {
        LOG_WARN_T("SecurityConfig", "Validate", "InvalidInputMode", "using 'balanced'");
        safe_strncpy(cfg->input_mode, "balanced", sizeof(cfg->input_mode));
        valid = 0;
    }

    /* 验证权限列表非空 */
    if (cfg->shadow_perm_count == 0) {
        LOG_WARN_T("SecurityConfig", "Validate", "NoShadowPerms", "shadow permissions empty");
        cfg->shadow_perm_count = 1;
        safe_strncpy(cfg->shadow_permissions[0], "camera", 32);
        valid = 0;
    }

    /* 验证窗口大小 */
    if (cfg->behavior_window_size < 10) cfg->behavior_window_size = 10;
    if (cfg->behavior_threshold < 1) cfg->behavior_threshold = 1;
    if (cfg->behavior_threshold > 100) cfg->behavior_threshold = 100;

    LOG_DEBUG_T("SecurityConfig", "Validate", "OK", "validation %s", valid ? "passed" : "fixed");
    return valid ? 0 : -1;
}

int security_config_load(void) {
    LOG_INFO_T("SecurityConfig", "Load", "Enter", "loading security config");

    LOG_DEBUG_T("SecurityConfig", "Load", "Lock", "acquiring config lock");
    pthread_mutex_lock(&g_config_lock);
    LOG_DEBUG_T("SecurityConfig", "Load", "Lock", "config lock acquired");

    /* 【关键修复】调用内部函数（不加锁） */
    LOG_DEBUG_T("SecurityConfig", "Load", "SetDefaults", "setting defaults (internal)");
    security_config_set_defaults_locked();
    LOG_DEBUG_T("SecurityConfig", "Load", "SetDefaults", "defaults set");

    const char *path = get_config_path();
    LOG_DEBUG_T("SecurityConfig", "Load", "OpenFile", "config path: %s", path);

    FILE *fp = fopen(path, "r");
    if (!fp) {
        LOG_DEBUG_T("SecurityConfig", "Load", "Unlock", "file not found, releasing lock");
        pthread_mutex_unlock(&g_config_lock);
        LOG_WARN_T("SecurityConfig", "Load", "NotFound", "config not found, using defaults");
        g_loaded = 1;
        return 0;
    }
    LOG_DEBUG_T("SecurityConfig", "Load", "OpenFile", "file opened successfully");

    fseek(fp, 0, SEEK_END);
    long len = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    LOG_DEBUG_T("SecurityConfig", "Load", "FileSize", "file size: %ld bytes", len);

    char *buf = malloc(len + 1);
    if (!buf) {
        fclose(fp);
        LOG_DEBUG_T("SecurityConfig", "Load", "Unlock", "malloc failed, releasing lock");
        pthread_mutex_unlock(&g_config_lock);
        LOG_ERROR_T("SecurityConfig", "Load", "MallocFail", "malloc failed");
        return -1;
    }

    LOG_DEBUG_T("SecurityConfig", "Load", "ReadFile", "reading file content");
    size_t read_len = fread(buf, 1, len, fp);
    buf[len] = '\0';
    fclose(fp);
    LOG_DEBUG_T("SecurityConfig", "Load", "ReadFile", "read %zu bytes", read_len);

    LOG_DEBUG_T("SecurityConfig", "Load", "ParseJSON", "parsing JSON");
    cJSON *root = cJSON_Parse(buf);
    free(buf);

    if (!root) {
        LOG_DEBUG_T("SecurityConfig", "Load", "Unlock", "parse failed, releasing lock");
        pthread_mutex_unlock(&g_config_lock);
        LOG_ERROR_T("SecurityConfig", "Load", "ParseFail", "invalid JSON");
        return -1;
    }
    LOG_DEBUG_T("SecurityConfig", "Load", "ParseJSON", "JSON parsed successfully");

    cJSON *item;

    /* version */
    item = cJSON_GetObjectItem(root, "version");
    if (item && cJSON_IsString(item)) {
        safe_strncpy(g_config.version, item->valuestring, sizeof(g_config.version));
    }

    /* input_mode */
    item = cJSON_GetObjectItem(root, "input_mode");
    if (item && cJSON_IsString(item)) {
        safe_strncpy(g_config.input_mode, item->valuestring, sizeof(g_config.input_mode));
    }

    /* shadow_mode */
    cJSON *shadow = cJSON_GetObjectItem(root, "shadow_mode");
    if (shadow) {
        item = cJSON_GetObjectItem(shadow, "enabled");
        if (item && cJSON_IsBool(item)) g_config.shadow_enabled = cJSON_IsTrue(item);

        item = cJSON_GetObjectItem(shadow, "permissions");
        if (item && cJSON_IsArray(item)) {
            g_config.shadow_perm_count = 0;
            int size = cJSON_GetArraySize(item);
            for (int i = 0; i < size && i < 8; i++) {
                cJSON *p = cJSON_GetArrayItem(item, i);
                if (p && cJSON_IsString(p)) {
                    safe_strncpy(g_config.shadow_permissions[g_config.shadow_perm_count++],
                                 p->valuestring, 32);
                }
            }
        }
    }

    /* dark_mode */
    cJSON *dark = cJSON_GetObjectItem(root, "dark_mode");
    if (dark) {
        item = cJSON_GetObjectItem(dark, "enabled");
        if (item && cJSON_IsBool(item)) g_config.dark_enabled = cJSON_IsTrue(item);

        item = cJSON_GetObjectItem(dark, "simulate_hardware_disable");
        if (item && cJSON_IsBool(item)) g_config.dark_simulate_hardware_disable = cJSON_IsTrue(item);

        item = cJSON_GetObjectItem(dark, "blocked_features");
        if (item && cJSON_IsArray(item)) {
            g_config.dark_blocked_count = 0;
            int size = cJSON_GetArraySize(item);
            for (int i = 0; i < size && i < 8; i++) {
                cJSON *p = cJSON_GetArrayItem(item, i);
                if (p && cJSON_IsString(p)) {
                    safe_strncpy(g_config.dark_blocked_features[g_config.dark_blocked_count++],
                                 p->valuestring, 32);
                }
            }
        }
    }

    /* absolute_protect */
    cJSON *abs = cJSON_GetObjectItem(root, "absolute_protect");
    if (abs) {
        item = cJSON_GetObjectItem(abs, "enabled");
        if (item && cJSON_IsBool(item)) g_config.absolute_enabled = cJSON_IsTrue(item);

        item = cJSON_GetObjectItem(abs, "trigger");
        if (item && cJSON_IsString(item)) {
            safe_strncpy(g_config.absolute_trigger, item->valuestring, sizeof(g_config.absolute_trigger));
        }

        item = cJSON_GetObjectItem(abs, "auto_close_check_interval");
        if (item && cJSON_IsNumber(item)) g_config.absolute_auto_close_interval = item->valueint;
    }

    /* behavior_monitoring */
    cJSON *beh = cJSON_GetObjectItem(root, "behavior_monitoring");
    if (beh) {
        item = cJSON_GetObjectItem(beh, "enabled");
        if (item && cJSON_IsBool(item)) g_config.behavior_enabled = cJSON_IsTrue(item);

        item = cJSON_GetObjectItem(beh, "window_size");
        if (item && cJSON_IsNumber(item)) g_config.behavior_window_size = item->valueint;

        item = cJSON_GetObjectItem(beh, "threshold");
        if (item && cJSON_IsNumber(item)) g_config.behavior_threshold = item->valueint;

        item = cJSON_GetObjectItem(beh, "auto_escalate");
        if (item && cJSON_IsBool(item)) g_config.behavior_auto_escalate = cJSON_IsTrue(item);
    }

    cJSON_Delete(root);

    LOG_DEBUG_T("SecurityConfig", "Load", "Validate", "validating config");
    security_config_validate(&g_config);
    LOG_DEBUG_T("SecurityConfig", "Load", "Validate", "validation complete");

    g_loaded = 1;

    LOG_DEBUG_T("SecurityConfig", "Load", "Unlock", "releasing config lock");
    pthread_mutex_unlock(&g_config_lock);

    LOG_INFO_T("SecurityConfig", "Load", "OK", "config loaded: input_mode=%s, shadow=%d, dark=%d, absolute=%d",
               g_config.input_mode, g_config.shadow_enabled,
               g_config.dark_enabled, g_config.absolute_enabled);
    return 0;
}

int security_config_save(void) {
    LOG_INFO_T("SecurityConfig", "Save", "Enter", "saving security config");

    LOG_DEBUG_T("SecurityConfig", "Save", "Lock", "acquiring config lock");
    pthread_mutex_lock(&g_config_lock);

    if (!g_loaded) {
        LOG_DEBUG_T("SecurityConfig", "Save", "Unlock", "not loaded, releasing lock");
        pthread_mutex_unlock(&g_config_lock);
        LOG_ERROR_T("SecurityConfig", "Save", "NotLoaded", "config not loaded");
        return -1;
    }

    ensure_config_dir();

    const char *path = get_config_path();
    LOG_DEBUG_T("SecurityConfig", "Save", "Path", "saving to: %s", path);

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "version", g_config.version);
    cJSON_AddStringToObject(root, "input_mode", g_config.input_mode);

    /* shadow_mode */
    cJSON *shadow = cJSON_CreateObject();
    cJSON_AddBoolToObject(shadow, "enabled", g_config.shadow_enabled);
    cJSON *perms = cJSON_CreateArray();
    for (int i = 0; i < g_config.shadow_perm_count; i++) {
        cJSON_AddItemToArray(perms, cJSON_CreateString(g_config.shadow_permissions[i]));
    }
    cJSON_AddItemToObject(shadow, "permissions", perms);
    cJSON_AddItemToObject(root, "shadow_mode", shadow);

    /* dark_mode */
    cJSON *dark = cJSON_CreateObject();
    cJSON_AddBoolToObject(dark, "enabled", g_config.dark_enabled);
    cJSON_AddBoolToObject(dark, "simulate_hardware_disable", g_config.dark_simulate_hardware_disable);
    cJSON *blocked = cJSON_CreateArray();
    for (int i = 0; i < g_config.dark_blocked_count; i++) {
        cJSON_AddItemToArray(blocked, cJSON_CreateString(g_config.dark_blocked_features[i]));
    }
    cJSON_AddItemToObject(dark, "blocked_features", blocked);
    cJSON_AddItemToObject(root, "dark_mode", dark);

    /* absolute_protect */
    cJSON *abs = cJSON_CreateObject();
    cJSON_AddBoolToObject(abs, "enabled", g_config.absolute_enabled);
    cJSON_AddStringToObject(abs, "trigger", g_config.absolute_trigger);
    cJSON_AddNumberToObject(abs, "auto_close_check_interval", g_config.absolute_auto_close_interval);
    cJSON_AddBoolToObject(abs, "block_all_external_input", g_config.absolute_block_all_external_input);
    cJSON_AddBoolToObject(abs, "block_infected_internal_input", g_config.absolute_block_infected_internal_input);
    cJSON_AddItemToObject(root, "absolute_protect", abs);

    /* behavior_monitoring */
    cJSON *beh = cJSON_CreateObject();
    cJSON_AddBoolToObject(beh, "enabled", g_config.behavior_enabled);
    cJSON_AddNumberToObject(beh, "window_size", g_config.behavior_window_size);
    cJSON_AddNumberToObject(beh, "threshold", g_config.behavior_threshold);
    cJSON_AddBoolToObject(beh, "auto_escalate", g_config.behavior_auto_escalate);
    cJSON_AddItemToObject(root, "behavior_monitoring", beh);

    char *json_str = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);

    if (!json_str) {
        LOG_DEBUG_T("SecurityConfig", "Save", "Unlock", "print failed, releasing lock");
        pthread_mutex_unlock(&g_config_lock);
        LOG_ERROR_T("SecurityConfig", "Save", "PrintFail", "cJSON_PrintUnformatted failed");
        return -1;
    }

    FILE *fp = fopen(path, "w");
    if (!fp) {
        free(json_str);
        LOG_DEBUG_T("SecurityConfig", "Save", "Unlock", "open failed, releasing lock");
        pthread_mutex_unlock(&g_config_lock);
        LOG_ERROR_T("SecurityConfig", "Save", "OpenFail", "cannot write %s", path);
        return -1;
    }

    fprintf(fp, "%s\n", json_str);
    fclose(fp);
    free(json_str);

    LOG_DEBUG_T("SecurityConfig", "Save", "Unlock", "releasing config lock");
    pthread_mutex_unlock(&g_config_lock);

    LOG_INFO_T("SecurityConfig", "Save", "OK", "saved to %s", path);
    return 0;
}

const security_config_t* security_config_get(void) {
    if (!g_loaded) {
        security_config_load();
    }
    return &g_config;
}

/* 供 config_loader 调用的热重载函数 */
int security_config_load_from_json(const void *root) {
    (void)root;
    return security_config_load();
}

void security_config_reload_notify(void) {
    LOG_INFO_T("SecurityConfig", "ReloadNotify", "OK", "security config reloaded");
}

/* 模式切换辅助 */
int security_config_set_input_mode(const char *mode) {
    if (!mode) return -1;
    if (strcmp(mode, "strict") != 0 && strcmp(mode, "balanced") != 0 &&
        strcmp(mode, "permissive") != 0) {
        LOG_ERROR_T("SecurityConfig", "SetInputMode", "Invalid", "mode must be strict/balanced/permissive");
        return -1;
    }
    pthread_mutex_lock(&g_config_lock);
    safe_strncpy(g_config.input_mode, mode, sizeof(g_config.input_mode));
    pthread_mutex_unlock(&g_config_lock);
    security_config_save();
    LOG_INFO_T("SecurityConfig", "SetInputMode", "OK", "input_mode set to %s", mode);
    return 0;
}

int security_config_set_shadow_enabled(int enabled) {
    pthread_mutex_lock(&g_config_lock);
    g_config.shadow_enabled = enabled ? 1 : 0;
    pthread_mutex_unlock(&g_config_lock);
    security_config_save();
    LOG_INFO_T("SecurityConfig", "SetShadow", "OK", "shadow_enabled=%d", enabled);
    return 0;
}

int security_config_set_dark_enabled(int enabled) {
    pthread_mutex_lock(&g_config_lock);
    g_config.dark_enabled = enabled ? 1 : 0;
    pthread_mutex_unlock(&g_config_lock);
    security_config_save();
    LOG_INFO_T("SecurityConfig", "SetDark", "OK", "dark_enabled=%d", enabled);
    return 0;
}

int security_config_set_absolute_enabled(int enabled) {
    pthread_mutex_lock(&g_config_lock);
    g_config.absolute_enabled = enabled ? 1 : 0;
    pthread_mutex_unlock(&g_config_lock);
    security_config_save();
    LOG_INFO_T("SecurityConfig", "SetAbsolute", "OK", "absolute_enabled=%d", enabled);
    return 0;
}