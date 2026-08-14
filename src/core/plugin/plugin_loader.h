/**
 * @file    plugin_loader.h
 * @brief   C 插件动态加载器接口
 * @version LN-B-4.2.0.0
 */

#ifndef CORE_PLUGIN_PLUGIN_LOADER_H
#define CORE_PLUGIN_PLUGIN_LOADER_H

#include "plugin.h"

/**
 * @brief 扫描目录并加载所有插件
 * @param dir 插件目录路径
 * @return 成功加载的插件数量
 */
int plugin_loader_scan(const char *dir);

/**
 * @brief 加载单个插件文件
 * @param path .so 文件路径
 * @return 0 成功，-1 失败
 */
int plugin_loader_load(const char *path);

/**
 * @brief 卸载插件（释放资源）
 * @param plugin 插件结构指针
 */
void plugin_loader_unload(plugin_t *plugin);

#endif /* CORE_PLUGIN_PLUGIN_LOADER_H */