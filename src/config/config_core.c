/**
 * @file    src/config/config_core.c
 * @brief   配置核心实现
 * @version LN-B-5.1.2.6-rc
 * @changes 增强 config_core_mark_configured()，确保 state.json 写入成功；
 *          增加 config_files_exist() 检查，防止覆盖已有配置；
 *          新增 config_core_save_force() 支持 --force。
 */

#include "config_core.h"
#include "../common/safe_string.h"
#include "../common/data_path.h"
#include "../common/lang.h"
#include "../lib/log_extra.h"
#include "../lib/cJSON/cJSON.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <errno.h>
#include <time.h>

/* 全局配置单例 */
static wizard_config_t g_config;
static int g_config_loaded = 0;

/* ============================================================
 * 辅助：文件路径
 * ============================================================ */
static void get_config_path(char *buf, size_t size, const char *filename) {
    const char *root = lingos_data_root();
    safe_snprintf(buf, size, "%s/system/config/%s", root, filename);
}

/* ============================================================
 * 设置默认值
 * ============================================================ */
void config_core_set_defaults(wizard_config_t *cfg) {
    if (!cfg) return;
    memset(cfg, 0, sizeof(wizard_config_t));
    strcpy(cfg->language, "en");
    strcpy(cfg->system_mode, "app");
    strcpy(cfg->ai_backend, "ollama");
    strcpy(cfg->model, "deepseek-v4-pro");
    strcpy(cfg->base_url, "https://api.deepseek.com");
    strcpy(cfg->startup_option, "shell");
    strcpy(cfg->user_name, "Sir");
    strcpy(cfg->ollama_url, "http://127.0.0.1:8080");
    strcpy(cfg->ollama_model, "glm-4.6:cloud");
    cfg->shadow_mode_enabled = 1;
    cfg->auto_allow_high_risk = 0;
    cfg->thinking_enabled = 1;
    cfg->stream_enabled = 1;
    cfg->show_thinking = 1;
    cfg->meta_info_enabled = 1;
    cfg->max_context_tokens = 32768;
    cfg->socket_timeout = 60;
    cfg->auth_timeout = 60;
    strcpy(cfg->log_level, "info");
    cfg->configured_at = 0;
    /* 【批次A】AI 高级配置默认值 */
    cfg->temperature = 0.7;
    cfg->creativity = 0.8;
    cfg->max_agents = 3;
    strcpy(cfg->search_backend, "searxng");
    cfg->search_max_urls = 50;
    cfg->search_rate_limit = 10;
    cfg->personality_file[0] = '\0';
    cfg->assistant_file[0] = '\0';
    strcpy(cfg->thinking_display, "visible");   /* 思考显示默认 visible */
}

/* ============================================================
 * 读取 JSON 文件到 cJSON
 * ============================================================ */
static cJSON* load_json(const char *path) {
    FILE *fp = fopen(path, "r");
    if (!fp) return NULL;
    fseek(fp, 0, SEEK_END);
    long size = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    char *content = malloc(size + 1);
    if (!content) { fclose(fp); return NULL; }
    size_t read_len = fread(content, 1, size, fp);
    content[read_len] = '\0';
    fclose(fp);
    cJSON *root = cJSON_Parse(content);
    free(content);
    return root;
}

static int save_json(const char *path, cJSON *root) {
    char *json_str = cJSON_PrintUnformatted(root);
    if (!json_str) return -1;

    char tmp_path[512];
    safe_snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", path);

    FILE *fp = fopen(tmp_path, "w");
    if (!fp) { free(json_str); return -1; }
    fputs(json_str, fp);
    fclose(fp);
    free(json_str);

    if (rename(tmp_path, path) != 0) {
        unlink(tmp_path);
        return -1;
    }
    return 0;
}

/* ============================================================
 * 配置文件存在性检查（用于防覆盖）
 * ============================================================ */
static int config_files_exist(void) {
    const char *root = lingos_data_root();
    const char *files[] = {
        "ai_config.json", "security.json", "privilege.json",
        "startup.conf", "defense.conf", "health.conf",
        "watchdog.conf", "sandbox.conf", "network.conf",
        "user_profile.json", "state.json"
    };
    for (size_t i = 0; i < sizeof(files)/sizeof(files[0]); i++) {
        char path[512];
        safe_snprintf(path, sizeof(path), "%s/system/config/%s", root, files[i]);
        if (access(path, F_OK) == 0) {
            LOG_DEBUG_T("ConfigCore", "ExistCheck", "Found", "%s exists", files[i]);
            return 1;
        }
    }
    return 0;
}

