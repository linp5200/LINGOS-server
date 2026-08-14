/**
 * @file    config_wizard.h
 * @brief   配置向导头文件（保持 API 兼容）
 * @version LN-B-3.8.0.0
 */

#ifndef SHELL_CONFIG_WIZARD_H
#define SHELL_CONFIG_WIZARD_H

/* 模式常量与核心层保持一致 */
#include "../wizard/wizard_core.h"

/**
 * @brief 运行配置向导（兼容入口）
 * @param force 1=强制重新配置（高级模式），0=仅在未配置时运行（快速模式）
 * @return 0 配置成功，-1 配置失败或用户取消
 */
int config_wizard_run(int force);

/**
 * @brief 运行配置向导（指定模式）
 * @param mode 模式常量：WIZARD_QUICK=0, WIZARD_ADVANCED=1,
 *             WIZARD_NONINTERACTIVE=2, WIZARD_UNKNOWN=-1（向导内选择）
 * @return 0 配置成功，-1 配置失败或用户取消
 */
int config_wizard_run_ex(int mode);

#endif /* SHELL_CONFIG_WIZARD_H */