/**
 * @file    src/config/config_core.c
 * @brief   配置核心实现
 * @version LN-0.4.3
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

/* 【2026-10-11 第 3 期·配置收官】统一 schema 版本——与 Python 迁移器
 * src/python/config_migrate.py 的 SCHEMA_VERSION 必须一致（当前 = 1） */
#define CONFIG_SCHEMA_VERSION 1

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

/* 敏感文件权限加固：config.json 可能含 api_key → 0600（与迁移器同规则） */
static void save_json_harden(const char *path) {
    if (path) chmod(path, 0600);
}

/* 取子对象，不存在则创建 */
static cJSON* object_ensure(cJSON *parent, const char *key) {
    cJSON *child;
    if (!parent || !key) return NULL;
    child = cJSON_GetObjectItem(parent, key);
    if (cJSON_IsObject(child)) return child;
    child = cJSON_CreateObject();
    if (!child) return NULL;
    cJSON_DeleteItemFromObject(parent, key);
    cJSON_AddItemToObject(parent, key, child);
    return child;
}

/* 键写入（存在即替换，不存在即新增；避免依赖 cJSON 版本差异） */
static void json_put(cJSON *obj, const char *key, cJSON *item) {
    if (!obj || !key || !item) {
        if (item) cJSON_Delete(item);
        return;
    }
    cJSON_DeleteItemFromObject(obj, key);
    cJSON_AddItemToObject(obj, key, item);
}
static void json_put_str(cJSON *obj, const char *key, const char *val) {
    json_put(obj, key, cJSON_CreateString(val ? val : ""));
}
static void json_put_num(cJSON *obj, const char *key, double val) {
    json_put(obj, key, cJSON_CreateNumber(val));
}
static void json_put_bool(cJSON *obj, const char *key, int val) {
    json_put(obj, key, cJSON_CreateBool(val ? 1 : 0));
}

/* 旧文件族是否存在（方案 C：旧文件缺失时以 config.json 为真源） */
static int legacy_file_exists(const char *filename) {
    char path[512];
    get_config_path(path, sizeof(path), filename);
    return (access(path, F_OK) == 0) ? 1 : 0;
}

/* ============================================================
 * 配置文件存在性检查（用于防覆盖）
 * ============================================================ */
