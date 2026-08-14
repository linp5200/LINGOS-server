/**
 * @file    config_wizard_advanced.c
 * @brief   高级配置向导入口（已合并至核心向导，此文件作为兼容包装）
 * @version LN-B-4.2.0.0
 * @note    实际逻辑已迁移至 src/wizard/wizard_steps.c 和 src/tui/tui_wizard.c
 *          此文件仅保留符号导出，以维持现有 Makefile 和头文件兼容性
 */

#include "config_wizard_advanced.h"
#include "config_wizard_common.h"
#include "log_extra.h"        /* ← 修改：去掉相对路径 */
#include "uart.h"             /* ← 修改：去掉相对路径 */
#include "lang.h"             /* ← 修改：去掉相对路径 */

/**
 * @brief 运行高级配置向导
 * @param ctx 向导上下文
 * @return 0 成功，-1 失败或取消
 */
int run_advanced_wizard(wizard_context_t *ctx) {
    LOG_INFO_T("ConfigWizardAdvanced", "Run", "Enter", "Advanced wizard started (wrapper)");

    if (!ctx) {
        LOG_ERROR_T("ConfigWizardAdvanced", "Run", "Invalid", "ctx is NULL");
        return -1;
    }

    /* 直接调用调度入口，强制高级模式 */
    extern int config_wizard_run_ex(int mode);
    int ret = config_wizard_run_ex(WIZARD_ADVANCED);

    if (ret == 0) {
        LOG_INFO_T("ConfigWizardAdvanced", "Run", "OK", "Advanced wizard completed successfully");
    } else {
        LOG_WARN_T("ConfigWizardAdvanced", "Run", "Fail", "Advanced wizard failed or cancelled (ret=%d)", ret);
    }

    return ret;
}