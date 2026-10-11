/**
 * @file    src/registry/function_registry_builtin.c
 * @brief   内置命令注册（统一调用 API 的首批真实功能——无假数据、无占位）
 * @version LN-0.9.0
 * @par     注册命令（全部调用已存在功能）：
 *           api.describe         命令清单 + rely 观测计数
 *           api.rely_status      rely 抑制状态（验证依赖控制是否生效）
 *           config.get           读取当前配置（密钥只回传"是否已设置"）
 *           config.set           按键写配置；rely=false → 只改内存不落盘
 *           debug.set_log_level  设置日志级别；rely=false → 不写回配置
 *           system.version       版本与注册表面板
 * @note    rely 用法示例见 fn_config_set / fn_debug_set_log_level：
 *          基础动作始终执行，依赖业务（落盘/通知）用 API_DEPENDENCY_BEGIN..END 包裹。
 */

#include "function_registry.h"
#include "../config/config_core.h"
#include "../core/version.h"
#include "../core/event_bus.h"   /* 【2026-10-11 第 4 期】事件总线观测/发布 */
#include "../lib/log_extra.h"
#include "../common/safe_string.h"

#include <string.h>
#include <stdlib.h>

/* ============================================================
 * 辅助：配置 → JSON
 * ============================================================ */
static cJSON* config_to_json(const wizard_config_t *c) {
    cJSON *o;
    if (!c) return NULL;
    o = cJSON_CreateObject();
    if (!o) return NULL;
    cJSON_AddStringToObject(o, "language", c->language);
    cJSON_AddStringToObject(o, "system_mode", c->system_mode);
    cJSON_AddStringToObject(o, "ai_backend", c->ai_backend);
    cJSON_AddStringToObject(o, "model", c->model);
    cJSON_AddStringToObject(o, "base_url", c->base_url);
    cJSON_AddBoolToObject(o, "api_key_set", c->api_key[0] ? 1 : 0);   /* 不回传明文密钥 */
    cJSON_AddStringToObject(o, "startup_option", c->startup_option);
    cJSON_AddStringToObject(o, "user_name", c->user_name);
    cJSON_AddStringToObject(o, "ollama_url", c->ollama_url);
    cJSON_AddStringToObject(o, "ollama_model", c->ollama_model);
    cJSON_AddBoolToObject(o, "shadow_mode_enabled", c->shadow_mode_enabled);
    cJSON_AddBoolToObject(o, "auto_allow_high_risk", c->auto_allow_high_risk);
    cJSON_AddBoolToObject(o, "thinking_enabled", c->thinking_enabled);
    cJSON_AddBoolToObject(o, "stream_enabled", c->stream_enabled);
    cJSON_AddBoolToObject(o, "show_thinking", c->show_thinking);
    cJSON_AddBoolToObject(o, "meta_info_enabled", c->meta_info_enabled);
    cJSON_AddNumberToObject(o, "max_context_tokens", c->max_context_tokens);
    cJSON_AddNumberToObject(o, "socket_timeout", c->socket_timeout);
    cJSON_AddNumberToObject(o, "auth_timeout", c->auth_timeout);
    cJSON_AddStringToObject(o, "log_level", c->log_level);
    cJSON_AddNumberToObject(o, "temperature", c->temperature);
    cJSON_AddNumberToObject(o, "creativity", c->creativity);
    cJSON_AddNumberToObject(o, "max_agents", c->max_agents);
    cJSON_AddStringToObject(o, "search_backend", c->search_backend);
    cJSON_AddNumberToObject(o, "search_max_urls", c->search_max_urls);
    cJSON_AddNumberToObject(o, "search_rate_limit", c->search_rate_limit);
    cJSON_AddStringToObject(o, "personality_file", c->personality_file);
    cJSON_AddStringToObject(o, "assistant_file", c->assistant_file);
    cJSON_AddStringToObject(o, "thinking_display", c->thinking_display);
    return o;
}

/* ============================================================
 * 辅助：按键写配置（白名单——未知键/类型不符 → -1001）
 * ============================================================ */
static int key_is(const char *a, const char *b) {
    return (a && b && strcmp(a, b) == 0) ? 1 : 0;
}

