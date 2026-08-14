/**
 * @file    plugin.c
 * @brief   C 插件管理器核心实现
 * @version LN-B-4.2.0.0
 */

#include "plugin.h"
#include "plugin_loader.h"
#include "../../common/data_path.h"
#include "../../common/safe_string.h"
#include "../../lib/log_extra.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>

#define MAX_PLUGINS 64

/* ============================================================
 * 全局状态
 * ============================================================ */

static plugin_t *g_plugins = NULL;
static int g_plugin_count = 0;
static int g_initialized = 0;
static pthread_mutex_t g_plugin_lock = PTHREAD_MUTEX_INITIALIZER;

/* ============================================================
 * 内部辅助
 * ============================================================ */

static const char* get_plugin_dir(void) {
    static char path[512];
    if (path[0] == '\0') {
        const char *root = lingos_data_root();
        safe_snprintf(path, sizeof(path), "%s/plugins/c", root);
    }
    return path;
}

/* ============================================================
 * 公共 API 实现
 * ============================================================ */

int plugin_system_init(void) {
    LOG_INFO_T("Plugin", "Init", "Enter", "Initializing plugin system");

    if (g_initialized) {
        LOG_WARN_T("Plugin", "Init", "Already", "plugin system already initialized");
        return 0;
    }

    pthread_mutex_lock(&g_plugin_lock);

    g_plugins = NULL;
    g_plugin_count = 0;
    g_initialized = 1;

    pthread_mutex_unlock(&g_plugin_lock);

    /* 扫描并加载插件 */
    const char *dir = get_plugin_dir();
    LOG_INFO_T("Plugin", "Init", "Scan", "scanning %s for plugins", dir);

    int loaded = plugin_loader_scan(dir);
    LOG_INFO_T("Plugin", "Init", "Done", "loaded %d plugins, total=%d", loaded, g_plugin_count);

    return 0;
}

void plugin_system_shutdown(void) {
    LOG_INFO_T("Plugin", "Shutdown", "Enter", "Shutting down plugin system");

    if (!g_initialized) {
        LOG_WARN_T("Plugin", "Shutdown", "NotInit", "plugin system not initialized");
        return;
    }

    pthread_mutex_lock(&g_plugin_lock);

    plugin_t *p = g_plugins;
    while (p) {
        plugin_t *next = p->next;
        if (p->state == PLUGIN_STATE_ACTIVE) {
            if (p->stop) p->stop(p);
            if (p->shutdown) p->shutdown(p);
        }
        LOG_DEBUG_T("Plugin", "Shutdown", "Unload", "unloading %s", p->name);
        plugin_loader_unload(p);
        free(p);
        p = next;
    }

    g_plugins = NULL;
    g_plugin_count = 0;
    g_initialized = 0;

    pthread_mutex_unlock(&g_plugin_lock);

    LOG_INFO_T("Plugin", "Shutdown", "OK", "plugin system shut down");
}

int plugin_register(plugin_t *plugin) {
    LOG_INFO_T("Plugin", "Register", "Enter", "name='%s', version='%s'",
               plugin ? plugin->name : "(null)", plugin ? plugin->version : "(null)");

    if (!plugin || !plugin->name[0]) {
        LOG_ERROR_T("Plugin", "Register", "Invalid", "plugin or name is NULL");
        return -1;
    }

    if (!g_initialized) {
        LOG_ERROR_T("Plugin", "Register", "NotInit", "plugin system not initialized");
        return -1;
    }

    pthread_mutex_lock(&g_plugin_lock);

    /* 检查是否已存在同名插件 */
    plugin_t *existing = g_plugins;
    while (existing) {
        if (strcmp(existing->name, plugin->name) == 0) {
            pthread_mutex_unlock(&g_plugin_lock);
            LOG_WARN_T("Plugin", "Register", "Duplicate", "plugin '%s' already registered", plugin->name);
            return -1;
        }
        existing = existing->next;
    }

    if (g_plugin_count >= MAX_PLUGINS) {
        pthread_mutex_unlock(&g_plugin_lock);
        LOG_ERROR_T("Plugin", "Register", "Overflow", "max plugins reached (%d)", MAX_PLUGINS);
        return -1;
    }

    /* 复制插件结构 */
    plugin_t *new_plugin = malloc(sizeof(plugin_t));
    if (!new_plugin) {
        pthread_mutex_unlock(&g_plugin_lock);
        LOG_ERROR_T("Plugin", "Register", "MallocFail", "malloc failed");
        return -1;
    }

    memcpy(new_plugin, plugin, sizeof(plugin_t));
    new_plugin->next = NULL;
    new_plugin->state = PLUGIN_STATE_LOADED;

    /* 添加到链表头部 */
    new_plugin->next = g_plugins;
    g_plugins = new_plugin;
    g_plugin_count++;

    pthread_mutex_unlock(&g_plugin_lock);

    LOG_INFO_T("Plugin", "Register", "OK", "plugin '%s' registered (total=%d)", plugin->name, g_plugin_count);
    return 0;
}

