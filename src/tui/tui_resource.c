/**
 * @file    tui_resource.c
 * @brief   TUI 资源池管理
 * @version LN-B-4.3.0.0
 * @par     核心协议：防弹编程（资源自动清理）
 */

#include "tui_resource.h"
#include "../lib/log_extra.h"
#include <stdlib.h>
#include <string.h>
#include <pthread.h>

/* ============================================================
 * 资源节点
 * ============================================================ */

typedef struct resource_node {
    void *ptr;
    resource_type_t type;
    struct resource_node *next;
} resource_node_t;

static resource_node_t *g_resources = NULL;
static pthread_mutex_t g_res_lock = PTHREAD_MUTEX_INITIALIZER;

/* ============================================================
 * 注册资源
 * ============================================================ */

void tui_resource_register(void *ptr) {
    if (!ptr) return;
    pthread_mutex_lock(&g_res_lock);
    resource_node_t *node = malloc(sizeof(resource_node_t));
    if (node) {
        node->ptr = ptr;
        node->type = RESOURCE_PLANE; /* 默认平面，后续可扩展 */
        node->next = g_resources;
        g_resources = node;
        LOG_DEBUG_T("TuiResource", "Register", "OK", "registered %p", ptr);
    } else {
        LOG_ERROR_T("TuiResource", "Register", "MallocFail", "cannot allocate resource node");
    }
    pthread_mutex_unlock(&g_res_lock);
}

/* ============================================================
 * 取消注册
 * ============================================================ */

void tui_resource_unregister(void *ptr) {
    if (!ptr) return;
    pthread_mutex_lock(&g_res_lock);
    resource_node_t **pp = &g_resources;
    while (*pp) {
        if ((*pp)->ptr == ptr) {
            resource_node_t *tmp = *pp;
            *pp = (*pp)->next;
            free(tmp);
            LOG_DEBUG_T("TuiResource", "Unregister", "OK", "unregistered %p", ptr);
            break;
        }
        pp = &(*pp)->next;
    }
    pthread_mutex_unlock(&g_res_lock);
}

/* ============================================================
 * 清理所有资源（异常退出时调用）
 * ============================================================ */

void tui_resource_cleanup_all(void) {
    pthread_mutex_lock(&g_res_lock);
    LOG_INFO_T("TuiResource", "Cleanup", "Enter", "cleaning up all resources");

    resource_node_t *node = g_resources;
    while (node) {
        resource_node_t *next = node->next;
        if (node->type == RESOURCE_PLANE && node->ptr) {
            ncplane_destroy((struct ncplane*)node->ptr);
            LOG_DEBUG_T("TuiResource", "Cleanup", "Destroy", "destroyed plane %p", node->ptr);
        } else if (node->type == RESOURCE_MEMORY && node->ptr) {
            free(node->ptr);
            LOG_DEBUG_T("TuiResource", "Cleanup", "Free", "freed memory %p", node->ptr);
        }
        free(node);
        node = next;
    }
    g_resources = NULL;
    pthread_mutex_unlock(&g_res_lock);
    LOG_INFO_T("TuiResource", "Cleanup", "OK", "all resources cleaned up");
}

/* ============================================================
 * 获取资源数量（调试用）
 * ============================================================ */

int tui_resource_count(void) {
    int count = 0;
    pthread_mutex_lock(&g_res_lock);
    resource_node_t *node = g_resources;
    while (node) {
        count++;
        node = node->next;
    }
    pthread_mutex_unlock(&g_res_lock);
    return count;
}