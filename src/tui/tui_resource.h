/**
 * @file    tui_resource.h
 * @brief   TUI 资源池管理头文件
 * @version LN-B-4.3.0.0
 */

#ifndef TUI_RESOURCE_H
#define TUI_RESOURCE_H

#include <notcurses/notcurses.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    RESOURCE_PLANE,
    RESOURCE_MEMORY
} resource_type_t;

/**
 * @brief 注册资源到池中
 * @param ptr 资源指针
 */
void tui_resource_register(void *ptr);

/**
 * @brief 从池中取消注册
 * @param ptr 资源指针
 */
void tui_resource_unregister(void *ptr);

/**
 * @brief 清理所有资源（异常退出时调用）
 */
void tui_resource_cleanup_all(void);

/**
 * @brief 获取当前资源数量（调试用）
 * @return 资源数量
 */
int tui_resource_count(void);

#ifdef __cplusplus
}
#endif

#endif /* TUI_RESOURCE_H */