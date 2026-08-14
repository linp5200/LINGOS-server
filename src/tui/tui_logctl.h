/**
 * @file    tui_logctl.h
 * @brief   TUI 日志自动控制头文件
 * @version LN-B-4.3.0.0
 */

#ifndef TUI_LOGCTL_H
#define TUI_LOGCTL_H

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 挂起日志（进入 TUI 时调用）
 */
void tui_logctl_suspend(void);

/**
 * @brief 恢复日志（退出 TUI 时调用）
 */
void tui_logctl_restore(void);

/**
 * @brief 检查日志是否已挂起
 * @return 1 挂起中，0 未挂起
 */
int tui_logctl_is_suspended(void);

#ifdef __cplusplus
}
#endif

#endif /* TUI_LOGCTL_H */