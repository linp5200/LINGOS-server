/**
 * @file    plugin_sdk.h
 * @brief   C 插件 SDK - 插件开发者接口
 * @version LN-B-4.3.0.0
 * @par     核心协议：契约式编程（插件必须实现所有接口）
 */

#ifndef PLUGIN_SDK_H
#define PLUGIN_SDK_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================
 * 插件元数据
 * ============================================================ */

#define PLUGIN_NAME_MAX 64
#define PLUGIN_VERSION_MAX 32
#define PLUGIN_DESC_MAX 256

typedef struct {
    char name[PLUGIN_NAME_MAX];
    char version[PLUGIN_VERSION_MAX];
    char description[PLUGIN_DESC_MAX];
    char author[64];
    uint32_t api_version;  /* LING OS API 版本号 */
} plugin_metadata_t;

/* ============================================================
 * 插件类型
 * ============================================================ */

typedef enum {
    PLUGIN_TYPE_SKILL = 0,
    PLUGIN_TYPE_COMMAND,
    PLUGIN_TYPE_DATA_SOURCE,
    PLUGIN_TYPE_HOOK,
    PLUGIN_TYPE_UI_WIDGET
} plugin_type_t;

/* ============================================================
 * 插件上下文
 * ============================================================ */

typedef struct {
    void *system_api;        /* 系统 API 函数指针表（预留） */
    void *user_data;         /* 插件私有数据 */
    const char *data_root;   /* /LINGOS 路径 */
} plugin_context_t;

/* ============================================================
 * 插件必须实现的函数
 * ============================================================ */

/**
 * @brief 获取插件元数据（必须实现）
 * @param meta 输出元数据
 */
void plugin_get_metadata(plugin_metadata_t *meta);

/**
 * @brief 初始化插件（必须实现）
 * @param ctx 插件上下文
 * @return 0 成功，-1 失败
 */
int plugin_init(plugin_context_t *ctx);

/**
 * @brief 启动插件（必须实现）
 * @return 0 成功，-1 失败
 */
int plugin_start(void);

/**
 * @brief 停止插件（必须实现）
 */
void plugin_stop(void);

/**
 * @brief 获取插件类型（必须实现）
 * @return 插件类型
 */
plugin_type_t plugin_get_type(void);

/* ============================================================
 * 可选的回调函数（可为 NULL）
 * ============================================================ */

/**
 * @brief 处理事件（可选）
 * @param event_name 事件名称
 * @param data 事件数据
 */
void plugin_on_event(const char *event_name, void *data);

/**
 * @brief 热重载（可选）
 */
int plugin_on_reload(void);

#ifdef __cplusplus
}
#endif

#endif /* PLUGIN_SDK_H */