/**
 * @file    src/core/repair_mode.h
 * @brief   修复模式头文件（异常关闭后的恢复选项）
 * @version LN-B-5.1.2.6-rc
 */

#ifndef CORE_REPAIR_MODE_H
#define CORE_REPAIR_MODE_H

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 运行修复模式（检测到异常关闭后调用）
 * @return 0 用户选择继续启动，-1 用户取消/退出
 */
int repair_mode_run(void);

/**
 * @brief 自动执行修复（无需用户交互）
 * @return 0 修复成功，-1 修复失败
 */
int repair_mode_auto(void);

#ifdef __cplusplus
}
#endif

#endif /* CORE_REPAIR_MODE_H */