static int config_files_exist(void) {
    const char *root = lingos_data_root();
    const char *files[] = {
        "config.json",   /* 【2026-10-11】统一真源同样纳入防覆盖检查 */
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

/* ============================================================
 * 【2026-10-10 配置统一·方案 C】优先读单一 config.json
 * ------------------------------------------------------------
 * 先生 2026-10-01 拍板（§10.5 Q1）：新建统一 schema = config.json 单文件，
 *   三端全部迁移；兼容铁律 = "有 config.json 用新，无则读旧文件族"。
 * 本函数实现 C 端读取侧的统一入口：
 *   ① ai.provider.active → 权威提供商（先生 Q5：AI 合并 OpenAI，与 App 同 schema）
 *      base_url / model / api_key(_enc) 取自该 active provider（解决配置三源分裂）
 *   ② schema_version 校验；缺失或读失败 → 返回 -1，由调用方回退旧文件族
 * 注：api_key_enc 为值级加密（S5），此处仅回填密文占位不落明文；
 *     解密由上层（Python llm_unified / C envelope）负责。
 * ============================================================ */
static int load_unified_config(wizard_config_t *cfg) {
    char path[512];
    get_config_path(path, sizeof(path), "config.json");
    cJSON *root = load_json(path);
    if (!root) return -1;   /* 无 config.json → 调用方回退旧文件族 */

    cJSON *ver = cJSON_GetObjectItem(root, "schema_version");
    int schema = cJSON_IsNumber(ver) ? ver->valueint : 0;
    LOG_INFO_T("ConfigCore", "LoadUnified", "Enter", "config.json found (schema_version=%d)", schema);

    cJSON *ai = cJSON_GetObjectItem(root, "ai");
    cJSON *ai_cfg = ai ? cJSON_GetObjectItem(ai, "ai_config") : NULL;
    cJSON *prov  = ai ? cJSON_GetObjectItem(ai, "provider") : NULL;

    /* ① 通用 AI 参数（新路径 ai.ai_config.*；旧结构完全一致，直接复用键名） */
    if (cJSON_IsObject(ai_cfg)) {
        cJSON *it;
        it = cJSON_GetObjectItem(ai_cfg, "backend");
        if (cJSON_IsString(it)) safe_strncpy(cfg->ai_backend, it->valuestring, sizeof(cfg->ai_backend));
        it = cJSON_GetObjectItem(ai_cfg, "thinking_enabled");
        if (cJSON_IsBool(it)) cfg->thinking_enabled = cJSON_IsTrue(it);
        it = cJSON_GetObjectItem(ai_cfg, "stream_enabled");
        if (cJSON_IsBool(it)) cfg->stream_enabled = cJSON_IsTrue(it);
        it = cJSON_GetObjectItem(ai_cfg, "show_thinking");
        if (cJSON_IsBool(it)) cfg->show_thinking = cJSON_IsTrue(it);
        it = cJSON_GetObjectItem(ai_cfg, "meta_info_enabled");
        if (cJSON_IsBool(it)) cfg->meta_info_enabled = cJSON_IsTrue(it);
        it = cJSON_GetObjectItem(ai_cfg, "max_context_tokens");
        if (cJSON_IsNumber(it)) cfg->max_context_tokens = it->valueint;
        it = cJSON_GetObjectItem(ai_cfg, "socket_timeout");
        if (cJSON_IsNumber(it)) cfg->socket_timeout = it->valueint;
        it = cJSON_GetObjectItem(ai_cfg, "auth_timeout");
        if (cJSON_IsNumber(it)) cfg->auth_timeout = it->valueint;
        it = cJSON_GetObjectItem(ai_cfg, "log_level");
        if (cJSON_IsString(it)) safe_strncpy(cfg->log_level, it->valuestring, sizeof(cfg->log_level));
        it = cJSON_GetObjectItem(ai_cfg, "temperature");
        if (cJSON_IsNumber(it)) cfg->temperature = it->valuedouble;
        it = cJSON_GetObjectItem(ai_cfg, "creativity");
        if (cJSON_IsNumber(it)) cfg->creativity = it->valuedouble;
        it = cJSON_GetObjectItem(ai_cfg, "max_agents");
        if (cJSON_IsNumber(it)) cfg->max_agents = it->valueint;
        it = cJSON_GetObjectItem(ai_cfg, "search_backend");
        if (cJSON_IsString(it)) safe_strncpy(cfg->search_backend, it->valuestring, sizeof(cfg->search_backend));
        it = cJSON_GetObjectItem(ai_cfg, "search_max_urls");
        if (cJSON_IsNumber(it)) cfg->search_max_urls = it->valueint;
        it = cJSON_GetObjectItem(ai_cfg, "search_rate_limit");
        if (cJSON_IsNumber(it)) cfg->search_rate_limit = it->valueint;
        it = cJSON_GetObjectItem(ai_cfg, "personality_file");
        if (cJSON_IsString(it)) safe_strncpy(cfg->personality_file, it->valuestring, sizeof(cfg->personality_file));
        it = cJSON_GetObjectItem(ai_cfg, "assistant_file");
        if (cJSON_IsString(it)) safe_strncpy(cfg->assistant_file, it->valuestring, sizeof(cfg->assistant_file));
        it = cJSON_GetObjectItem(ai_cfg, "thinking_display");
        if (cJSON_IsString(it)) safe_strncpy(cfg->thinking_display, it->valuestring, sizeof(cfg->thinking_display));
        /* 语言属 UI 区，但旧结构在 ai_config —— 兼容读取 */
        it = cJSON_GetObjectItem(ai_cfg, "language");
        if (cJSON_IsString(it)) safe_strncpy(cfg->language, it->valuestring, sizeof(cfg->language));
    }

    /* ② active provider → 权威 base_url / model / key（解决三源分裂） */
    if (cJSON_IsObject(prov)) {
        cJSON *act = cJSON_GetObjectItem(prov, "active");
        cJSON *list = cJSON_GetObjectItem(prov, "providers");
        cJSON *entry = NULL;
        if (cJSON_IsString(act) && cJSON_IsArray(list)) {
            cJSON *p = NULL;
            cJSON_ArrayForEach(p, list) {
                cJSON *id = cJSON_GetObjectItem(p, "id");
                if (cJSON_IsString(id) && strcmp(id->valuestring, act->valuestring) == 0) { entry = p; break; }
            }
        }
        if (entry) {
            cJSON *it;
            it = cJSON_GetObjectItem(entry, "base_url");
            if (cJSON_IsString(it)) safe_strncpy(cfg->base_url, it->valuestring, sizeof(cfg->base_url));
            it = cJSON_GetObjectItem(entry, "model");
            if (cJSON_IsString(it)) safe_strncpy(cfg->model, it->valuestring, sizeof(cfg->model));
            it = cJSON_GetObjectItem(entry, "api_key");
            if (cJSON_IsString(it) && it->valuestring[0]) {
                safe_strncpy(cfg->api_key, it->valuestring, sizeof(cfg->api_key));
            } else {
                /* 仅密文（值级加密）→ 明文留空，由上层解密注入 */
                it = cJSON_GetObjectItem(entry, "api_key_enc");
                if (cJSON_IsString(it) && it->valuestring[0]) cfg->api_key[0] = '\0';
            }
            /* active 提供商与 backend 不一致 → 以 active 为准（先生 Q5） */
            if (cJSON_IsString(act)) safe_strncpy(cfg->ai_backend, act->valuestring, sizeof(cfg->ai_backend));
            LOG_INFO_T("ConfigCore", "LoadUnified", "Provider", "active='%s' model='%s'",
                       act && cJSON_IsString(act) ? act->valuestring : "?", cfg->model);
        }
    }

    /* ③ UI 区（language 等按先生"指定内容由指定端配置"归 ui） */
    cJSON *ui = cJSON_GetObjectItem(root, "ui");
    if (cJSON_IsObject(ui)) {
        cJSON *it;
        /* 迁移器布局：UI_KEYS_FROM_AI 把 language 从 ai 区挪到 ui.ai_config.language
         * （旧读取侧只看 ui.language / ai.ai_config.language → 迁移后语言会丢，此处补齐） */
        cJSON *uai = cJSON_GetObjectItem(ui, "ai_config");
        if (cJSON_IsObject(uai)) {
            it = cJSON_GetObjectItem(uai, "language");
            if (cJSON_IsString(it)) safe_strncpy(cfg->language, it->valuestring, sizeof(cfg->language));
        }
        it = cJSON_GetObjectItem(ui, "language");   /* ui.language 为权威（C 写入侧落点） */
        if (cJSON_IsString(it)) safe_strncpy(cfg->language, it->valuestring, sizeof(cfg->language));
    }

    /* ④ 其余分区（方案 C 完整真源）：旧文件缺失时以 config.json 为准
     *    ——迁移器把旧文件改名 .bak 后旧读取路径落空（治"配置三源分裂" F2） */
    if (!legacy_file_exists("user_profile.json")) {
        cJSON *up = ui ? cJSON_GetObjectItem(ui, "user_profile") : NULL;
        cJSON *it = up ? cJSON_GetObjectItem(up, "user_name") : NULL;
        if (cJSON_IsString(it)) safe_strncpy(cfg->user_name, it->valuestring, sizeof(cfg->user_name));
    }
    if (!legacy_file_exists("startup.conf")) {
        cJSON *rt = cJSON_GetObjectItem(root, "runtime");
        cJSON *st = rt ? cJSON_GetObjectItem(rt, "startup") : NULL;
        cJSON *it = st ? cJSON_GetObjectItem(st, "mode") : NULL;
        if (cJSON_IsString(it)) {
            safe_strncpy(cfg->startup_option, it->valuestring, sizeof(cfg->startup_option));
        } else if (cJSON_IsString(st)) {   /* 容错：runtime.startup 直接是字符串 */
            safe_strncpy(cfg->startup_option, st->valuestring, sizeof(cfg->startup_option));
        }
    }
    if (!legacy_file_exists("security.json")) {
        cJSON *sec = cJSON_GetObjectItem(root, "security");
        cJSON *s2 = sec ? cJSON_GetObjectItem(sec, "security") : NULL;
        cJSON *shadow = s2 ? cJSON_GetObjectItem(s2, "shadow_mode") : NULL;
        cJSON *it = shadow ? cJSON_GetObjectItem(shadow, "enabled") : NULL;
        if (cJSON_IsBool(it)) cfg->shadow_mode_enabled = cJSON_IsTrue(it);
    }
    if (!legacy_file_exists("privilege.json")) {
        cJSON *sec = cJSON_GetObjectItem(root, "security");
        cJSON *pv = sec ? cJSON_GetObjectItem(sec, "privilege") : NULL;
        cJSON *it = pv ? cJSON_GetObjectItem(pv, "auto_allow_high_risk") : NULL;
        if (cJSON_IsBool(it)) cfg->auto_allow_high_risk = cJSON_IsTrue(it);
    }

    cJSON_Delete(root);
    LOG_INFO_T("ConfigCore", "LoadUnified", "OK", "unified config loaded (backend=%s)", cfg->ai_backend);
    return 0;
}

int config_core_load(wizard_config_t *cfg) {
    if (!cfg) return -1;
    config_core_set_defaults(cfg);
    /* 【2026-10-10 方案 C】优先单一 config.json；不存在则回退旧文件族（兼容铁律） */
    if (load_unified_config(cfg) == 0) {
        load_user_profile(cfg);
        load_startup_conf(cfg);
        load_security_config(cfg);
        load_privilege_config(cfg);
        g_config = *cfg;
        g_config_loaded = 1;
        LOG_INFO_T("ConfigCore", "Load", "UnifiedOK", "config.json (backend=%s, language=%s)",
                   cfg->ai_backend, cfg->language);
        return 0;
    }
    LOG_INFO_T("ConfigCore", "Load", "LegacyFallback", "config.json absent → reading legacy file family");
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
 * 【2026-10-11 第 3 期·配置收官】写入侧统一（方案 C · 单一真源）
 * ------------------------------------------------------------
 * schema 与 Python 迁移器 src/python/config_migrate.py 完全一致：
 *   { schema_version, _note, _updated_at,
 *     ai:       { ai_config:{...}, provider:{ active, providers:[{id,base_url,model,api_key}] } },
 *     security: { security:{shadow_mode:{enabled}}, privilege:{auto_allow_high_risk} },
 *     runtime:  { startup:{mode} },
 *     ui:       { language, user_profile:{user_name} } }
 * 策略 = 读-改-写（不重建文件）：
 *   · C 端拥有的键全量刷新；
 *   · 迁移器并入的其他分区与未知键**原样保留**（先生"功能只增不减"）；
 *   · 旧文件族继续镜像写出（兼容铁律：旧组件/旧版本升级不炸）；
 *   · 落盘即 0600（config.json 可能含 api_key）。
 * ============================================================ */
static int save_unified_config(const wizard_config_t *cfg) {
    char path[512];
    char ts[64];
    time_t now;
    struct tm tmv;
    cJSON *root, *ai, *ai_cfg, *prov, *list, *entry = NULL, *ui, *ui_ai;
    cJSON *sec, *sec_inner, *shadow, *priv, *rt, *startup, *uprof;
    int ret;

    if (!cfg) return -1;
    get_config_path(path, sizeof(path), "config.json");

    /* 读-改-写：保留迁移器并入的其余分区/未知键 */
    root = load_json(path);
    if (root && !cJSON_IsObject(root)) { cJSON_Delete(root); root = NULL; }
    if (!root) root = cJSON_CreateObject();
    if (!root) return -1;

    now = time(NULL);
    {
        struct tm *lt = localtime(&now);
        if (lt) { tmv = *lt; strftime(ts, sizeof(ts), "%Y-%m-%dT%H:%M:%S%z", &tmv); }
        else safe_strncpy(ts, "unknown", sizeof(ts));
    }
    json_put(root, "schema_version", cJSON_CreateNumber(CONFIG_SCHEMA_VERSION));
    json_put_str(root, "_note", "LINGOS 统一配置——全部内容集中此文件；改动后 config reload 生效");
    json_put_str(root, "_updated_at", ts);
    json_put_str(root, "_written_by", "config_core_save (C 写入侧统一)");

    /* ---- ai 区 ---- */
    ai = object_ensure(root, "ai");
    ai_cfg = object_ensure(ai, "ai_config");
    json_put_str(ai_cfg, "backend", cfg->ai_backend);
    json_put_str(ai_cfg, "language", cfg->language);   /* 兼容旧读取路径（权威在 ui 区） */
    json_put_bool(ai_cfg, "thinking_enabled", cfg->thinking_enabled);
    json_put_bool(ai_cfg, "stream_enabled", cfg->stream_enabled);
    json_put_bool(ai_cfg, "show_thinking", cfg->show_thinking);
    json_put_bool(ai_cfg, "meta_info_enabled", cfg->meta_info_enabled);
    json_put_num(ai_cfg, "max_context_tokens", cfg->max_context_tokens);
    json_put_num(ai_cfg, "socket_timeout", cfg->socket_timeout);
    json_put_num(ai_cfg, "auth_timeout", cfg->auth_timeout);
    json_put_str(ai_cfg, "log_level", cfg->log_level);
    json_put_num(ai_cfg, "temperature", cfg->temperature);
    json_put_num(ai_cfg, "creativity", cfg->creativity);
    json_put_num(ai_cfg, "max_agents", cfg->max_agents);
    json_put_str(ai_cfg, "search_backend", cfg->search_backend);
    json_put_num(ai_cfg, "search_max_urls", cfg->search_max_urls);
    json_put_num(ai_cfg, "search_rate_limit", cfg->search_rate_limit);
    json_put_str(ai_cfg, "personality_file", cfg->personality_file);
    json_put_str(ai_cfg, "assistant_file", cfg->assistant_file);
    json_put_str(ai_cfg, "thinking_display", cfg->thinking_display);
    {   /* ollama 子对象：保留旧键（Python 统一层仍读） */
        cJSON *ollama = object_ensure(ai_cfg, "ollama");
        json_put_str(ollama, "url", cfg->ollama_url);
        json_put_str(ollama, "model", cfg->ollama_model);
    }

    /* provider 区：保留既有 providers 列表，仅刷新 active 项（三源分裂的解药） */
    prov = object_ensure(ai, "provider");
    list = cJSON_GetObjectItem(prov, "providers");
    if (!cJSON_IsArray(list)) {
        json_put(prov, "providers", cJSON_CreateArray());
        list = cJSON_GetObjectItem(prov, "providers");
    }
    if (cfg->ai_backend[0] != '\0') json_put_str(prov, "active", cfg->ai_backend);
    if (cJSON_IsArray(list) && cfg->ai_backend[0] != '\0') {
        cJSON *it = NULL;
        cJSON_ArrayForEach(it, list) {
            cJSON *id = cJSON_GetObjectItem(it, "id");
            if (cJSON_IsString(id) && strcmp(id->valuestring, cfg->ai_backend) == 0) {
                entry = it;
                break;
            }
        }
        if (!entry) {
            entry = cJSON_CreateObject();
            if (entry) {
                json_put_str(entry, "id", cfg->ai_backend);
                cJSON_AddItemToArray(list, entry);
            }
        }
        if (entry) {
            json_put_str(entry, "base_url", cfg->base_url);
            json_put_str(entry, "model", cfg->model);
            /* 私密值：仅在配置里有明文时写入；已有 api_key_enc 密文原样保留（S5） */
            if (cfg->api_key[0] != '\0') json_put_str(entry, "api_key", cfg->api_key);
        }
    }

    /* ---- ui 区（先生"指定内容由指定端配置"）---- */
    ui = object_ensure(root, "ui");
    json_put_str(ui, "language", cfg->language);
    ui_ai = cJSON_GetObjectItem(ui, "ai_config");
    if (cJSON_IsObject(ui_ai)) json_put_str(ui_ai, "language", cfg->language);  /* 迁移器布局同步 */
    uprof = object_ensure(ui, "user_profile");
    json_put_str(uprof, "user_name", cfg->user_name);

    /* ---- security 区（键名沿用迁移器分区布局：security.security.*）---- */
    sec = object_ensure(root, "security");
    sec_inner = object_ensure(sec, "security");
    shadow = object_ensure(sec_inner, "shadow_mode");
    json_put_bool(shadow, "enabled", cfg->shadow_mode_enabled);
    priv = object_ensure(sec, "privilege");
    json_put_bool(priv, "auto_allow_high_risk", cfg->auto_allow_high_risk);

    /* ---- runtime 区（startup.conf → runtime.startup.mode）---- */
    rt = object_ensure(root, "runtime");
    startup = object_ensure(rt, "startup");
    json_put_str(startup, "mode", cfg->startup_option);

    ret = save_json(path, root);
    if (ret == 0) save_json_harden(path);
    cJSON_Delete(root);
    if (ret == 0) {
        LOG_INFO_T("ConfigCore", "SaveUnified", "OK", "config.json written (schema_version=%d)", CONFIG_SCHEMA_VERSION);
    } else {
        LOG_ERROR_T("ConfigCore", "SaveUnified", "Fail", "failed to write config.json");
    }
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
    /* 【2026-10-11】写入侧统一：先写单一真源 config.json，再镜像旧文件族（兼容铁律） */
    ret |= save_unified_config(cfg);
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
    /* 【2026-10-11】写入侧统一：config.json 为真源；旧文件族继续镜像写出 */
    ret |= save_unified_config(cfg);
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