/* ============================================================
 * 加载配置
 * ============================================================ */
static int load_ai_config(wizard_config_t *cfg) {
    char path[512];
    get_config_path(path, sizeof(path), "ai_config.json");
    cJSON *root = load_json(path);
    if (!root) {
        LOG_WARN_T("ConfigCore", "Load", "NoFile", "ai_config.json not found, using defaults");
        return -1;
    }

    cJSON *item;
    item = cJSON_GetObjectItem(root, "backend");
    if (cJSON_IsString(item)) safe_strncpy(cfg->ai_backend, item->valuestring, sizeof(cfg->ai_backend));
    item = cJSON_GetObjectItem(root, "language");
    if (cJSON_IsString(item)) safe_strncpy(cfg->language, item->valuestring, sizeof(cfg->language));
    item = cJSON_GetObjectItem(root, "thinking_enabled");
    if (cJSON_IsBool(item)) cfg->thinking_enabled = cJSON_IsTrue(item);
    item = cJSON_GetObjectItem(root, "stream_enabled");
    if (cJSON_IsBool(item)) cfg->stream_enabled = cJSON_IsTrue(item);
    item = cJSON_GetObjectItem(root, "show_thinking");
    if (cJSON_IsBool(item)) cfg->show_thinking = cJSON_IsTrue(item);
    item = cJSON_GetObjectItem(root, "meta_info_enabled");
    if (cJSON_IsBool(item)) cfg->meta_info_enabled = cJSON_IsTrue(item);
    item = cJSON_GetObjectItem(root, "max_context_tokens");
    if (cJSON_IsNumber(item)) cfg->max_context_tokens = item->valueint;
    item = cJSON_GetObjectItem(root, "socket_timeout");
    if (cJSON_IsNumber(item)) cfg->socket_timeout = item->valueint;
    item = cJSON_GetObjectItem(root, "auth_timeout");
    if (cJSON_IsNumber(item)) cfg->auth_timeout = item->valueint;
    item = cJSON_GetObjectItem(root, "log_level");
    if (cJSON_IsString(item)) safe_strncpy(cfg->log_level, item->valuestring, sizeof(cfg->log_level));

    /* 【批次A】AI 高级配置读取 */
    item = cJSON_GetObjectItem(root, "temperature");
    if (cJSON_IsNumber(item)) cfg->temperature = item->valuedouble;
    item = cJSON_GetObjectItem(root, "creativity");
    if (cJSON_IsNumber(item)) cfg->creativity = item->valuedouble;
    item = cJSON_GetObjectItem(root, "max_agents");
    if (cJSON_IsNumber(item)) cfg->max_agents = item->valueint;
    item = cJSON_GetObjectItem(root, "search_backend");
    if (cJSON_IsString(item)) safe_strncpy(cfg->search_backend, item->valuestring, sizeof(cfg->search_backend));
    item = cJSON_GetObjectItem(root, "search_max_urls");
    if (cJSON_IsNumber(item)) cfg->search_max_urls = item->valueint;
    item = cJSON_GetObjectItem(root, "search_rate_limit");
    if (cJSON_IsNumber(item)) cfg->search_rate_limit = item->valueint;
    item = cJSON_GetObjectItem(root, "personality_file");
    if (cJSON_IsString(item)) safe_strncpy(cfg->personality_file, item->valuestring, sizeof(cfg->personality_file));
    item = cJSON_GetObjectItem(root, "assistant_file");
    if (cJSON_IsString(item)) safe_strncpy(cfg->assistant_file, item->valuestring, sizeof(cfg->assistant_file));
    item = cJSON_GetObjectItem(root, "thinking_display");
    if (cJSON_IsString(item)) safe_strncpy(cfg->thinking_display, item->valuestring, sizeof(cfg->thinking_display));

    cJSON *ollama = cJSON_GetObjectItem(root, "ollama");
    if (ollama) {
        item = cJSON_GetObjectItem(ollama, "url");
        if (cJSON_IsString(item)) safe_strncpy(cfg->ollama_url, item->valuestring, sizeof(cfg->ollama_url));
        item = cJSON_GetObjectItem(ollama, "model");
        if (cJSON_IsString(item)) safe_strncpy(cfg->ollama_model, item->valuestring, sizeof(cfg->ollama_model));
    }

    cJSON *deepseek = cJSON_GetObjectItem(root, "deepseek");
    if (deepseek) {
        item = cJSON_GetObjectItem(deepseek, "api_key");
        if (cJSON_IsString(item)) safe_strncpy(cfg->api_key, item->valuestring, sizeof(cfg->api_key));
        item = cJSON_GetObjectItem(deepseek, "model");
        if (cJSON_IsString(item)) safe_strncpy(cfg->model, item->valuestring, sizeof(cfg->model));
        item = cJSON_GetObjectItem(deepseek, "base_url");
        if (cJSON_IsString(item)) safe_strncpy(cfg->base_url, item->valuestring, sizeof(cfg->base_url));
    }

    cJSON_Delete(root);
    return 0;
}

