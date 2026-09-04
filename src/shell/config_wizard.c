/**
 * @file    src/shell/config_wizard.c
 * @brief   配置向导 Shell 入口（集成 wizard_core 框架，三层回退）
 * @version LN-0.4.3
 * @changes 简化回退调度：移除 is_tui_available() 重复检测，由 tui_wizard_run 内部处理回退；
 *          增加模式检测日志
 */

#include "config_wizard.h"
#include "../common/safe_string.h"
#include "../common/data_path.h"
#include "../common/lang.h"
#include "../lib/log_extra.h"
#include "../drivers/uart.h"
#include "../tui/tui_wizard.h"
#include "../wizard/wizard_core.h"
#include "../wizard/wizard_steps.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

/* ============================================================
 * 内部辅助：注册所有步骤（核心 + 模块）
 * ============================================================ */
static void register_all_steps(wizard_state_t *state) {
    LOG_DEBUG_T("ConfigWizard", "RegisterSteps", "Enter", "registering all steps");

    /* ====== 核心步骤（由 wizard_steps.c 提供） ====== */
    wizard_core_register_step(state, &step_language);
    wizard_core_register_step(state, &step_mode);
    wizard_core_register_step(state, &step_ai_backend);
    wizard_core_register_step(state, &step_start_option);

    LOG_INFO_T("ConfigWizard", "RegisterSteps", "OK", "registered %d steps", state->step_count);
}

/* ============================================================
 * 内部辅助：运行 TUI 向导（失败后由 tui_wizard_run 内部处理回退）
 * ============================================================ */
static int run_tui_wizard(wizard_state_t *state) {
    LOG_INFO_T("ConfigWizard", "RunTUI", "Enter", "starting TUI wizard (will fallback internally if needed)");
    return tui_wizard_run(state);
}

/* ============================================================
 * 内部辅助：运行 CLI 向导（回退模式）
 * ============================================================ */
static int run_cli_wizard(wizard_state_t *state) {
    LOG_INFO_T("ConfigWizard", "RunCLI", "Enter", "starting CLI wizard (fallback)");

    uart_puts(COLOR_YELLOW);
    uart_puts(tr("\n=== Configuration Wizard (CLI Mode) ===\n",
                 "\n=== 配置向导（CLI 模式） ===\n"));
    uart_puts(COLOR_RESET);

    while (state->current_index < state->step_count) {
        wizard_step_t *step = wizard_core_get_current_step(state);
        if (!step) break;

        uart_puts(tr("\nStep: ", "\n步骤："));
        uart_puts(tr(step->title_en, step->title_zh));
        uart_puts("\n");

        if (step->on_enter) {
            step->on_enter(state->ctx);
        }

        uart_puts(tr("Press Enter to confirm, 'q' to quit: ",
                     "按 Enter 确认，'q' 退出："));
        char c = uart_getc();
        uart_putc(c);
        uart_puts("\n");

        if (c == 'q' || c == 'Q') {
            wizard_core_set_cancelled(state);
            LOG_INFO_T("ConfigWizard", "RunCLI", "Quit", "user quit");
            return -1;
        }

        step->state = STEP_STATE_COMPLETED;
        state->completed_count++;
        wizard_core_next_step(state);
    }

    if (state->current_index >= state->step_count) {
        uart_puts(tr("\nAll steps completed. Saving configuration...\n",
                     "\n所有步骤已完成，正在保存配置...\n"));
        if (wizard_core_merge_config(state->ctx) != 0) {
            LOG_ERROR_T("ConfigWizard", "RunCLI", "MergeFail", "config merge failed");
            return -1;
        }
        uart_puts(tr("Configuration saved successfully!\n", "配置保存成功！\n"));
        return 0;
    }

    return -1;
}

/* ============================================================
 * 内部辅助：执行非交互模式
 * ============================================================ */
static int run_noninteractive_wizard(wizard_state_t *state) {
    LOG_INFO_T("ConfigWizard", "NonInteractive", "Enter", "non-interactive mode");

    while (state->current_index < state->step_count) {
        wizard_step_t *step = wizard_core_get_current_step(state);
        if (!step) break;

        LOG_DEBUG_T("ConfigWizard", "NonInteractive", "Step", "executing step '%s'", step->id);

        if (step->on_enter) {
            step->on_enter(state->ctx);
        }
        step->state = STEP_STATE_COMPLETED;
        state->completed_count++;
        wizard_core_next_step(state);
    }

    if (state->current_index >= state->step_count) {
        LOG_INFO_T("ConfigWizard", "NonInteractive", "Complete", "all steps completed");
        if (wizard_core_merge_config(state->ctx) != 0) {
            LOG_ERROR_T("ConfigWizard", "NonInteractive", "MergeFail", "config merge failed");
            return -1;
        }
        return 0;
    }

    return -1;
}

/* ============================================================
 * 公共 API：运行配置向导（三层回退：TUI → CLI → RAW）
 * ============================================================ */
int config_wizard_run_ex(int mode) {
    LOG_INFO_T("ConfigWizard", "RunEx", "Enter", "mode=%d", mode);

    /* 1. 初始化向导状态 */
    wizard_state_t state;
    if (wizard_core_init(&state, mode) != 0) {
        LOG_ERROR_T("ConfigWizard", "RunEx", "InitFail", "wizard_core_init failed");
        return -1;
    }

    /* 2. 注册所有步骤 */
    register_all_steps(&state);

    if (state.step_count == 0) {
        LOG_ERROR_T("ConfigWizard", "RunEx", "NoSteps", "no steps registered");
        return -1;
    }

    LOG_DEBUG_T("ConfigWizard", "RunEx", "Steps", "registered %d steps", state.step_count);

    /* 3. 根据模式执行（三层回退：TUI → CLI → RAW） */
    int ret = -1;

    if (mode == WIZARD_NONINTERACTIVE) {
        ret = run_noninteractive_wizard(&state);
    } else if (mode == WIZARD_QUICK) {
        /* 快速模式直接使用 CLI（无 TUI 依赖） */
        ret = run_cli_wizard(&state);
    } else {
        /* 高级模式：优先 TUI，失败回退到 CLI，CLI 内部可回退到 RAW */
        LOG_DEBUG_T("ConfigWizard", "RunEx", "Mode", "advanced mode, trying TUI first");
        ret = run_tui_wizard(&state);
        if (ret != 0) {
            LOG_WARN_T("ConfigWizard", "RunEx", "TUIFallback", "TUI failed, falling back to CLI");
            ret = run_cli_wizard(&state);
        }
    }

    /* 4. 清理 */
    if (state.ctx) {
        free(state.ctx);
        state.ctx = NULL;
    }

    LOG_INFO_T("ConfigWizard", "RunEx", "Exit", "ret=%d", ret);
    return ret;
}

/* ============================================================
 * 兼容旧接口
 * ============================================================ */
int config_wizard_run(int force) {
    (void)force;
    return config_wizard_run_ex(WIZARD_QUICK);
}