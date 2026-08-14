/**
 * @file    src/tui/tui_wizard.h
 * @brief   TUI 配置向导渲染器头文件
 * @version LN-B-4.3.0.0
 * @changes 更新声明以匹配 wizard_core 集成
 */

#ifndef TUI_TUI_WIZARD_H
#define TUI_TUI_WIZARD_H

#include "../wizard/wizard_core.h"

/**
 * @brief 运行 TUI 配置向导
 * @param state 向导状态（由 wizard_core_init 初始化）
 * @return 0 成功，-1 失败或取消
 */
int tui_wizard_run(wizard_state_t *state);

#endif /* TUI_TUI_WIZARD_H */