static int load_user_profile(wizard_config_t *cfg) {
    char path[512];
    get_config_path(path, sizeof(path), "user_profile.json");
    cJSON *root = load_json(path);
    if (!root) return -1;
    cJSON *item = cJSON_GetObjectItem(root, "user_name");
    if (cJSON_IsString(item)) safe_strncpy(cfg->user_name, item->valuestring, sizeof(cfg->user_name));
    cJSON_Delete(root);
    return 0;
}

static int load_startup_conf(wizard_config_t *cfg) {
    char path[512];
    get_config_path(path, sizeof(path), "startup.conf");
    FILE *fp = fopen(path, "r");
    if (!fp) return -1;
    char line[128];
    while (fgets(line, sizeof(line), fp)) {
        if (line[0] == '#' || line[0] == '\n') continue;
        char key[32], val[32];
        if (sscanf(line, "%31[^=]=%31s", key, val) == 2) {
            if (strcmp(key, "mode") == 0) {
                safe_strncpy(cfg->startup_option, val, sizeof(cfg->startup_option));
            }
        }
    }
    fclose(fp);
    return 0;
}

static int load_security_config(wizard_config_t *cfg) {
    char path[512];
    get_config_path(path, sizeof(path), "security.json");
    cJSON *root = load_json(path);
    if (!root) return -1;
    cJSON *shadow = cJSON_GetObjectItem(root, "shadow_mode");
    if (shadow) {
        cJSON *enabled = cJSON_GetObjectItem(shadow, "enabled");
        if (cJSON_IsBool(enabled)) cfg->shadow_mode_enabled = cJSON_IsTrue(enabled);
    }
    cJSON_Delete(root);
    return 0;
}

static int load_privilege_config(wizard_config_t *cfg) {
    char path[512];
    get_config_path(path, sizeof(path), "privilege.json");
    cJSON *root = load_json(path);
    if (!root) return -1;
    cJSON *item = cJSON_GetObjectItem(root, "auto_allow_high_risk");
    if (cJSON_IsBool(item)) cfg->auto_allow_high_risk = cJSON_IsTrue(item);
    cJSON_Delete(root);
    return 0;
}

int config_core_load(wizard_config_t *cfg) {
    if (!cfg) return -1;
    config_core_set_defaults(cfg);
    load_ai_config(cfg);
    load_user_profile(cfg);
    load_startup_conf(cfg);
    load_security_config(cfg);
    load_privilege_config(cfg);
    /* 【2026-08-22 健康自检修复】从 state.json 恢复 system_configured → configured_at
     * 防止重启后自检误判"配置不完整"（历史 Bug：验证标志与内存字段脱节） */
    {
        char spath[512];
        get_config_path(spath, sizeof(spath), "state.json");
        cJSON *sroot = load_json(spath);
        if (sroot) {
            cJSON *sc = cJSON_GetObjectItem(sroot, "system_configured");
            if (sc && cJSON_IsTrue(sc)) {
                if (cfg->configured_at == 0) cfg->configured_at = time(NULL);
            }
            cJSON_Delete(sroot);
        }
    }
    g_config = *cfg;
    g_config_loaded = 1;
    LOG_INFO_T("ConfigCore", "Load", "OK", "config loaded (backend=%s, language=%s)", cfg->ai_backend, cfg->language);
    return 0;
}

/* ============================================================
 * 保存配置（带防护）
 * ============================================================ */