#define CFG_SET_STR(field, name) \
    if (key_is(key, name) && cJSON_IsString(v)) { \
        safe_strncpy(c->field, v->valuestring, sizeof(c->field)); return API_ERR_OK; }
#define CFG_SET_BOOL(field, name) \
    if (key_is(key, name) && cJSON_IsBool(v)) { c->field = cJSON_IsTrue(v) ? 1 : 0; return API_ERR_OK; }
#define CFG_SET_INT(field, name) \
    if (key_is(key, name) && cJSON_IsNumber(v)) { c->field = v->valueint; return API_ERR_OK; }
#define CFG_SET_DBL(field, name) \
    if (key_is(key, name) && cJSON_IsNumber(v)) { c->field = v->valuedouble; return API_ERR_OK; }

static int cfg_apply_key(wizard_config_t *c, const char *key, cJSON *v) {
    if (!c || !key || !v) return API_ERR_INVALID_ARG;
    CFG_SET_STR(language, "language");
    CFG_SET_STR(system_mode, "system_mode");
    CFG_SET_STR(ai_backend, "ai_backend");
    CFG_SET_STR(api_key, "api_key");
    CFG_SET_STR(model, "model");
    CFG_SET_STR(base_url, "base_url");
    CFG_SET_STR(startup_option, "startup_option");
    CFG_SET_STR(user_name, "user_name");
    CFG_SET_STR(ollama_url, "ollama_url");
    CFG_SET_STR(ollama_model, "ollama_model");
    CFG_SET_STR(log_level, "log_level");
    CFG_SET_STR(search_backend, "search_backend");
    CFG_SET_STR(personality_file, "personality_file");
    CFG_SET_STR(assistant_file, "assistant_file");
    CFG_SET_STR(thinking_display, "thinking_display");
    CFG_SET_BOOL(shadow_mode_enabled, "shadow_mode_enabled");
    CFG_SET_BOOL(auto_allow_high_risk, "auto_allow_high_risk");
    CFG_SET_BOOL(thinking_enabled, "thinking_enabled");
    CFG_SET_BOOL(stream_enabled, "stream_enabled");
    CFG_SET_BOOL(show_thinking, "show_thinking");
    CFG_SET_BOOL(meta_info_enabled, "meta_info_enabled");
    CFG_SET_INT(max_context_tokens, "max_context_tokens");
    CFG_SET_INT(socket_timeout, "socket_timeout");
    CFG_SET_INT(auth_timeout, "auth_timeout");
    CFG_SET_INT(max_agents, "max_agents");
    CFG_SET_INT(search_max_urls, "search_max_urls");
    CFG_SET_INT(search_rate_limit, "search_rate_limit");
    CFG_SET_DBL(temperature, "temperature");
    CFG_SET_DBL(creativity, "creativity");
    return API_ERR_INVALID_ARG;
}

/* ============================================================
 * api.describe —— 命令清单 + rely 观测
 * ============================================================ */
static int fn_api_describe(cJSON *args, cJSON **result, void *ud) {
    const char *filter = NULL;
    cJSON *f;
    (void)ud;
    if (!result) return API_ERR_INTERNAL;
    f = args ? cJSON_GetObjectItem(args, "command") : NULL;
    if (cJSON_IsString(f)) filter = f->valuestring;

    *result = cJSON_CreateObject();
    if (!*result) return API_ERR_INTERNAL;
    {
        cJSON *cmds = api_describe_commands(filter);
        if (!cmds) cmds = cJSON_CreateArray();
        cJSON_AddNumberToObject(*result, "count", (double)cJSON_GetArraySize(cmds));
        cJSON_AddNumberToObject(*result, "suppressed_calls", (double)api_dependency_suppressed_count());
        cJSON_AddItemToObject(*result, "commands", cmds);
    }
    return API_ERR_OK;
}

/* ============================================================
 * api.rely_status —— 依赖控制状态（验证 rely 是否真的生效）
 * ============================================================ */
