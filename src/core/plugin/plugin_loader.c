/**
 * @file    plugin_loader.c
 * @brief   C 插件动态加载器（dlopen/dlsym）
 * @version LN-B-4.2.0.0
 */

#include "plugin_loader.h"
#include "plugin.h"
#include "../../common/data_path.h"
#include "../../common/safe_string.h"
#include "../../lib/log_extra.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <dirent.h>
#include <dlfcn.h>
#include <errno.h>
#include <sys/stat.h>

/* ============================================================
 * 插件入口函数类型
 * ============================================================ */

typedef int (*plugin_entry_t)(plugin_t *plugin);

/* ============================================================
 * 内部辅助
 * ============================================================ */

/**
 * @brief 尝试加载单个插件文件
 * @param path 插件文件路径（.so 文件）
 * @return 0 成功，-1 失败
 */
static int load_plugin_file(const char *path) {
    LOG_DEBUG_T("PluginLoader", "LoadFile", "Enter", "path='%s'", path ? path : "(null)");

    if (!path) {
        LOG_ERROR_T("PluginLoader", "LoadFile", "Invalid", "path is NULL");
        return -1;
    }

    /* 检查文件是否存在 */
    if (access(path, F_OK) != 0) {
        LOG_WARN_T("PluginLoader", "LoadFile", "NotFound", "%s not found", path);
        return -1;
    }

    /* 使用 dlopen 加载共享库 */
    void *handle = dlopen(path, RTLD_NOW);
    if (!handle) {
        LOG_ERROR_T("PluginLoader", "LoadFile", "DlopenFail", "%s: %s", path, dlerror());
        return -1;
    }

    /* 清除之前的错误 */
    dlerror();

    /* 查找 plugin_entry 符号 */
    plugin_entry_t entry = (plugin_entry_t)dlsym(handle, "plugin_entry");
    const char *err = dlerror();
    if (err) {
        LOG_ERROR_T("PluginLoader", "LoadFile", "DlsymFail", "plugin_entry not found in %s: %s", path, err);
        dlclose(handle);
        return -1;
    }

    /* 创建插件结构（由插件填充） */
    plugin_t plugin;
    memset(&plugin, 0, sizeof(plugin_t));
    plugin.handle = handle;

    /* 调用插件入口函数 */
    int ret = entry(&plugin);
    if (ret != 0) {
        LOG_ERROR_T("PluginLoader", "LoadFile", "EntryFail", "plugin_entry returned %d for %s", ret, path);
        dlclose(handle);
        return -1;
    }

    /* 验证插件元数据 */
    if (plugin.name[0] == '\0') {
        LOG_ERROR_T("PluginLoader", "LoadFile", "NoName", "plugin has no name");
        dlclose(handle);
        return -1;
    }

    /* 注册插件 */
    ret = plugin_register(&plugin);
    if (ret != 0) {
        LOG_ERROR_T("PluginLoader", "LoadFile", "RegisterFail", "plugin_register failed for %s", plugin.name);
        dlclose(handle);
        return -1;
    }

    LOG_INFO_T("PluginLoader", "LoadFile", "OK", "loaded plugin '%s' version '%s' from %s",
               plugin.name, plugin.version, path);
    return 0;
}

/* ============================================================
 * 公共 API 实现
 * ============================================================ */

int plugin_loader_scan(const char *dir) {
    LOG_INFO_T("PluginLoader", "Scan", "Enter", "dir='%s'", dir ? dir : "(null)");

    if (!dir) {
        LOG_ERROR_T("PluginLoader", "Scan", "Invalid", "dir is NULL");
        return -1;
    }

    /* 确保目录存在 */
    if (access(dir, F_OK) != 0) {
        LOG_DEBUG_T("PluginLoader", "Scan", "NoDir", "%s does not exist, creating", dir);
        if (mkdir(dir, 0755) != 0) {
            LOG_WARN_T("PluginLoader", "Scan", "MkdirFail", "cannot create %s: %s", dir, strerror(errno));
            return 0;
        }
        LOG_DEBUG_T("PluginLoader", "Scan", "DirCreated", "created %s", dir);
    }

    DIR *d = opendir(dir);
    if (!d) {
        LOG_ERROR_T("PluginLoader", "Scan", "OpendirFail", "cannot open %s: %s", dir, strerror(errno));
        return -1;
    }

    int loaded = 0;
    struct dirent *entry;

    while ((entry = readdir(d)) != NULL) {
        if (entry->d_name[0] == '.') continue;

        char *dot = strrchr(entry->d_name, '.');
        if (!dot || strcmp(dot, ".so") != 0) continue;

        char full_path[512];
        safe_snprintf(full_path, sizeof(full_path), "%s/%s", dir, entry->d_name);

        LOG_DEBUG_T("PluginLoader", "Scan", "Found", "found plugin: %s", entry->d_name);

        if (load_plugin_file(full_path) == 0) {
            loaded++;
        } else {
            LOG_WARN_T("PluginLoader", "Scan", "LoadFail", "failed to load %s", entry->d_name);
        }
    }

    closedir(d);

    LOG_INFO_T("PluginLoader", "Scan", "Done", "scanned %s, loaded %d plugins", dir, loaded);
    return loaded;
}

int plugin_loader_load(const char *path) {
    LOG_INFO_T("PluginLoader", "Load", "Enter", "path='%s'", path ? path : "(null)");
    if (!path) {
        LOG_ERROR_T("PluginLoader", "Load", "Invalid", "path is NULL");
        return -1;
    }

    int ret = load_plugin_file(path);
    if (ret == 0) {
        LOG_INFO_T("PluginLoader", "Load", "OK", "loaded %s", path);
    } else {
        LOG_ERROR_T("PluginLoader", "Load", "Fail", "failed to load %s", path);
    }
    return ret;
}

void plugin_loader_unload(plugin_t *plugin) {
    LOG_DEBUG_T("PluginLoader", "Unload", "Enter", "plugin=%p", (void*)plugin);

    if (!plugin) {
        LOG_WARN_T("PluginLoader", "Unload", "Invalid", "plugin is NULL");
        return;
    }

    if (plugin->handle) {
        if (dlclose(plugin->handle) != 0) {
            LOG_WARN_T("PluginLoader", "Unload", "DlcloseFail", "dlclose failed: %s", dlerror());
        } else {
            LOG_DEBUG_T("PluginLoader", "Unload", "OK", "dlclose succeeded");
        }
        plugin->handle = NULL;
    }

    plugin->state = PLUGIN_STATE_UNLOADED;
}