static int save_ai_config(const wizard_config_t *cfg) {
    char path[512];
    get_config_path(path, sizeof(path), "ai_config.json");
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "backend", cfg->ai_backend);
    cJSON_AddStringToObject(root, "language", cfg->language);
    cJSON_AddBoolToObject(root, "thinking_enabled", cfg->thinking_enabled);
    cJSON_AddBoolToObject(root, "stream_enabled", cfg->stream_enabled);
    cJSON_AddBoolToObject(root, "show_thinking", cfg->show_thinking);
    cJSON_AddBoolToObject(root, "meta_info_enabled", cfg->meta_info_enabled);
    cJSON_AddNumberToObject(root, "max_context_tokens", cfg->max_context_tokens);
    cJSON_AddNumberToObject(root, "socket_timeout", cfg->socket_timeout);
    cJSON_AddNumberToObject(root, "auth_timeout", cfg->auth_timeout);
    cJSON_AddStringToObject(root, "log_level", cfg->log_level);

    /* 【批次A】AI 高级配置写入 */
    cJSON_AddNumberToObject(root, "temperature", cfg->temperature);
    cJSON_AddNumberToObject(root, "creativity", cfg->creativity);
    cJSON_AddNumberToObject(root, "max_agents", cfg->max_agents);
    cJSON_AddStringToObject(root, "search_backend", cfg->search_backend);
    cJSON_AddNumberToObject(root, "search_max_urls", cfg->search_max_urls);
    cJSON_AddNumberToObject(root, "search_rate_limit", cfg->search_rate_limit);
    cJSON_AddStringToObject(root, "personality_file", cfg->personality_file);
    cJSON_AddStringToObject(root, "assistant_file", cfg->assistant_file);
    cJSON_AddStringToObject(root, "thinking_display", cfg->thinking_display);

    cJSON *ollama = cJSON_CreateObject();
    cJSON_AddStringToObject(ollama, "url", cfg->ollama_url);
    cJSON_AddStringToObject(ollama, "model", cfg->ollama_model);
    cJSON_AddItemToObject(root, "ollama", ollama);

    cJSON *deepseek = cJSON_CreateObject();
    cJSON_AddStringToObject(deepseek, "api_key", cfg->api_key);
    cJSON_AddStringToObject(deepseek, "model", cfg->model);
    cJSON_AddStringToObject(deepseek, "base_url", cfg->base_url);
    cJSON_AddItemToObject(root, "deepseek", deepseek);

    int ret = save_json(path, root);
    cJSON_Delete(root);
    return ret;
}

static int save_user_profile(const wizard_config_t *cfg) {
    char path[512];
    get_config_path(path, sizeof(path), "user_profile.json");
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "user_name", cfg->user_name);
    int ret = save_json(path, root);
    cJSON_Delete(root);
    return ret;
}

static int save_startup_conf(const wizard_config_t *cfg) {
    char path[512];
    get_config_path(path, sizeof(path), "startup.conf");
    FILE *fp = fopen(path, "w");
    if (!fp) return -1;
    fprintf(fp, "# LING OS Startup Configuration\n");
    fprintf(fp, "mode = %s\n", cfg->startup_option);
    fclose(fp);
    return 0;
}

static int save_security_config(const wizard_config_t *cfg) {
    char path[512];
    get_config_path(path, sizeof(path), "security.json");
    cJSON *root = cJSON_CreateObject();
    cJSON *shadow = cJSON_CreateObject();
    cJSON_AddBoolToObject(shadow, "enabled", cfg->shadow_mode_enabled);
    cJSON_AddItemToObject(root, "shadow_mode", shadow);
    cJSON *dark = cJSON_CreateObject();
    cJSON_AddBoolToObject(dark, "enabled", 0);
    cJSON_AddItemToObject(root, "dark_mode", dark);
    int ret = save_json(path, root);
    cJSON_Delete(root);
    return ret;
}

static int save_privilege_config(const wizard_config_t *cfg) {
    char path[512];
    get_config_path(path, sizeof(path), "privilege.json");
    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "auto_allow_high_risk", cfg->auto_allow_high_risk);
    int ret = save_json(path, root);
    cJSON_Delete(root);
    return ret;
}

/* ============================================================
 * 常规保存（带防覆盖保护）
 * ============================================================ */
