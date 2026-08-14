#ifndef CORE_CONFIG_LOADER_H
#define CORE_CONFIG_LOADER_H

#include "cJSON.h"

/* 注册配置文件处理器 */
void config_register(const char *path,
                     int (*load_func)(const cJSON *root),
                     void (*reload_notify)(void));

/* 加载所有已注册的配置文件 */
int config_load_all(void);

/* 热重载所有配置文件 */
int config_reload_all(void);

#endif