static int fn_rely_status(cJSON *args, cJSON **result, void *ud) {
    (void)args;
    (void)ud;
    if (!result) return API_ERR_INTERNAL;
    *result = cJSON_CreateObject();
    if (!*result) return API_ERR_INTERNAL;
    cJSON_AddBoolToObject(*result, "suppressed_now", api_dependency_suppressed() ? 1 : 0);
    cJSON_AddNumberToObject(*result, "suppressed_total", (double)api_dependency_suppressed_count());
    cJSON_AddStringToObject(*result, "mechanism", "thread-local suppress_dependencies + API_CALL_DEPENDENCY/API_DEPENDENCY_BEGIN");
    return API_ERR_OK;
}

/* ============================================================
 * config.get
 * ============================================================ */
static int fn_config_get(cJSON *args, cJSON **result, void *ud) {
    (void)args;
    (void)ud;
    if (!result) return API_ERR_INTERNAL;
    *result = config_to_json(config_core_get());
    return *result ? API_ERR_OK : API_ERR_INTERNAL;
}

/* ============================================================
 * config.set —— rely 语义示例
 *   基础动作：改内存配置（始终执行）
 *   依赖业务：落盘（config.json 真源 + 旧文件族镜像）——rely=false 时跳过
 * ============================================================ */
static int fn_config_set(cJSON *args, cJSON **result, void *ud) {
    wizard_config_t cfg;
    cJSON *key_item;
    cJSON *val_item;
    const char *key;
    int persisted = 0;
    (void)ud;

    if (!args) {
        if (result) *result = cJSON_CreateString("Missing 'args' object");
        return API_ERR_INVALID_ARG;
    }
    key_item = cJSON_GetObjectItem(args, "key_path");
    if (!cJSON_IsString(key_item)) key_item = cJSON_GetObjectItem(args, "key");
    val_item = cJSON_GetObjectItem(args, "value");
    if (!cJSON_IsString(key_item) || !val_item) {
        if (result) *result = cJSON_CreateString("Missing 'key_path' or 'value' parameter");
        return API_ERR_INVALID_ARG;
    }
    key = key_item->valuestring;
    cfg = *config_core_get();          /* 先改副本：失败不污染内存 */

    if (cfg_apply_key(&cfg, key, val_item) != API_ERR_OK) {
        if (result) *result = cJSON_CreateString("Unknown config key or wrong value type");
        return API_ERR_INVALID_ARG;
    }
    *config_core_get_mutable() = cfg;  /* 基础动作：立即生效 */

    /* 依赖业务：落盘（config.json 真源 + 旧文件族镜像）
     * rely=false 时宏直接返回 0 → persisted=0，且记调试日志（先生 §5.3） */
    persisted = API_CALL_DEPENDENCY((config_core_save_force(&cfg) == API_ERR_OK) ? 1 : 0);

    if (result) {
        *result = cJSON_CreateObject();
        if (*result) {
            cJSON_AddBoolToObject(*result, "success", 1);
            cJSON_AddStringToObject(*result, "key", key);
            cJSON_AddBoolToObject(*result, "persisted", persisted);
        }
    }
    return API_ERR_OK;
}

/* ============================================================
 * debug.set_log_level —— rely 语义示例（依赖 = 写回配置，重启沿用）
 * ============================================================ */
static int fn_debug_set_log_level(cJSON *args, cJSON **result, void *ud) {
    cJSON *lv;
    int level;
    int persisted = 0;
    (void)ud;

    if (!args) {
        if (result) *result = cJSON_CreateString("Missing 'args' object");
        return API_ERR_INVALID_ARG;
    }
    lv = cJSON_GetObjectItem(args, "level");
    if (cJSON_IsString(lv)) level = log_level_from_string(lv->valuestring);
    else if (cJSON_IsNumber(lv)) level = lv->valueint;
    else {
        if (result) *result = cJSON_CreateString("Missing 'level' parameter (string or number)");
        return API_ERR_INVALID_ARG;
    }
    if (level < LOG_LEVEL_ERROR || level > LOG_LEVEL_DEBUG) {
        if (result) *result = cJSON_CreateString("'level' out of range (1=error .. 4=debug)");
        return API_ERR_INVALID_ARG;
    }

    log_set_level(level);              /* 基础动作：立即生效 */
    (void)event_publish_fmt(EVT_LOG_LEVEL_CHANGED, "{\"level\":%d,\"name\":\"%s\"}",
                            level, log_level_to_string(level));

    API_DEPENDENCY_BEGIN
        wizard_config_t cfg = *config_core_get();
        safe_strncpy(cfg.log_level, log_level_to_string(level), sizeof(cfg.log_level));
        *config_core_get_mutable() = cfg;
        persisted = (config_core_save_force(&cfg) == API_ERR_OK) ? 1 : 0;
    API_DEPENDENCY_END

    if (result) {
        *result = cJSON_CreateObject();
        if (*result) {
            cJSON_AddNumberToObject(*result, "level", level);
            cJSON_AddStringToObject(*result, "level_name", log_level_to_string(level));
            cJSON_AddBoolToObject(*result, "persisted", persisted);
        }
    }
    return API_ERR_OK;
}

