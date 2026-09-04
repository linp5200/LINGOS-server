/**
 * @file    src/config/config_debug.c
 * @brief   配置调试模块实现
 * @version LN-0.4.3
 * @par     核心协议：C1, C-C
 * @changes 实现 config_debug_test 和 config_debug_cut；
 *          支持自检输出和升降级模拟。
 */

#include "config_debug.h"
#include "config_renderer.h"
#include "../common/lang.h"
#include "../common/safe_string.h"
#include "../drivers/uart.h"
#include "../lib/log_extra.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* ============================================================
 * 内部辅助
 * ============================================================ */
static const char* debug_mode_to_display(renderer_type_t mode) {
    switch (mode) {
        case RENDERER_TYPE_TUI: return "TUI";
        case RENDERER_TYPE_CLI: return "CLI";
        case RENDERER_TYPE_RAW: return "RAW";
        default: return "UNKNOWN";
    }
}

/* ============================================================
 * 实现：测试自检
 * ============================================================ */
int config_debug_test(renderer_type_t mode) {
    LOG_INFO_T("ConfigDebug", "Test", "Enter", "mode=%d (%s)", mode, debug_mode_to_display(mode));

    renderer_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));

    /* 创建渲染器 */
    if (renderer_create(mode, &ctx) != 0) {
        LOG_ERROR_T("ConfigDebug", "Test", "CreateFail", "renderer_create failed for mode %d", mode);
        uart_puts(COLOR_RED);
        uart_puts(tr("❌ Renderer creation failed\n", "❌ 渲染器创建失败\n"));
        uart_puts(COLOR_RESET);
        return -1;
    }

    /* 执行自检 */
    int ret = renderer_self_test(&ctx);

    if (ret == 0) {
        LOG_INFO_T("ConfigDebug", "Test", "OK", "renderer self-test passed");
        uart_puts(COLOR_GREEN);
        uart_puts(tr("✅ Renderer test passed\n", "✅ 渲染器测试通过\n"));
        uart_puts(COLOR_RESET);
    } else {
        LOG_ERROR_T("ConfigDebug", "Test", "Fail", "renderer self-test failed (ret=%d)", ret);
        uart_puts(COLOR_RED);
        uart_puts(tr("❌ Renderer test failed (see logs)\n", "❌ 渲染器测试失败（请查看日志）\n"));
        uart_puts(COLOR_RESET);
    }

    renderer_destroy(&ctx);
    return ret;
}

/* ============================================================
 * 实现：升降级模拟
 * ============================================================ */
int config_debug_cut(renderer_type_t mode, int direction) {
    LOG_INFO_T("ConfigDebug", "Cut", "Enter", "mode=%d, direction=%s",
               mode, direction == CONFIG_CUT_DOWN ? "DOWN" : "UP");

    int new_mode;
    const char *action;

    if (direction == CONFIG_CUT_DOWN) {
        action = tr("downgrade to", "降级到");
        new_mode = renderer_type_next(mode);
        if (new_mode < 0) {
            LOG_WARN_T("ConfigDebug", "Cut", "Invalid", "cannot downgrade from RAW");
            uart_puts(tr("❌ Cannot downgrade from RAW (already lowest)\n",
                         "❌ 不能从 RAW 降级（已是最低级）\n"));
            return -1;
        }
    } else {
        action = tr("upgrade to", "升级到");
        new_mode = renderer_type_prev(mode);
        if (new_mode < 0) {
            LOG_WARN_T("ConfigDebug", "Cut", "Invalid", "cannot upgrade from TUI");
            uart_puts(tr("❌ Cannot upgrade from TUI (already highest)\n",
                         "❌ 不能从 TUI 升级（已是最高级）\n"));
            return -1;
        }
    }

    /* 验证新模式的渲染器是否可用 */
    renderer_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    if (renderer_create((renderer_type_t)new_mode, &ctx) != 0) {
        LOG_WARN_T("ConfigDebug", "Cut", "CreateFail", "cannot create renderer for mode %d", new_mode);
        uart_puts(COLOR_YELLOW);
        uart_puts(tr("⚠ Target renderer unavailable\n", "⚠ 目标渲染器不可用\n"));
        uart_puts(COLOR_RESET);
        return -1;
    }
    renderer_destroy(&ctx);

    /* 输出成功信息 */
    char buf[128];
    safe_snprintf(buf, sizeof(buf), "%s %s %s %s",
                  tr("✅ Switched from", "✅ 从"),
                  debug_mode_to_display(mode),
                  action,
                  debug_mode_to_display((renderer_type_t)new_mode));
    uart_puts(buf);
    uart_puts("\n");

    LOG_INFO_T("ConfigDebug", "Cut", "OK", "switched from %d to %d", mode, new_mode);
    return 0;
}

/* ============================================================
 * 获取可读名称
 * ============================================================ */
const char* config_debug_mode_name(renderer_type_t mode) {
    return debug_mode_to_display(mode);
}