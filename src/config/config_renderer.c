/**
 * @file    src/config/config_renderer.c
 * @brief   渲染器工厂与工具函数实现
 * @version LN-0.4.3
 * @par     实现 renderer_create 和工具函数。
 */

#include "config_renderer.h"
#include "config_renderer_tui.h"
#include "config_renderer_cli.h"
#include "config_renderer_raw.h"
#include "../common/safe_string.h"
#include "../lib/log_extra.h"
#include <string.h>

/* ============================================================
 * 创建函数
 * ============================================================ */
int renderer_create(renderer_type_t type, renderer_ctx_t *ctx) {
    if (!ctx) return -1;
    memset(ctx, 0, sizeof(renderer_ctx_t));
    ctx->type = type;

    switch (type) {
        case RENDERER_TYPE_TUI:
            safe_strncpy(ctx->name, "tui", sizeof(ctx->name));
            return renderer_tui_impl_create(ctx);
        case RENDERER_TYPE_CLI:
            safe_strncpy(ctx->name, "cli", sizeof(ctx->name));
            return renderer_cli_impl_create(ctx);
        case RENDERER_TYPE_RAW:
            safe_strncpy(ctx->name, "raw", sizeof(ctx->name));
            return renderer_raw_impl_create(ctx);
        default:
            LOG_ERROR_T("Renderer", "Create", "Invalid", "unknown type %d", type);
            return -1;
    }
}

int renderer_tui_create(renderer_ctx_t *ctx) {
    return renderer_create(RENDERER_TYPE_TUI, ctx);
}

int renderer_cli_create(renderer_ctx_t *ctx) {
    return renderer_create(RENDERER_TYPE_CLI, ctx);
}

int renderer_raw_create(renderer_ctx_t *ctx) {
    return renderer_create(RENDERER_TYPE_RAW, ctx);
}

void renderer_destroy(renderer_ctx_t *ctx) {
    if (!ctx) return;
    if (ctx->cleanup) ctx->cleanup(ctx);
    memset(ctx, 0, sizeof(renderer_ctx_t));
}

/* ============================================================
 * 工具函数
 * ============================================================ */
const char* renderer_type_to_string(renderer_type_t type) {
    switch (type) {
        case RENDERER_TYPE_TUI: return "tui";
        case RENDERER_TYPE_CLI: return "cli";
        case RENDERER_TYPE_RAW: return "raw";
        default: return "unknown";
    }
}

int renderer_type_from_string(const char *str) {
    if (!str) return -1;
    if (strcmp(str, "tui") == 0) return RENDERER_TYPE_TUI;
    if (strcmp(str, "cli") == 0) return RENDERER_TYPE_CLI;
    if (strcmp(str, "raw") == 0) return RENDERER_TYPE_RAW;
    return -1;
}

int renderer_type_next(renderer_type_t type) {
    int next = (int)type + 1;
    if (next >= RENDERER_TYPE_COUNT) return -1;
    return next;
}

int renderer_type_prev(renderer_type_t type) {
    int prev = (int)type - 1;
    if (prev < 0) return -1;
    return prev;
}

int renderer_type_valid(renderer_type_t type) {
    return (type >= 0 && type < RENDERER_TYPE_COUNT);
}