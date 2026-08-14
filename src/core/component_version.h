/**
 * @file    component_version.h
 * @brief   组件版本管理（注册、检查兼容性、升级）
 * @version LN-B-4.2.0.0
 */

#ifndef UPDATE_COMPONENT_VERSION_H
#define UPDATE_COMPONENT_VERSION_H

#include <stdint.h>

/* ============================================================
 * 组件结构
 * ============================================================ */

typedef struct {
    const char *name;           /* 组件名称 */
    const char *path;           /* 版本文件路径 */
    const char *cur_version;    /* 当前版本 */
    const char *min_supported;  /* 最低支持版本 */
    const char *max_supported;  /* 最高支持版本 */
    int (*migrate)(void);       /* 迁移函数（升级时调用） */
} component_t;

/* ============================================================
 * 公共 API
 * ============================================================ */

/**
 * @brief 注册组件到管理系统
 * @param comp 组件指针
 * @return 0 成功，-1 失败
 */
int component_register(component_t *comp);

/**
 * @brief 初始化组件版本系统（加载所有已注册组件的版本）
 * @return 0 成功，-1 失败
 */
int component_version_init(void);

/**
 * @brief 获取组件当前版本
 * @param name 组件名称
 * @return 版本字符串，未找到返回 NULL
 */
const char *component_get_version(const char *name);

/**
 * @brief 检查组件当前版本是否在支持范围内
 * @param comp 组件指针
 * @return 1 兼容，0 不兼容
 */
int component_is_compatible(const component_t *comp);

/**
 * @brief 升级指定组件
 * @param name 组件名称
 * @return 0 成功，-1 失败
 */
int component_upgrade(const char *name);

/**
 * @brief 升级所有不兼容的组件（交互式确认）
 * @return 成功升级的数量
 */
int component_upgrade_all(void);

/**
 * @brief 显示所有组件的版本状态
 */
void component_show_status(void);

#endif /* UPDATE_COMPONENT_VERSION_H */