int plugin_unregister(const char *name) {
    LOG_INFO_T("Plugin", "Unregister", "Enter", "name='%s'", name ? name : "(null)");

    if (!name || !name[0]) {
        LOG_ERROR_T("Plugin", "Unregister", "Invalid", "name is NULL or empty");
        return -1;
    }

    if (!g_initialized) {
        LOG_ERROR_T("Plugin", "Unregister", "NotInit", "plugin system not initialized");
        return -1;
    }

    pthread_mutex_lock(&g_plugin_lock);

    plugin_t *prev = NULL;
    plugin_t *curr = g_plugins;

    while (curr) {
        if (strcmp(curr->name, name) == 0) {
            break;
        }
        prev = curr;
        curr = curr->next;
    }

    if (!curr) {
        pthread_mutex_unlock(&g_plugin_lock);
        LOG_WARN_T("Plugin", "Unregister", "NotFound", "plugin '%s' not found", name);
        return -1;
    }

    /* 从链表中移除 */
    if (prev) {
        prev->next = curr->next;
    } else {
        g_plugins = curr->next;
    }
    g_plugin_count--;

    /* 停止并卸载 */
    if (curr->state == PLUGIN_STATE_ACTIVE) {
        if (curr->stop) curr->stop(curr);
        if (curr->shutdown) curr->shutdown(curr);
    }
    plugin_loader_unload(curr);
    free(curr);

    pthread_mutex_unlock(&g_plugin_lock);

    LOG_INFO_T("Plugin", "Unregister", "OK", "plugin '%s' unregistered", name);
    return 0;
}

plugin_t* plugin_find(const char *name) {
    if (!name || !name[0] || !g_initialized) {
        return NULL;
    }

    pthread_mutex_lock(&g_plugin_lock);

    plugin_t *p = g_plugins;
    while (p) {
        if (strcmp(p->name, name) == 0) {
            pthread_mutex_unlock(&g_plugin_lock);
            LOG_DEBUG_T("Plugin", "Find", "Found", "plugin '%s' found", name);
            return p;
        }
        p = p->next;
    }

    pthread_mutex_unlock(&g_plugin_lock);
    LOG_DEBUG_T("Plugin", "Find", "NotFound", "plugin '%s' not found", name);
    return NULL;
}

int plugin_list(plugin_t **out, int max_count) {
    if (!out || max_count <= 0 || !g_initialized) {
        return 0;
    }

    pthread_mutex_lock(&g_plugin_lock);

    int count = 0;
    plugin_t *p = g_plugins;
    while (p && count < max_count) {
        out[count++] = p;
        p = p->next;
    }

    pthread_mutex_unlock(&g_plugin_lock);

    LOG_DEBUG_T("Plugin", "List", "OK", "returned %d plugins", count);
    return count;
}

int plugin_count(void) {
    int count = g_plugin_count;
    LOG_DEBUG_T("Plugin", "Count", "result", "count=%d", count);
    return count;
}

const char* plugin_state_str(plugin_state_t state) {
    switch (state) {
        case PLUGIN_STATE_UNLOADED: return "unloaded";
        case PLUGIN_STATE_LOADED:   return "loaded";
        case PLUGIN_STATE_ACTIVE:   return "active";
        case PLUGIN_STATE_ERROR:    return "error";
        default:                    return "unknown";
    }
}

const char* plugin_type_str(plugin_type_t type) {
    switch (type) {
        case PLUGIN_TYPE_SKILL:       return "skill";
        case PLUGIN_TYPE_COMMAND:     return "command";
        case PLUGIN_TYPE_HOOK:        return "hook";
        case PLUGIN_TYPE_DATA_SOURCE: return "data_source";
        case PLUGIN_TYPE_UI:          return "ui";
        default:                      return "unknown";
    }
}