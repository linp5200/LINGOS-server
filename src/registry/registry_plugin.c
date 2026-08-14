/**
 * @file    registry_plugin.c
 * @brief   插件注册与查询
 * @version LN-B-5.0.0.0
 */

#include "registry.h"
#include "../common/safe_string.h"
#include "../lib/log_extra.h"
#include "../common/data_path.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

#define PLUGIN_REGISTRY_DIR "/registry/plugins"

/**
 * @brief 扫描插件目录并注册
 */
int registry_plugin_scan_and_register(const char *dir) {
    LOG_INFO_T("RegistryPlugin", "Scan", "Enter", "dir='%s'", dir ? dir : "(null)");

    if (!dir) return -1;
    const char *root = lingos_data_root();
    char full_dir[512];
    safe_snprintf(full_dir, sizeof(full_dir), "%s%s", root, dir);

    DIR *d = opendir(full_dir);
    if (!d) {
        LOG_WARN_T("RegistryPlugin", "Scan", "OpenFail", "cannot open %s", full_dir);
        return 0;
    }

    int count = 0;
    struct dirent *entry;
    while ((entry = readdir(d)) != NULL) {
        if (entry->d_name[0] == '.') continue;
        /* 注册 .so 或 .py 插件 */
        char *dot = strrchr(entry->d_name, '.');
        if (!dot || (strcmp(dot, ".so") != 0 && strcmp(dot, ".py") != 0)) continue;

        registry_entry_t reg_entry;
        memset(&reg_entry, 0, sizeof(reg_entry));
        safe_snprintf(reg_entry.id, sizeof(reg_entry.id), "plugin:%s", entry->d_name);
        reg_entry.type = REG_TYPE_PLUGIN;
        safe_strncpy(reg_entry.name, entry->d_name, sizeof(reg_entry.name));
        safe_strncpy(reg_entry.version, "1.0.0", sizeof(reg_entry.version));
        reg_entry.status = REG_STATUS_ACTIVE;
        char full_path[512];
        safe_snprintf(full_path, sizeof(full_path), "%s/%s", full_dir, entry->d_name);
        safe_strncpy(reg_entry.path, full_path, sizeof(reg_entry.path));

        if (registry_register(&reg_entry) == 0) count++;
    }

    closedir(d);
    LOG_INFO_T("RegistryPlugin", "Scan", "OK", "registered %d plugins from %s", count, dir);
    return count;
}