/**
 * @file    src/alert/plugin_loader.c
 * @brief   预警插件加载器（加载 /LINGOS/plugins/alert/ 下的插件）
 * @version LN-B-4.3.0.0
 * @par     核心协议：契约式编程（插件加载失败则终止子进程）
 * @changes 函数名改为 alert_plugin_loader_scan 避免与 core/plugin 冲突
 */

#include "plugin_loader.h"
#include "plugin_interface.h"
#include "../common/error_report.h"
#include "../common/safe_string.h"
#include "../lib/log_extra.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <dlfcn.h>
#include <sys/stat.h>
#include <unistd.h>

#define PLUGIN_DIR "/LINGOS/plugins/alert"
#define MAX_PLUGINS 16

static alert_plugin_t g_plugins[MAX_PLUGINS];
static int g_plugin_count = 0;

/* ============================================================
 * 加载单个插件
 * ============================================================ */

static int load_plugin(const char *path) {
    LOG_DEBUG_T("PluginLoader", "Load", "Enter", "path=%s", path);

    void *handle = dlopen(path, RTLD_NOW);
    if (!handle) {
        LOG_ERROR_T("PluginLoader", "Load", "DlopenFail", "dlopen error: %s", dlerror());
        return -1;
    }

    dlerror();

    int (*entry)(alert_plugin_t *) = (int (*)(alert_plugin_t *))dlsym(handle, "plugin_entry");
    const char *err = dlerror();
    if (err) {
        LOG_ERROR_T("PluginLoader", "Load", "SymbolFail", "plugin_entry not found: %s", err);
        dlclose(handle);
        return -1;
    }

    alert_plugin_t plugin;
    memset(&plugin, 0, sizeof(plugin));
    if (entry(&plugin) != 0) {
        LOG_ERROR_T("PluginLoader", "Load", "EntryFail", "plugin_entry returned error");
        dlclose(handle);
        return -1;
    }

    if (!plugin.fetch) {
        LOG_ERROR_T("PluginLoader", "Load", "NoFetch", "plugin does not provide fetch function");
        dlclose(handle);
        return -1;
    }

    if (g_plugin_count >= MAX_PLUGINS) {
        LOG_WARN_T("PluginLoader", "Load", "Overflow", "max plugins reached");
        dlclose(handle);
        return -1;
    }

    g_plugins[g_plugin_count] = plugin;
    g_plugin_count++;

    LOG_INFO_T("PluginLoader", "Load", "OK", "loaded plugin '%s' v%s", plugin.name, plugin.version);
    return 0;
}

/* ============================================================
 * 扫描并加载所有插件（重命名）
 * ============================================================ */

int alert_plugin_loader_scan(void) {
    LOG_DEBUG_T("PluginLoader", "Scan", "Enter", "scanning %s", PLUGIN_DIR);

    mkdir(PLUGIN_DIR, 0755);

    DIR *d = opendir(PLUGIN_DIR);
    if (!d) {
        LOG_WARN_T("PluginLoader", "Scan", "OpenFail", "cannot open %s", PLUGIN_DIR);
        return 0;
    }

    struct dirent *entry;
    int loaded = 0;
    while ((entry = readdir(d)) != NULL) {
        if (entry->d_name[0] == '.') continue;
        char *dot = strrchr(entry->d_name, '.');
        if (!dot || strcmp(dot, ".so") != 0) continue;

        char full_path[512];
        safe_snprintf(full_path, sizeof(full_path), "%s/%s", PLUGIN_DIR, entry->d_name);

        if (load_plugin(full_path) == 0) {
            loaded++;
        }
    }
    closedir(d);

    LOG_INFO_T("PluginLoader", "Scan", "OK", "loaded %d plugins", loaded);
    return loaded;
}

/* ============================================================
 * 从所有插件获取数据
 * ============================================================ */

int plugin_loader_fetch_all(alert_event_t *events, int max_count) {
    if (!events || max_count <= 0) return 0;

    int count = 0;
    for (int i = 0; i < g_plugin_count && count < max_count; i++) {
        if (g_plugins[i].fetch) {
            int n = g_plugins[i].fetch(events + count, max_count - count);
            count += n;
            LOG_DEBUG_T("PluginLoader", "Fetch", "Plugin", "plugin '%s' returned %d events",
                        g_plugins[i].name, n);
        }
    }
    return count;
}

/* ============================================================
 * 获取插件数量
 * ============================================================ */

int plugin_loader_count(void) {
    return g_plugin_count;
}