/* ============================================================
 * system.events —— 事件总线观测（先生方案：event_publish/subscribe 可视化）
 * ============================================================ */
static int fn_system_events(cJSON *args, cJSON **result, void *ud) {
    cJSON *limit_item;
    int limit = 10;
    (void)ud;
    if (!result) return API_ERR_INTERNAL;
    limit_item = args ? cJSON_GetObjectItem(args, "limit") : NULL;
    if (cJSON_IsNumber(limit_item) && limit_item->valueint > 0) limit = limit_item->valueint;

    *result = cJSON_CreateObject();
    if (!*result) return API_ERR_INTERNAL;
    {
        cJSON *stats = event_bus_stats();
        cJSON *recent = event_bus_recent(limit);
        if (stats) cJSON_AddItemToObject(*result, "stats", stats);
        cJSON_AddItemToObject(*result, "recent", recent ? recent : cJSON_CreateArray());
    }
    return API_ERR_OK;
}

/* ============================================================
 * system.version
 * ============================================================ */
static int fn_system_version(cJSON *args, cJSON **result, void *ud) {
    (void)args;
    (void)ud;
    if (!result) return API_ERR_INTERNAL;
    *result = cJSON_CreateObject();
    if (!*result) return API_ERR_INTERNAL;
    cJSON_AddStringToObject(*result, "version", version_get());
    cJSON_AddStringToObject(*result, "api_schema", "lingos-unified-api-v1");
    cJSON_AddNumberToObject(*result, "registered_commands", api_function_count());
    return API_ERR_OK;
}

/* ============================================================
 * 注册
 * ============================================================ */
void api_register_builtin_functions(void) {
    (void)api_register_function("api.describe",
        fn_api_describe, NULL,
        "列出已注册命令（可选 command 过滤）",
        "{\"type\":\"object\",\"properties\":{\"command\":{\"type\":\"string\"}}}");

    (void)api_register_function("api.rely_status",
        fn_rely_status, NULL,
        "查看 rely 依赖控制状态（当前/累计抑制次数）",
        "{\"type\":\"object\",\"properties\":{}}");

    (void)api_register_function("config.get",
        fn_config_get, NULL,
        "读取当前配置（密钥只回传是否已设置）",
        "{\"type\":\"object\",\"properties\":{}}");

    (void)api_register_function("config.set",
        fn_config_set, NULL,
        "按键写配置；rely=false 时只改内存不落盘",
        "{\"type\":\"object\",\"properties\":{\"key_path\":{\"type\":\"string\"},\"value\":{}},\"required\":[\"key_path\",\"value\"]}");

    (void)api_register_function("debug.set_log_level",
        fn_debug_set_log_level, NULL,
        "设置日志级别；rely=false 时不写回配置",
        "{\"type\":\"object\",\"properties\":{\"level\":{\"type\":[\"string\",\"integer\"]}},\"required\":[\"level\"]}");

    (void)api_register_function("system.events",
        fn_system_events, NULL,
        "事件总线观测（统计 + 最近事件）",
        "{\"type\":\"object\",\"properties\":{\"limit\":{\"type\":\"integer\"}}}");

    (void)api_register_function("system.version",
        fn_system_version, NULL,
        "版本与注册表面板",
        "{\"type\":\"object\",\"properties\":{}}");

    LOG_INFO_T("ApiRegistry", "Builtin", "OK", "builtin commands registered (%d total)", api_function_count());
}
