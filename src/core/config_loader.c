/**
 * @file    config_loader.c
 * @brief   配置文件热加载框架（支持注册表和安全配置）
 * @version LN-B-5.0.0.0
 * @changes 新增注册 security.json 和 registry 热重载
 */

#include "config_loader.h"
#include "data_path.h"
#include "log_extra.h"
#include "cJSON.h"
#include "safe_string.h"
#include "../config/config_core.h"
#include "../ai/ai_config.h"
#include "../security/defense_mode.h"
#include "../common/lang.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

typedef struct config_handler {
    const char *path;
    int (*load_func)(const cJSON *root);
    void (*reload_notify)(void);
    struct config_handler *next;
} config_handler_t;

static config_handler_t *handlers = NULL;

void config_register(const char *path, int (*load_func)(const cJSON *), void (*notify)(void)) {
    config_handler_t *h = malloc(sizeof(config_handler_t));
    if (!h) return;
    h->path = path;
    h->load_func = load_func;
    h->reload_notify = notify;
    h->next = handlers;
    handlers = h;
    LOG_DEBUG_T("ConfigLoader", "Register", "OK", "registered %s", path);
}

int config_load_all(void) {
    const char *root = lingos_data_root();
    char full_path[512];
    int ret = 0;
    for (config_handler_t *h = handlers; h; h = h->next) {
        safe_snprintf(full_path, sizeof(full_path), "%s%s", root, h->path);
        if (access(full_path, F_OK) != 0) {
            LOG_DEBUG_T("ConfigLoader", "Load", "NotFound", "skip %s", full_path);
            continue;
        }
        FILE *fp = fopen(full_path, "r");
        if (!fp) {
            LOG_WARN_T("ConfigLoader", "Load", "OpenFail", "%s", full_path);
            continue;
        }
        fseek(fp, 0, SEEK_END);
        long len = ftell(fp);
        fseek(fp, 0, SEEK_SET);
        char *buf = malloc(len + 1);
        if (!buf) { fclose(fp); continue; }
        fread(buf, 1, len, fp);
        buf[len] = '\0';
        fclose(fp);
        cJSON *root_json = cJSON_Parse(buf);
        free(buf);
        if (!root_json) {
            LOG_ERROR_T("ConfigLoader", "Load", "ParseFail", "%s", full_path);
            ret = -1;
            continue;
        }
        if (h->load_func) {
            if (h->load_func(root_json) != 0) ret = -1;
        }
        cJSON_Delete(root_json);
    }
    return ret;
}

int config_reload_all(void) {
    int ret = config_load_all();

    /* 1. 已注册处理器（security.json / registry.json）的重载通知 */
    for (config_handler_t *h = handlers; h; h = h->next) {
        if (h->reload_notify) h->reload_notify();
    }

    /* 2. 核心配置热载（ai_config.json / user_profile / startup.conf /
     *    security.json / privilege.json 全部重读） */
    if (config_core_reload() != 0) {
        LOG_WARN_T("ConfigLoader", "Reload", "CoreFail", "config_core_reload failed");
        ret = -1;
    }

    /* 3. AI 配置缓存刷新（backend / model / api_key / language / stream_style） */
    ai_config_load();

    /* 4. 防御模式重应用（shadow / dark / absolute） */
    defense_mode_apply_current();

    /* 5. 语言重载（tr() 即时切换中英） */
    lang_reload();

    LOG_INFO_T("ConfigLoader", "Reload", "OK", "all configs reloaded (core+ai+defense+lang)");
    return ret;
}

/* 新增：注册 security.json 和 registry 的相关处理器（由外部调用） */
void config_loader_register_security(void) {
    extern int security_config_load_from_json(const cJSON *root);
    extern void security_config_reload_notify(void);
    config_register("/system/config/security.json", security_config_load_from_json, security_config_reload_notify);
    LOG_DEBUG_T("ConfigLoader", "RegisterSecurity", "OK", "registered security.json");
}

void config_loader_register_registry(void) {
    extern int registry_reload_from_json(const cJSON *root);
    extern void registry_reload_notify(void);
    config_register("/registry/core/registry.json", registry_reload_from_json, registry_reload_notify);
    LOG_DEBUG_T("ConfigLoader", "RegisterRegistry", "OK", "registered registry.json");
}