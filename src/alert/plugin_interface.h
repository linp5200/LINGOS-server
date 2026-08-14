/**
 * @file    plugin_interface.h
 * @brief   预警插件统一接口（供开发者实现）
 * @version LN-B-4.3.0.0
 * @par     核心协议：契约式编程（插件必须实现 fetch_alert_data）
 */

#ifndef ALERT_PLUGIN_INTERFACE_H
#define ALERT_PLUGIN_INTERFACE_H

#include "alert_manager.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 插件必须实现的函数原型
 * @param events 输出事件数组
 * @param max_count 最大数量
 * @return 实际填充的事件数量
 */
typedef int (*alert_plugin_fetch_fn)(alert_event_t *events, int max_count);

/**
 * @brief 插件元数据结构（可选）
 */
typedef struct {
    const char *name;
    const char *version;
    const char *author;
    alert_plugin_fetch_fn fetch;
} alert_plugin_t;

/**
 * @brief 插件入口函数（插件必须导出此符号）
 * @param plugin 填充插件信息
 * @return 0 成功，-1 失败
 */
int plugin_entry(alert_plugin_t *plugin);

#ifdef __cplusplus
}
#endif

#endif /* ALERT_PLUGIN_INTERFACE_H */