/**
 * @file    src/tui/tui_wizard.c
 * @brief   TUI 配置向导渲染器（v0.7.0-hf2 存根化）
 * @version LN-0.7.0
 *
 * 【背景】原实现（2026-09-25 全量静态扫描发现）引用了一整组**从未入库**的 API：
 *   cli_wizard.h / raw_wizard.h（不存在）· wizard_core_get_current_step /
 *   wizard_core_merge_config / wizard_core_next_step / wizard_core_set_cancelled
 *   （无定义）· state->ctx 成员（结构不匹配）——即 ENABLE_TUI=1 构建必然失败。
 *   全仓核对：tui_wizard_run() 没有任何调用者。
 *
 * 【处理】按"优雅降级"存根化：保留接口、返回 -1（未执行）+ WARN 日志。
 *   原件备份于 deadcode/tui_wizard.c.orig。若未来恢复 TUI 向导，
 *   请基于现役的 wizard_engine API（src/config/wizard_engine.c）重写。
 */

#include "tui_wizard.h"
#include "../lib/log_extra.h"

int tui_wizard_run(wizard_state_t *state) {
    (void)state;
    LOG_WARN_T("TUIWizard", "Run", "Stub",
               "TUI wizard unavailable in this build (legacy implementation removed; "
               "use CLI wizard: 'system configuration')");
    return -1;
}
