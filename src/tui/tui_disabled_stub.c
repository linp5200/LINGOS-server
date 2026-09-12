/**
 * @file    src/tui/tui_disabled_stub.c
 * @brief   TUI 关闭时的符号桩（先生 2026-09-12 裁决：关闭入口，保留代码）
 * @version LN-0.5.0
 *
 * 用途：
 *   当 ENABLE_TUI=0 时，tui/ 目录与 config_renderer_tui.c **不参与编译**，
 *   但以下符号仍被非 TUI 代码引用：
 *     · renderer_tui_impl_create()  ← config_renderer.c
 *     · tui_desktop_run()           ← main.c / shell.c
 *   本文件提供**安全降级**实现：一律返回「不可用」，调用方自然回退到 CLI。
 *
 * 重新启用 TUI：make ENABLE_TUI=1（本文件将不参与编译）
 */

#include "../config/config_renderer.h"
#include "../lib/log_extra.h"

#include <string.h>

/* ============================================================
 * 渲染器：TUI 不可用 → 返回 -1，向导自动回退 CLI/RAW
 * ============================================================ */
int renderer_tui_impl_create(renderer_ctx_t *ctx) {
    if (ctx) memset(ctx, 0, sizeof(*ctx));
    LOG_WARN_T("TUI", "Renderer", "Disabled",
               "TUI 渲染器不可用（本构建以 ENABLE_TUI=0 编译）—— 回退 CLI");
    return -1;
}

/* ============================================================
 * 桌面：TUI 不可用 → 返回 -1
 * ============================================================ */
int tui_desktop_run(void) {
    LOG_WARN_T("TUI", "Desktop", "Disabled",
               "TUI 桌面不可用（本构建以 ENABLE_TUI=0 编译）—— 回退 Shell CLI");
    return -1;
}