int config_core_save(const wizard_config_t *cfg) {
    if (!cfg) return -1;

    if (config_files_exist()) {
        LOG_INFO_T("ConfigCore", "Save", "Skip", "config files exist, not overwriting");
        return 0;  /* 静默成功，保护已有配置 */
    }

    LOG_INFO_T("ConfigCore", "Save", "Start", "no config files exist, saving defaults");

    int ret = 0;
    ret |= save_ai_config(cfg);
    ret |= save_user_profile(cfg);
    ret |= save_startup_conf(cfg);
    ret |= save_security_config(cfg);
    ret |= save_privilege_config(cfg);

    if (ret == 0) {
        LOG_INFO_T("ConfigCore", "Save", "OK", "config saved");
    } else {
        LOG_ERROR_T("ConfigCore", "Save", "Partial", "some configs failed to save");
    }
    return ret;
}

/* ============================================================
 * 强制保存（无视文件存在性，供 --force 使用）
 * ============================================================ */
int config_core_save_force(const wizard_config_t *cfg) {
    if (!cfg) return -1;

    LOG_INFO_T("ConfigCore", "SaveForce", "Start", "force saving config (overwriting if exists)");

    int ret = 0;
    ret |= save_ai_config(cfg);
    ret |= save_user_profile(cfg);
    ret |= save_startup_conf(cfg);
    ret |= save_security_config(cfg);
    ret |= save_privilege_config(cfg);

    if (ret == 0) {
        LOG_INFO_T("ConfigCore", "SaveForce", "OK", "config saved (forced)");
    } else {
        LOG_ERROR_T("ConfigCore", "SaveForce", "Partial", "some configs failed to save");
    }
    return ret;
}

/* ============================================================
 * 全局 API
 * ============================================================ */
const wizard_config_t* config_core_get(void) {
    if (!g_config_loaded) {
        config_core_load(&g_config);
    }
    return &g_config;
}

wizard_config_t* config_core_get_mutable(void) {
    if (!g_config_loaded) {
        config_core_load(&g_config);
    }
    return &g_config;
}

int config_core_reload(void) {
    wizard_config_t tmp;
    if (config_core_load(&tmp) == 0) {
        g_config = tmp;
        g_config_loaded = 1;
        LOG_INFO_T("ConfigCore", "Reload", "OK", "config reloaded");
        return 0;
    }
    return -1;
}

int config_core_is_configured(void) {
    char path[512];
    get_config_path(path, sizeof(path), "state.json");
    cJSON *root = load_json(path);
    if (!root) return 0;
    cJSON *item = cJSON_GetObjectItem(root, "system_configured");
    int result = (item && cJSON_IsTrue(item)) ? 1 : 0;
    cJSON_Delete(root);
    return result;
}

/* ============================================================
 * 标记为已配置（增强版）
 * ============================================================ */
int config_core_mark_configured(void) {
    char path[512];
    get_config_path(path, sizeof(path), "state.json");

    char dir_path[512];
    safe_snprintf(dir_path, sizeof(dir_path), "%s/system/config", lingos_data_root());
    mkdir(dir_path, 0755);

    cJSON *root = load_json(path);
    if (!root) root = cJSON_CreateObject();

    cJSON_ReplaceItemInObject(root, "system_configured", cJSON_CreateBool(1));
    char time_str[64];
    time_t now = time(NULL);
    struct tm *tm = localtime(&now);
    strftime(time_str, sizeof(time_str), "%Y-%m-%dT%H:%M:%SZ", tm);
    cJSON_ReplaceItemInObject(root, "last_config_time", cJSON_CreateString(time_str));

    int ret = save_json(path, root);
    cJSON_Delete(root);

    /* 【2026-08-22 健康自检修复】同步设置内存 configured_at——
     * 原 Bug：configured_at 仅初始化=0 从未置位，自检(configured_at==0)永远判"配置不完整"
     * 导致"配置完成但验证未更新 → 被误判重新配置"。 */
    if (ret == 0) {
        g_config.configured_at = now;
        g_config_loaded = 1;
        LOG_INFO_T("ConfigCore", "MarkConfigured", "OK", "marked as configured at %s (mem+state synced)", time_str);
    } else {
        LOG_ERROR_T("ConfigCore", "MarkConfigured", "Fail", "failed to save state.json");
    }
    return ret;
}