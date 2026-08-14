/**
 * @file    src/alert/plugin_loader.h
 * @brief   预警插件加载器头文件
 * @version LN-B-4.3.0.0
 * @changes 函数名改为 alert_plugin_loader_scan
 */

#ifndef ALERT_PLUGIN_LOADER_H
#define ALERT_PLUGIN_LOADER_H

#include "alert_manager.h"

int alert_plugin_loader_scan(void);
int plugin_loader_fetch_all(alert_event_t *events, int max_count);
int plugin_loader_count(void);

#endif /* ALERT_PLUGIN_LOADER_H */