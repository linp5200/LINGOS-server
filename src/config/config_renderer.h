/**
 * @file    src/config/config_renderer.h
 * @brief   配置渲染器统一接口
 * @version LN-B-5.1.2.6-rc
 * @par     包含 renderer_ctx_t 定义，self_test 和 is_healthy 包装函数。
 */

#ifndef CONFIG_RENDERER_H
#define CONFIG_RENDERER_H

#include "wizard_engine.h"   /* wizard_step_def_t, wizard_option_t */
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================
 * 渲染器类型枚举
 * ============================================================ */
typedef enum {
    RENDERER_TYPE_TUI = 0,
    RENDERER_TYPE_CLI = 1,
    RENDERER_TYPE_RAW = 2,
    RENDERER_TYPE_COUNT
} renderer_type_t;

/* ============================================================
 * 渲染器上下文
 * ============================================================ */
typedef struct renderer_ctx {
    void *impl;

    /* 渲染函数 */
    int (*render_header)(struct renderer_ctx *ctx, wizard_step_def_t *step,
                         int current, int total);
    int (*render_options)(struct renderer_ctx *ctx, wizard_option_t *options,
                          int count, int selected);
    int (*render_input)(struct renderer_ctx *ctx, const char *prompt,
                        char *buf, size_t size);
    int (*render_message)(struct renderer_ctx *ctx, const char *msg, int is_error);
    int (*render_complete)(struct renderer_ctx *ctx, int success);

    /* 输入函数 */
    int (*get_input)(struct renderer_ctx *ctx, char *buf, size_t size);
    int (*wait_key)(struct renderer_ctx *ctx);

    /* 生命周期 */
    void (*cleanup)(struct renderer_ctx *ctx);

    /* 健康与自检 */
    int (*self_test)(struct renderer_ctx *ctx);
    int (*is_healthy)(struct renderer_ctx *ctx);

    /* 元信息 */
    renderer_type_t type;
    char name[16];
} renderer_ctx_t;

/* ============================================================
 * 创建函数
 * ============================================================ */
int renderer_tui_create(renderer_ctx_t *ctx);
int renderer_cli_create(renderer_ctx_t *ctx);
int renderer_raw_create(renderer_ctx_t *ctx);
int renderer_create(renderer_type_t type, renderer_ctx_t *ctx);

/* ============================================================
 * 销毁函数
 * ============================================================ */
void renderer_destroy(renderer_ctx_t *ctx);

/* ============================================================
 * 包装函数（内联）
 * ============================================================ */
static inline int renderer_self_test(renderer_ctx_t *ctx) {
    if (ctx && ctx->self_test) return ctx->self_test(ctx);
    return -1;
}

static inline int renderer_is_healthy(renderer_ctx_t *ctx) {
    if (ctx && ctx->is_healthy) return ctx->is_healthy(ctx);
    return 0;
}

/* ============================================================
 * 工具函数
 * ============================================================ */
const char* renderer_type_to_string(renderer_type_t type);
int renderer_type_from_string(const char *str);
int renderer_type_next(renderer_type_t type);
int renderer_type_prev(renderer_type_t type);
int renderer_type_valid(renderer_type_t type);

#ifdef __cplusplus
}
#endif

#endif /* CONFIG_RENDERER_H */