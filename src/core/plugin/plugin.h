/**
 * @file    plugin.h
 * @brief   C 插件系统头文件 - 插件结构定义和核心 API
 * @version LN-B-4.2.0.0
 */

#ifndef CORE_PLUGIN_PLUGIN_H
#define CORE_PLUGIN_PLUGIN_H

#include <stdint.h>
#include <stddef.h>

/* ============================================================
 * 插件状态枚举
 * ============================================================ */

typedef enum {
    PLUGIN_STATE_UNLOADED = 0,
    PLUGIN_STATE_LOADED,
    PLUGIN_STATE_ACTIVE,
    PLUGIN_STATE_ERROR
} plugin_state_t;

/* ============================================================
 * 插件类型枚举
 * ============================================================ */

typedef enum {
    PLUGIN_TYPE_SKILL = 0,      /* 技能插件 - 新增 AI 技能 */
    PLUGIN_TYPE_COMMAND,        /* 命令插件 - 新增 Shell 命令 */
    PLUGIN_TYPE_HOOK,           /* Hook 插件 - 系统事件触发 */
    PLUGIN_TYPE_DATA_SOURCE,    /* 数据源插件 - 新增 AI 可查询数据源 */
    PLUGIN_TYPE_UI              /* UI 插件 - TUI 桌面小部件 */
} plugin_type_t;

/* ============================================================
 * 插件结构
 * ============================================================ */

typedef struct plugin {
    char name[64];              /* 插件名称 */
    char version[32];           /* 插件版本 */
    char description[256];      /* 插件描述 */
    char author[64];            /* 作者 */
    plugin_type_t type;         /* 插件类型 */
    plugin_state_t state;       /* 当前状态 */
    void *handle;               /* dlopen 返回的句柄 */
    void *user_data;            /* 插件私有数据 */

    /* ====== 生命周期回调 ====== */
    int (*init)(struct plugin *p);       /* 初始化 */
    int (*start)(struct plugin *p);      /* 启动 */
    int (*stop)(struct plugin *p);       /* 停止 */
    int (*shutdown)(struct plugin *p);   /* 关闭 */

    /* ====== 插件特有回调（根据类型选择实现） ====== */
    void *(*get_api)(const char *name);  /* 获取 API 函数指针 */
    int (*handle_event)(const char *event, void *data); /* 事件处理 */

    struct plugin *next;        /* 链表指针 */
} plugin_t;

/* ============================================================
 * 插件管理器 API
 * ============================================================ */

/**
 * @brief 初始化插件系统
 * @return 0 成功，-1 失败
 */
int plugin_system_init(void);

/**
 * @brief 关闭插件系统（卸载所有插件）
 */
void plugin_system_shutdown(void);

/**
 * @brief 注册插件
 * @param plugin 插件结构指针
 * @return 0 成功，-1 失败
 */
int plugin_register(plugin_t *plugin);

/**
 * @brief 注销插件
 * @param name 插件名称
 * @return 0 成功，-1 失败
 */
int plugin_unregister(const char *name);

/**
 * @brief 根据名称查找插件
 * @param name 插件名称
 * @return 插件指针，未找到返回 NULL
 */
plugin_t* plugin_find(const char *name);

/**
 * @brief 获取插件列表
 * @param out 输出数组
 * @param max_count 最大数量
 * @return 实际数量
 */
int plugin_list(plugin_t **out, int max_count);

/**
 * @brief 获取插件数量
 * @return 插件数量
 */
int plugin_count(void);

/**
 * @brief 获取插件状态字符串
 * @param state 状态枚举
 * @return 状态名称字符串
 */
const char* plugin_state_str(plugin_state_t state);

/**
 * @brief 获取插件类型字符串
 * @param type 类型枚举
 * @return 类型名称字符串
 */
const char* plugin_type_str(plugin_type_t type);

#endif /* CORE_PLUGIN_PLUGIN_H */