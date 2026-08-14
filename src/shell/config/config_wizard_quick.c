/**
 * @file    config_wizard_quick.c
 * @brief   快速配置向导入口（已合并至核心向导，此文件作为兼容包装）
 * @version LN-B-4.2.0.0
 * @note    实际逻辑已迁移至 src/wizard/wizard_steps.c 和 src/tui/tui_wizard.c
 *          此文件仅保留符号导出，以维持现有 Makefile 和头文件兼容性
 */

#include "config_wizard_quick.h"
#include "config_wizard_common.h"
#include "log_extra.h"        /* ← 修改：去掉相对路径 */
#include "uart.h"             /* ← 修改：去掉相对路径 */
#include "lang.h"             /* ← 修改：去掉相对路径 */
/**
 * @brief 运行快速配置向导
 * @param ctx 向导上下文
 * @return 0 成功，-1 失败或取消
 */
int run_quick_wizard(wizard_context_t *ctx) {
    LOG_INFO_T("ConfigWizardQuick", "Run", "Enter", "Quick wizard started (wrapper)");

    if (!ctx) {
        LOG_ERROR_T("ConfigWizardQuick", "Run", "Invalid", "ctx is NULL");
        return -1;
    }

    /* 直接调用高级入口，并强制指定快速模式 */
    /* 实际配置逻辑由 wizard_core 和 tui_wizard 接管 */
    extern int config_wizard_run_ex(int mode);
    int ret = config_wizard_run_ex(WIZARD_QUICK);

    if (ret == 0) {
        LOG_INFO_T("ConfigWizardQuick", "Run", "OK", "Quick wizard completed successfully");
    } else {
        LOG_WARN_T("ConfigWizardQuick", "Run", "Fail", "Quick wizard failed or cancelled (ret=%d)", ret);
    }

    return ret;
}