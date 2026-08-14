/**
 * @file    plugin_api.h
 * @brief   插件 API 接口定义 - 供插件开发者使用
 * @version LN-B-4.2.0.0
 *
 * 插件开发者应包含此头文件，并实现 plugin_entry 函数。
 *
 * 示例：
 *   #include "plugin_api.h"
 *
 *   static int my_init(plugin_t *p) {
 *       LOG_INFO_T("MyPlugin", "Init", "OK", "initialized");
 *       return 0;
 *   }
 *
 *   int plugin_entry(plugin_t *p) {
 *       strcpy(p->name, "my_plugin");
 *       strcpy(p->version, "1.0.0");
 *       strcpy(p->description, "My test plugin");
 *       strcpy(p->author, "LING OS");
 *       p->type = PLUGIN_TYPE_SKILL;
 *       p->init = my_init;
 *       return 0;
 *   }
 */

#ifndef CORE_PLUGIN_PLUGIN_API_H
#define CORE_PLUGIN_PLUGIN_API_H

#include "plugin.h"
#include "../../lib/log_extra.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ============================================================
 * 插件入口函数声明（每个插件必须实现）
 * ============================================================
 *
 * 插件入口函数在插件加载时被调用，用于填充 plugin_t 结构。
 * 必须设置 name 字段，其他字段可选。
 *
 * @param plugin 插件结构指针（由系统分配）
 * @return 0 成功，-1 失败
 */
int plugin_entry(plugin_t *plugin);

/* ============================================================
 * 插件辅助宏
 * ============================================================ */

/**
 * @brief 设置插件名称
 */
#define PLUGIN_SET_NAME(p, n) do { \
    strncpy((p)->name, (n), sizeof((p)->name) - 1); \
    (p)->name[sizeof((p)->name) - 1] = '\0'; \
} while(0)

/**
 * @brief 设置插件版本
 */
#define PLUGIN_SET_VERSION(p, v) do { \
    strncpy((p)->version, (v), sizeof((p)->version) - 1); \
    (p)->version[sizeof((p)->version) - 1] = '\0'; \
} while(0)

/**
 * @brief 设置插件描述
 */
#define PLUGIN_SET_DESC(p, d) do { \
    strncpy((p)->description, (d), sizeof((p)->description) - 1); \
    (p)->description[sizeof((p)->description) - 1] = '\0'; \
} while(0)

/**
 * @brief 设置插件作者
 */
#define PLUGIN_SET_AUTHOR(p, a) do { \
    strncpy((p)->author, (a), sizeof((p)->author) - 1); \
    (p)->author[sizeof((p)->author) - 1] = '\0'; \
} while(0)

/* ============================================================
 * 插件 API 辅助函数（供插件使用）
 * ============================================================ */

/**
 * @brief 获取系统 API 函数（预留）
 * @param name API 名称
 * @return 函数指针，未找到返回 NULL
 */
static inline void* plugin_get_api(const char *name) {
    (void)name;
    /* 预留：后续可扩展 */
    return NULL;
}

/**
 * @brief 注册插件提供的技能（供插件使用）
 * @param name 技能名称
 * @param func 技能函数指针
 * @param risk 风险等级
 * @return 0 成功，-1 失败
 */
static inline int plugin_register_skill(const char *name, void *func, const char *risk) {
    (void)name;
    (void)func;
    (void)risk;
    /* 预留：后续通过 Python 端注册技能 */
    LOG_DEBUG_T("PluginAPI", "RegisterSkill", "Stub", "skill '%s' (stub)", name);
    return 0;
}

/**
 * @brief 注册插件提供的 Shell 命令（供插件使用）
 * @param name 命令名称
 * @param func 命令函数指针
 * @return 0 成功，-1 失败
 */
static inline int plugin_register_command(const char *name, void *func) {
    (void)name;
    (void)func;
    /* 预留：后续通过 Shell 注册命令 */
    LOG_DEBUG_T("PluginAPI", "RegisterCommand", "Stub", "command '%s' (stub)", name);
    return 0;
}

#endif /* CORE_PLUGIN_PLUGIN_API_H */